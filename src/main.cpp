// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

// main.cpp —— SKSE 插件入口
// 独立雪地变形插件：Phase 1 = 变形图数据层（滚动窗口 + 印章 + 计算着色器）
// 与 ENB 共存：本插件不替换引擎着色器、不注入几何，只维护数据纹理。
//
// 每帧驱动：hook IDXGISwapChain::Present（vtable slot 8），这是独立 SKSE 渲染插件
// 的标准做法，与 ENB 的 Present hook 共存无冲突（ENB 也 hook Present，链式调用）。
//
// 注意：必须先包含 CommonLib 头（RE/SKSE），再包含本地头（DeformationMap.h 带
// d3d11.h 等 Windows API 头）——REX::W32 强制要求这个顺序。

#include <RE/Skyrim.h>
#include <RE/N/NativeFunction.h>  // RegisterFunction 模板需要完整定义
#include <SKSE/SKSE.h>

#include <Windows.h>  // VirtualProtect / DWORD（须在 CommonLib 之后）

#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>

#include "DeformationMap.h"
#include "MeshScanner.h"
#include "SnowShellMesh.h"
#include "StampCollector.h"
#include "CoreAPI.h"  // v543：DS 插件化——DynamicShader Core API（渲染回调/调度/天气）

#include <spdlog/sinks/basic_file_sink.h>

#define DLLEXPORT __declspec(dllexport)

namespace
{
	// ---- Papyrus 全局函数：SnowDeformScan <radius> ----
	// 控制台输入 SnowDeformScan 500 → 扫描玩家周围 500 单位内的 kSnow 网格，
	// 结果写日志 + 控制台。用于确定 Phase 2A 的目标网格范围。
	bool PapyrusSnowDeformScan(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*, float a_radius)
	{
		[[maybe_unused]] auto* vm = a_vm;
		[[maybe_unused]] auto  stack = a_stackID;
		SKSE::log::info("SnowDeformScan called: radius={}", a_radius);
		const auto entries = SnowDeform::MeshScanner::Scan(a_radius);
		SnowDeform::MeshScanner::LogSummary(entries);
		return true;
	}

	// ---- Present hook：每帧驱动变形图更新 ----
	using PresentFunc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	PresentFunc g_originalPresent = nullptr;
	std::atomic<std::uint32_t> autoCreateAt{0};  // v569：跨线程（渲染线程读 / 游戏线程读档写）改 atomic
	std::atomic<bool> autoCreateDone{false};        // v449b：提为文件级（读档后可重置）

	SnowDeform::FrameInput GetFrameInput();

	// v543（DS 插件化）：**渲染回调（由 DynamicShader Core 的 Present hook 每帧调用，
	// 渲染线程）**——原 PresentHook 的 swapchain 链式调用由 Core 负责，插件只做变形。
	void RenderCB(ID3D11DeviceContext* a_ctx, void*)
	{
		(void)a_ctx;  // v567：C4100——UpdateLandscape 内部自取 renderer ctx
		// v406：**性能检测**——每帧总耗时 + 各阶段耗时（渲染线程），每 2 秒汇总。
		// 判断依据：frame avg >16.7ms(60fps) = 掉帧；land 含 RebuildField（盖章帧重）；
		// env/nrm 限频烘焙（盖章帧重，平时 ~0）；max 值定位卡顿峰值。
		using clk = std::chrono::steady_clock;
		static long long sFrameUs = 0, sLandUs = 0;
		static long long sFrameMaxUs = 0, sLandMaxUs = 0;
		static int sFrames = 0;
		static auto sDiagT = clk::now();
		auto usBetween = [](clk::time_point a, clk::time_point b) {
			return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
		};

		const auto tF0 = clk::now();

		// 每帧：驱动变形图更新（在渲染之前，确保变形图数据是本帧最新）
		SnowDeform::GetDeformationMap().Update(GetFrameInput());

		// v129：真地形变形（LANDSCAPE）——每帧改真实地形顶点
		// v426（0.5 分支）：**纯地形版本**——只保留 LANDSCAPE 真地形变形
		// （v304 回滚雪壳后的稳定态 = 用户喜欢的 0.5 方向）；v430 雪壳死代码已清。
		if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
			auto ctx = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
			const auto t1 = clk::now();
			SnowDeform::GetSnowShellMesh().UpdateLandscape(ctx);
			const auto t3 = clk::now();
			// v444：动态视差已关闭（用户拍板——ENB wrap 采样 170.7 周期重复无法消除，
			// 聚焦动态法线柔化阴影）
			// SnowDeform::GetSnowShellMesh().UpdateDynamicParallax(ctx);
			sLandUs += usBetween(t1, t3);
			sLandMaxUs = std::max(sLandMaxUs, usBetween(t1, t3));
		}
		const auto tF1 = clk::now();
		sFrameUs += usBetween(tF0, tF1);
		sFrameMaxUs = std::max(sFrameMaxUs, usBetween(tF0, tF1));
		sFrames++;
		if (usBetween(sDiagT, clk::now()) >= 2'000'000) {
			const double fAvg = static_cast<double>(sFrameUs) / 1000.0 / sFrames;
			SKSE::log::info("v406-fps: frame={:.1f}ms({:.0f}fps) max={:.1f} land={:.2f}(max {:.1f})",
				fAvg, fAvg > 0.0 ? 1000.0 / fAvg : 0.0,
				static_cast<double>(sFrameMaxUs) / 1000.0,
				static_cast<double>(sLandUs) / 1000.0 / sFrames,
				static_cast<double>(sLandMaxUs) / 1000.0);
			sFrameUs = sLandUs = 0;
			sFrameMaxUs = sLandMaxUs = 0;
			sFrames = 0;
			sDiagT = clk::now();
		}
	}

	// 组装本帧输入（印章收集 + 窗口跟随 + 回填）
	SnowDeform::FrameInput GetFrameInput()
	{
		SnowDeform::FrameInput input{};

		// 热键调试：F9 = 切换雪壳网格（创建/销毁 + 顶点刷新）
		// F10 = 扫描玩家周围 500 单位 kSnow 网格
		// 注意：必须在游戏主线程执行（TaskInterface）——渲染线程访问游戏数据（REF/NiAVObject 树）
		// 线程不安全，直接执行会闪退（实测踩坑）。
		// 节流 500ms：防连按导致 TaskInterface 任务排队刷屏（v14）
		static std::uint32_t lastF9Time = 0;
		static std::uint32_t lastF10Time = 0;
		const auto now = GetTickCount();

		static bool f9Pressed = false;
		if (GetAsyncKeyState(VK_F9) & 0x8000) {
			if (!f9Pressed && now - lastF9Time > 500) {
				lastF9Time = now;
				SKSE::log::info("F9 pressed: creating snow shell");
				SKSE::GetTaskInterface()->AddTask([]() {
					auto& shell = SnowDeform::GetSnowShellMesh();
					// v129：真地形——F9 直接缓存玩家 cell 的 landscape（不再创建雪壳假模型）
					shell.FindLandscape();
					// v434：零替换（v284b 架构），不再 BuildHighResMesh
					shell.DebugLandscape();
				});
			}
			f9Pressed = true;
		} else {
			f9Pressed = false;
		}

		static bool f10Pressed = false;
		if (GetAsyncKeyState(VK_F10) & 0x8000) {
			if (!f10Pressed && now - lastF10Time > 500) {
				lastF10Time = now;
				SKSE::log::info("SnowDeformScan triggered by F10");
				SKSE::GetTaskInterface()->AddTask([]() {
					const auto entries = SnowDeform::MeshScanner::Scan(500);
					SnowDeform::MeshScanner::LogSummary(entries);
				});
			}
			f10Pressed = true;
		} else {
			f10Pressed = false;
		}

		// v77：自动创建（kDataLoaded 后，免按 F9）
		// v426（0.5 分支）：**纯地形版本**——只 FindLandscape（真地形变形），
		// 禁用雪壳 Initialize（v304 回滚雪壳后的稳定态 = 用户喜欢的 0.5 方向）。
		// v434：**撤销 v432 替换恢复**——v433-dbg 实锤：替换后引擎持续重建
		// （SmoothTerrain 竞争）→ rd 地址 4 秒变两次 → 缓存 geom 与引擎渲染
		// geom 分裂 → 改对数据传错对象（rawMinZ 变了但视觉没坑 + 用户"闪"）。
		// 回 v284b 零替换架构（= 0.5 v296 本质）：SmoothTerrain 提供 129² 引擎
		// 网格，直接改它的顶点（零替换零闪）。覆盖不足由 SmoothTerrain.ini
		// iSmoothedQuads 调大解决（3→6，3×3 cells 视野全 129²）。
		// v544：**失败自动重试（用户"进游戏不是主动触发，要按F9"实锤：21:27:44
		// auto-create 触发但 v130 no landscape geoms found = 进游戏 15 秒场景
		// 未就绪/室内无地形 → 一次性标志 autoCreateDoneLocal=true 永久挡死 →
		// 只能手动 F9）。改为：未成功前每 5 秒重试 FindLandscape，直到
		// IsLandReady()（成功）或玩家手动 F9。读档后 autoCreateDone 重置继续。**
		if (autoCreateAt != 0 && now >= autoCreateAt && !autoCreateDone) {
			static std::uint32_t lastAutoRetry = 0;
			if (now - lastAutoRetry >= 5000) {
				lastAutoRetry = now;
				SKSE::log::info("SnowShellMesh: auto-create retry (landscape only)");
				SKSE::GetTaskInterface()->AddTask([]() {
					auto& shell = SnowDeform::GetSnowShellMesh();
					shell.FindLandscape();
					if (shell.IsLandReady())
						autoCreateDone = true;  // 成功 → 停止重试
				});
			}
		}

		// 相机位置（变形窗口跟随相机）
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			const auto pos = player->GetPosition();
			input.windowOriginX = pos.x;
			input.windowOriginY = pos.y;
			// v63：每帧记录玩家位置（雪壳网格 GPU 顶点更新用）
			SnowDeform::GetSnowShellMesh().UpdatePlayerPos(pos);
		}

		// v565：死分支删除——v434 零替换架构（Smooth Terrain 129² 直接改顶点），
		// BuildHighResMesh 无调用者 → highResBuildQueued 恒 false → 本分支永不执行。
		// v185：窗口 texel-snapped 滚动（CS SnowDeformation 借鉴）——
		// 窗口原点按 texel 取整步进，ScrollDelta 让计算着色器滚动旧数据，
		// 保证变形图跟随相机且不采样模糊。
		{
			const float texel = SnowDeform::kWorldSize / static_cast<float>(SnowDeform::kTextureDim);
			static bool originInit = false;
			static float lastOriginX = 0.0f;
			static float lastOriginY = 0.0f;
			if (!originInit) {
				originInit = true;
				lastOriginX = input.windowOriginX;
				lastOriginY = input.windowOriginY;
			}
			const int32_t sdx = static_cast<int32_t>(
				std::floor((input.windowOriginX - lastOriginX) / texel + 0.5f));
			const int32_t sdy = static_cast<int32_t>(
				std::floor((input.windowOriginY - lastOriginY) / texel + 0.5f));
			// 窗口原点对齐 texel（防累积漂移）
			input.windowOriginX = lastOriginX + static_cast<float>(sdx) * texel;
			input.windowOriginY = lastOriginY + static_cast<float>(sdy) * texel;
			lastOriginX = input.windowOriginX;
			lastOriginY = input.windowOriginY;
			input.scrollDeltaX = sdx;
			input.scrollDeltaY = sdy;
		}

		// 收集印章
		static SnowDeform::StampCollector collector;
		SnowDeform::StampData stamps[SnowDeform::kMaxStamps]{};
		SnowDeform::StampData stampEnds[SnowDeform::kMaxStamps]{};
		const uint32_t count = collector.Collect(
			input.windowOriginX, input.windowOriginY, SnowDeform::kWorldSize,
			stamps, stampEnds, SnowDeform::kMaxStamps);

		input.stampCount = count;
		for (uint32_t i = 0; i < count; i++) {
			input.stamps[i] = { stamps[i].x, stamps[i].y, stamps[i].depth, stamps[i].radius };
			input.stampEnds[i] = { stampEnds[i].x, stampEnds[i].y, 0.0f, 0.0f };
		}

		// 回填参数（Phase 1：固定速率；Phase 2 接入天气检测）
		input.refillAmount = 0.001f;

		// 首帧清空 + 窗口滚动（v185：texel-snapped 滚动已在上面计算）
		static bool firstFrame = true;
		input.clearMap = firstFrame;
		firstFrame = false;

		return input;
	}

	// v543（DS 插件化）：**连接 DynamicShader Core**（kDataLoaded 后）——
	// 初始化变形图 + LoadLibrary 拿 DS_API + 注册渲染回调（Present hook 由 Core 负责，
	// 插件不再自行 hook swapchain）
	void ConnectCore()
	{
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;
		auto& rt = renderer->GetRuntimeData();

		// 初始化变形图（REX::W32 接口，须 reinterpret_cast）
		auto device = reinterpret_cast<ID3D11Device*>(rt.forwarder);
		auto context = reinterpret_cast<ID3D11DeviceContext*>(rt.context);
		if (device && context) {
			if (!SnowDeform::GetDeformationMap().Initialize(device, context)) {
				SKSE::log::error("DynamicSnow: failed to init deformation map");
				return;
			}
			SKSE::log::info("DynamicSnow: deformation map initialized ({}x{}, world size {})",
				SnowDeform::kTextureDim, SnowDeform::kTextureDim, SnowDeform::kWorldSize);
		}

		// Core 连接（LoadLibrary 强制——即使 SKSE 加载顺序中 Core 在后也能找到）
		HMODULE core = LoadLibraryA("DynamicShader.dll");
		if (!core) {
			SKSE::log::error("DynamicSnow: DynamicShader Core not loaded — plugin disabled");
			return;
		}
		auto getApi = reinterpret_cast<DynamicShader::DS_API* (*)(std::uint32_t)>(
			GetProcAddress(core, "DynamicShader_GetAPI"));
		if (!getApi) {
			SKSE::log::error("DynamicSnow: Core GetAPI export missing");
			return;
		}
		auto* api = getApi(DynamicShader::DS_API_VERSION);
		if (!api) {
			SKSE::log::error("DynamicSnow: Core API version mismatch (need {})", DynamicShader::DS_API_VERSION);
			return;
		}
		if (api->RegisterRenderCallback(RenderCB, nullptr)) {
			SKSE::log::info("DynamicSnow: render callback registered to Core");
		} else {
			SKSE::log::error("DynamicSnow: register render callback failed");
		}
	}
}

// 现代 CommonLibSSE-NG 约定：SKSEPlugin_Version + SKSEPlugin_Load
// 版本声明（2026-08-19 实测踩坑后的最终结论）：
//   UsesAddressLibrary() + UsesUpdatedStructs()，不加 UsesNoStructs()、不声明 CompatibleVersions。
// 关键：UsesNoStructs() 写入 versionIndependenceEx 字段（offset 0x304，CommonLibSSE-NG 4.x 新增），
//   SKSE 2.2.6 不识别该字段会按旧偏移读取 → 数据错位 → "fatal error occurred while loading plugin"。
//   CS 官方版能用 UsesNoStructs 是因为其编译用 CommonLibSSE-NG 版本与 SKSE 对齐；
//   本地 extern（GitHub 开发源码）编译必须避开它。
// 不声明 CompatibleVersions = 不锁游戏版本，配合 ENABLE_SKYRIM_SE/AE/VR 三开编译
// （HAS_SKYRIM_MULTI_TARGETING 多版本运行时适配）+ Address Library 地址解析，
// 可通吃 1.5.97 / 1.6.x（1.6.1130/1170/1179 等）各版本游戏本体。
extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName("DynamicSnow");
	v.PluginVersion(1);
	v.UsesAddressLibrary();
	v.UsesUpdatedStructs();
	return v;
}();

// SKSEPlugin_Query：SKSE 2.2.6 加载插件时的必需导出（与 SmoothTerrain/CS 一致）
extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = SKSEPlugin_Version.pluginName;
	a_info->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	// SKSE::Init 内部已自动初始化 spdlog 日志（CommonLibSSE-NG 4.x）：
	// 按插件名创建 {PluginName}.log 并注册 "global" logger。
	// 切勿再手动 spdlog::basic_logger_mt("global", ...) —— 与内置 logger 重名，
	// 注册表冲突抛异常 → 未捕获 → SKSE "fatal error occurred while loading plugin"（实测踩坑）。
	SKSE::Init(a_skse);

	SKSE::log::info("DynamicSnow loaded");


	// v573：INI 配置加载（MaxFootprints 玩家可调——雪堆保留量/帧数权衡）
	SnowDeform::GetSnowShellMesh().LoadConfig();

	// v196：hook 引擎 BuildQuadTriShape 调用点（加载阶段即可——函数地址固定）——
	// 引擎每次构建/重建 quad 网格时我们立即 setMesh 换成高密度网格（SmoothTerrain
	// 方式），根治"F9 事后替换只 2 块生效"（引擎渲染管线缓存旧引用）。
	SnowDeform::GetSnowShellMesh().InstallQuadBuildHook();

	// 注册 Papyrus 全局函数（控制台可调用）
	if (SKSE::GetPapyrusInterface()->Register([](RE::BSScript::IVirtualMachine* a_vm) {
			a_vm->RegisterFunction("SnowDeformScan", "SnowDeformNative", PapyrusSnowDeformScan);
			return true;
		})) {
		SKSE::log::info("DynamicSnow: Papyrus SnowDeformScan registered");
	} else {
		SKSE::log::error("DynamicSnow: failed to register Papyrus functions");
	}

	// 在 DataLoaded 后连接 Core（此时 Renderer/D3D11 已就绪）——Present hook 由
	// DynamicShader Core 负责，插件只注册渲染回调（RenderCB）
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
		if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
			SKSE::log::info("DynamicSnow: data loaded, connecting Core");
			ConnectCore();
			// v559：安装投射物命中 hook（箭矢 + 法术爆炸雪地效果）——vtable 替换，
			// DataLoaded 后安全（Address Library 已就绪）
			SnowDeform::GetSnowShellMesh().InstallProjectileHook();
			// v80：自动创建延迟 3 秒 → 15 秒（v78/v79 在加载后 3 秒自动创建时崩——
			// 场景流式加载高峰期引擎状态可能未就绪；手动 F9 从未在加载时崩过）
			autoCreateAt = GetTickCount() + 15000;
		}
		// v449b：**读档/新游戏复位（F9 快速读档闪退修复）**——引擎卸载全部地形，
		// cells/geom 缓存悬空 → 下一帧 UpdateLandscape 写崩（vmovdqa [rcx] AV 实锤）。
		// 清空全部状态 + 重新调度 FindLandscape（15 秒兜底 + 立即试一次）。
		else if (msg->type == SKSE::MessagingInterface::kPreLoadGame ||
			msg->type == SKSE::MessagingInterface::kPostLoadGame ||
			msg->type == SKSE::MessagingInterface::kNewGame ||
			msg->type == SKSE::MessagingInterface::kDeleteGame) {
			SKSE::log::info("SnowDeformationPlugin: load event type={}, resetting cache", msg->type);
			SnowDeform::StampCollector::ClearPrevPositions();  // v569：读档清胶囊轨迹缓存（旧 formID 复用）
			SnowDeform::GetSnowShellMesh().ResetForLoadGame();
			if (msg->type == SKSE::MessagingInterface::kPostLoadGame ||
				msg->type == SKSE::MessagingInterface::kNewGame) {
				autoCreateDone = false;
				autoCreateAt = GetTickCount() + 15000;
				// 立即试一次（读档地形可能已就绪）
				SKSE::GetTaskInterface()->AddTask([]() {
					SnowDeform::GetSnowShellMesh().FindLandscape();
				});
			}
		}
	});

	return true;
}
