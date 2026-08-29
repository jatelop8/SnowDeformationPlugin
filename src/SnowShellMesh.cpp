// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

// SnowShellMesh.cpp —— 程序化雪壳网格（Phase 2 核心）
// 模板 NIF（NiNode + NiTriShape + NiTriShapeData）经 NiStream::Load3 加载：
//   引擎 NIF 加载器创建 rendererData（GPU 渲染数据）→ 网格真正可渲染
//   （手工 new BSDynamicTriShape 缺 rendererData → 引擎不渲染，此路已死）
// 每帧改 NiGeometryData::vertex（CPU 顶点数组，引擎分配）+ dirtyFlags 标记重上传。
// 顶点写世界坐标，挂玩家 3D 的父节点（场景根）→ 玩家移动网格跟随、第一人称可见。
// 线程：所有操作必须在游戏线程（TaskInterface / 帧回调）。

#include "SnowShellMesh.h"
#include "NiUtils.h"

#include <SKSE/SKSE.h>

#include <Windows.h>  // 须在 CommonLib 之后（REX::W32 强制）
#include <d3d11.h>
// v443：Terrain Helper 接入（hook BSLightingShader vtable 槽 4 + SetPSTexture）
#include <RE/B/BSLightingShader.h>
// v558q：BSTempEffectParticle include 已移除（粒子特效全部删除）
#include <RE/Offsets_VTABLE.h>
// v559：投射物命中 hook（箭矢 + 法术爆炸）——AddImpact vtable 槽 0xBD
#include <RE/P/Projectile.h>
#include <RE/M/MissileProjectile.h>
#include <RE/A/ArrowProjectile.h>
// v560：动物脚印（ProcessLists 遍历 + Actor + Race 人形排除）
#include <RE/P/ProcessLists.h>
#include <RE/A/Actor.h>
#include <RE/T/TESRace.h>
#include <RE/B/BSContainer.h>
// v562：v561 贴花 include（BSTempEffectSimpleDecal/BGSTextureSet/NiPlane/TESDataHandler）已移除

#include <fstream>
#include <algorithm>  // v587：std::sort（最低节点兜底排序）
#include <filesystem>  // v443：LoadBC4DDS Data 目录拼接
#include <functional>
#include <vector>
#include <array>
#include <cmath>
#include <cstdlib>  // v573：atoi（INI 解析）
#include <cctype>  // v206：雪分类 tolower
#include <cstring>
#include "snow_heights.inc"  // v377: 真实雪粒高度基底（Displacement 高通 256²）
#include "melting_grain.inc"  // v402: Poly Haven melting_snow_uexjbdudy 4K Bump 高度图（1024²，EnvMask A 视差基底）
#include <bit>     // v338：std::bit_cast（float→half）
#include <limits>  // v338：std::numeric_limits（halfToFloat）

namespace SnowDeform
{
	namespace
	{
		constexpr const char* kMeshPath = "Data\\meshes\\snowdeformation\\snow.nif";

		// Skyrim cell 尺寸：4096×4096 单位（4 quadrant × 2048）。v130 3×3 cell
		// 缓存用——probe = 玩家位置 + dx/dy×kCellSize 落进相邻 cell。
		constexpr float kCellSize = 4096.0f;

		// v154：SEH 保护辅助函数（独立 C 风格，无对象展开 → __try 可用）。
		// geom 可能被引擎 LOD 重建（悬空）——SafeGeomValid 读 vertexCount 对比缓存值，
		// SafeUpload 保护 UpdateSubresource（崩溃日志实锤：nvwgf2umx rep movsb 读非法地址）
		[[nodiscard]] bool SafeGeomValid(RE::BSTriShape* a_g, std::uint32_t a_cachedVc, std::uint32_t& a_curVc)
		{
			__try {
				a_curVc = a_g->GetTrishapeRuntimeData().vertexCount;
				return a_curVc == a_cachedVc;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		void SafeUpload(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_vb, const void* a_src, std::uint32_t a_size)
		{
			__try {
				a_ctx->UpdateSubresource(a_vb, 0, nullptr, a_src, a_size, 0);
			} __except(EXCEPTION_EXECUTE_HANDLER) {
			}
		}
		// v186：SEH 保护的手工构造 NiSourceTexture + SetTexture 替换（C 风格 helper，
		// 无对象展开 → __try 可用）。NiSourceTexture 0x58{rendererTexture@0x48}，
		// BSGraphics::Texture 0x28{texture@0,resourceView@0x10}，_refCount@0x08。
		// 地形材质 = TESLandTexture::textureSet（v184 链路），ENB TerrainParallax
		// 读新 diffuse 的 alpha → 像素级粉雪视差。
		bool SafeInstallDynamicDiffuse(RE::BSTextureSet* a_ts, ID3D11Texture2D* a_tex,
			ID3D11ShaderResourceView* a_srv, RE::NiSourceTexture*& a_outNewTex)
		{
			a_outNewTex = nullptr;
			__try {
				auto* bgTex = RE::malloc<RE::BSGraphics::Texture>();
				std::memset(bgTex, 0, sizeof(RE::BSGraphics::Texture));
				bgTex->texture = a_tex;
				bgTex->resourceView = a_srv;
				auto* niTex = RE::malloc<RE::NiSourceTexture>();
				std::memset(niTex, 0, sizeof(RE::NiSourceTexture));
				*reinterpret_cast<std::uintptr_t*>(niTex) = RE::NiSourceTexture::VTABLE[0].address();
				niTex->_refCount = 1;
				niTex->rendererTexture = bgTex;
				a_outNewTex = niTex;  // 持有裸指针（_refCount=1，泄漏不释放——调试期安全）
				// SetTexture 是 BSTextureSet 虚函数 26（this + Texture + NiSourceTexturePtr&）。
				// 不走 C++ 引用（NiPointer 析构触发 C2712），裸 vtable 调用传指针值。
				using SetTexFn = void(__thiscall*)(RE::BSTextureSet*, std::uint32_t, RE::NiSourceTexture*);
				const auto vt = *reinterpret_cast<std::uintptr_t**>(a_ts);
				const auto fn = reinterpret_cast<SetTexFn>(vt[26]);
				fn(a_ts, static_cast<std::uint32_t>(RE::BSTextureSet::Textures::kDiffuse), niTex);
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		// v364：**通用动态纹理安装**（v186 helper 改造：任意 slot）——手工构造
		// BSGraphics::Texture + NiSourceTexture + BSTextureSet 虚函数 26 SetTexture
		// 替换指定槽位（盒子 EnvMask=kEnvironmentMask=2）。
		bool SafeInstallDynamicTex(RE::BSTextureSet* a_ts, std::uint32_t a_slot,
			ID3D11Texture2D* a_tex, ID3D11ShaderResourceView* a_srv,
			RE::NiSourceTexture*& a_outNewTex)
		{
			a_outNewTex = nullptr;
			__try {
				auto* bgTex = RE::malloc<RE::BSGraphics::Texture>();
				std::memset(bgTex, 0, sizeof(RE::BSGraphics::Texture));
				bgTex->texture = a_tex;
				bgTex->resourceView = a_srv;
				// v373：**NiSourceTexture 改用 placement new 默认构造**（v372 install failed 根因）——
				// memset 半吊子构造（vtable/refcount/rendererTexture 外全 0）→ SetTexture 内部
				// 访问 formatPrefs/name/prev/next 等未初始化字段 → AV（vt[26] 有效实锤：槽位/
				// 签名都对，崩在对象不完整）。placement new 调引擎默认构造 = 完整 vtable +
				// formatPrefs + name 初始化；再手动补 prev/next=null + rendererTexture。
				auto* niTex = RE::malloc<RE::NiSourceTexture>();
				new (niTex) RE::NiSourceTexture();  // 默认构造（vtable/成员完整）。C4291：placement new
				// 无匹配 delete 属预期——对象所有权归引擎引用计数（NiRefObject）。
				niTex->prev = nullptr;   // 纹理链表指针（默认构造不初始化 → 防引擎遍历崩）
				niTex->next = nullptr;
				niTex->unk40 = nullptr;  // BSResource::Stream*（无流）
				niTex->rendererTexture = bgTex;
				a_outNewTex = niTex;
				// SetTexture 第二参数 NiSourceTexturePtr&（指针变量引用）→ 传 &niTex
				using SetTexFn = void(__thiscall*)(RE::BSTextureSet*, std::uint32_t, RE::NiSourceTexture**);
				const auto vt = *reinterpret_cast<std::uintptr_t**>(a_ts);
				const auto fn = reinterpret_cast<SetTexFn>(vt[26]);
				fn(a_ts, a_slot, &niTex);
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				// v373：异常码 + RIP 诊断（定位崩溃指令）
				__try {
					const auto vt2 = *reinterpret_cast<std::uintptr_t**>(a_ts);
					SKSE::log::error("v373: install failed ts={} vt={} vt[26]={} slot={}",
						static_cast<void*>(a_ts), static_cast<void*>(vt2),
						static_cast<void*>(vt2 ? reinterpret_cast<void*>(vt2[26]) : nullptr), a_slot);
				} __except(EXCEPTION_EXECUTE_HANDLER) {
					SKSE::log::error("v373: install failed + vtable unreadable ts={}", static_cast<void*>(a_ts));
				}
				return false;
			}
		}
		// v181：粉雪噪声——稳定 2D value noise（整数 hash + smoothstep 插值）。
		// 世界坐标驱动 → 顶点固定噪声值不闪烁；粉雪效果：
		//  边缘坍塌（低频扰动 SDF 边界 → 毛边/碎裂）、坑内碎雪（高频 ±2.5）、
		//  雪脊蓬松（幅度 0.6~1.4 随机）。返回 0..1。
		[[nodiscard]] float PowderNoise(float a_x, float a_y)
		{
			const auto ih = [](int a, int b) -> float {
				unsigned int h = static_cast<unsigned int>(a) * 374761393u +
					static_cast<unsigned int>(b) * 668265263u;
				h = (h ^ (h >> 13)) * 1274126177u;
				return static_cast<float>(h & 0x00FFFFFF) / 16777215.0f;
			};
			const int ix = static_cast<int>(std::floor(a_x));
			const int iy = static_cast<int>(std::floor(a_y));
			const float fx = a_x - static_cast<float>(ix);
			const float fy = a_y - static_cast<float>(iy);
			const float sx = fx * fx * (3.0f - 2.0f * fx);
			const float sy = fy * fy * (3.0f - 2.0f * fy);
			const float v00 = ih(ix, iy);
			const float v10 = ih(ix + 1, iy);
			const float v01 = ih(ix, iy + 1);
			const float v11 = ih(ix + 1, iy + 1);
			const float a = v00 + (v10 - v00) * sx;
			const float b = v01 + (v11 - v01) * sx;
			return a + (b - a) * sy;
		}
		// v205：沙丘起伏（Undulation）——Josef SnowShell 移植。世界锚定双八度
		// value noise（340 低频 0.72 + 110 高频 0.28），±kUndulationAmp 单位，
		// 深度缩放（saturate(剩余雪深/8)）：深坑底/薄雪处平坦，雪面自然起伏。
		// 复用 v181 PowderNoise（稳定 2D value noise，世界坐标驱动不闪烁）。
		constexpr float kUndulationAmp = 3.5f;
		[[nodiscard]] float UndulationXY(float a_wx, float a_wy)
		{
			const float n = PowderNoise(a_wx / 340.0f, a_wy / 340.0f) * 0.72f +
				PowderNoise(a_wx / 110.0f, a_wy / 110.0f) * 0.28f;
			return (n - 0.5f) * 2.0f * kUndulationAmp;
		}
		// v205：回填速率——48 单位深度 / (700 秒 × 60fps) ≈ 每帧 +0.00114（Josef RefillTime）
		// v398：**回填 10s→1 天（用户：\"回弹基础最起码需要一天，哪有刚走过就回弹\"）**——
		// v390 为修\"轨迹塌陷\"把 700s 砍到 10s = 15 秒回弹太假。真实雪地脚印保持
		// 很久。回 1 天（86400s）：每 2 秒回填 2.3e-5，1.5 深度 ~1.5 天回完。
		// stamps 上限同步放大 200→3000（回填慢 → 脚印长期积累，200 上限会导致走
		// 200 步后最早脚印 erase 突变消失）。轨迹塌陷问题由 RebuildField 增量重建
		// 机制兜底（脚印列表 erase 才从场消失，回填是渐进自然消失）。
		constexpr float kRefillPerFrame = 1.0f / (86400.0f * 60.0f);
		constexpr float kSnowDepth = 18.0f;  // v449：26→18（用户"凹槽太深违和"——v440 观感 deepest≈-14；雪堆 12 < 坑 18 比例 0.67 不盖坑）。v568：v442b"18→26"注释过时（v449 已改回 18）
		// v206：雪分类（Josef TerrainData ClassifySnowClass 移植）——diffuse 文件名子串
		// 匹配雪关键字（snow01/snow02/snowpath/grasssnow/snowrocks），回退 materialType
		//（kSnow/kSnowStairs）。TESLandTexture 链路 v184/v186 实锤：
		// LoadedLandData::quadTextures[4][6] → TESLandTexture::textureSet → diffuse 路径。
		// v445：**扩展三态分类（用户"雪地/沙地/泥地有效，其他材质完全无效"）**——
		// 返回 0=其他（岩石/草地/道路/石头等）1=雪 2=沙 3=泥。
		[[nodiscard]] int ClassifyLandTexture(RE::TESLandTexture* a_lt)
		{
			if (!a_lt || a_lt->formID == 0)
				return 0;
			if (auto* ts = a_lt->textureSet) {
				if (auto* path = ts->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					std::string lower(path);
					std::transform(lower.begin(), lower.end(), lower.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					// 雪类（Josef kSnowClasses 前 5 类；v446：**删 ice/frost/glacier**——
					// 用户"去掉 ice/frost"，冰面/霜地不再算雪 → 回 0 其他）
					static constexpr const char* kSnowKeywords[] = {
						"snow01", "snow02", "snowpath", "grasssnow", "snowrocks",
						"snowstone", "snowslope"
					};
					for (const char* kw : kSnowKeywords) {
						if (lower.find(kw) != std::string::npos)
							return 1;
					}
					// 沙类
					static constexpr const char* kSandKeywords[] = {
						"sand", "desert", "dune", "beach"
					};
					for (const char* kw : kSandKeywords) {
						if (lower.find(kw) != std::string::npos)
							return 2;
					}
					// 泥类（v446：**删纯 dirt**——用户"去掉纯 dirt"，dirt01/02 不再算泥；
					// v446b：补 swamp/marsh/bog——Morthal 沼泽贴图常见命名）
					static constexpr const char* kMudKeywords[] = {
						"mud", "muddy", "soil", "peat", "swamp", "marsh", "bog"
					};
					for (const char* kw : kMudKeywords) {
						if (lower.find(kw) != std::string::npos)
							return 3;
					}
				}
			}
			// v449：**删 materialType 回退（材质检测失效根因）**——Skyrim 大量非雪
			// 地形贴图（岩石/草地，Vanaheimr 改名后文件名不含 snow）的物理材质
			// materialID 就是 kSnow（脚步音效用）→ 回退把它们全判成雪 → 全材质都
			// 变形（用户"材质检测没成功"实锤）。**只信文件名关键字**（雪/沙/泥）。
			return 0;
		}
		// quad 材质分类：base(defQuadTextures) + 6 层——任一层命中（雪>沙>泥优先级）
		// 返回该类；全无 → 0（其他材质，不变形）
		[[nodiscard]] int QuadSurfaceClass(RE::TESObjectLAND::LoadedLandData* a_ld, int a_q)
		{
			if (!a_ld)
				return 0;
			int best = 0;
			if (a_q >= 0 && a_q < 4) {
				const int c = ClassifyLandTexture(a_ld->defQuadTextures[a_q]);
				if (c > best) best = c;
			}
			if (a_q < 0 || a_q >= 4)
				return best;
			for (int li = 0; li < 6; li++) {
				const int c = ClassifyLandTexture(a_ld->quadTextures[a_q][li]);
				if (c > best) best = c;
			}
			return best;
		}
		// v155：mesh[q].child[0]（真正渲染网格）完整结构诊断——纯读 + SEH 保护。
		// v152 直接缓存 mesh.child 进游戏即崩 → 本版不缓存不写，只打印结构：
		// verts/stride(vb ByteWidth/vc)/rendererData/vb/raw/worldT@0x07C，
		// 确认它是否可安全操作（raw/vb 有效、worldT 正确），为 v156 改它铺路。
		void SafeDumpMeshChild(int a_q, RE::BSTriShape* a_g)
		{
			__try {
				if (!a_g) {
					SKSE::log::info("v155: mesh[{}] child=null", a_q);
					return;
				}
				auto& rtd = a_g->GetGeometryRuntimeData();
				const auto vc = a_g->GetTrishapeRuntimeData().vertexCount;
				auto* rd = rtd.rendererData;
				void* vb = nullptr;
				std::uint32_t stride = 0;
				if (rd) {
					vb = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(rd) + 0x00);
					if (vb) {
						D3D11_BUFFER_DESC bd{};
						static_cast<ID3D11Buffer*>(vb)->GetDesc(&bd);
						stride = vc ? bd.ByteWidth / vc : 0;
					}
				}
				void* raw = nullptr;
				if (rd)
					raw = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(rd) + 0x20);
				const float* wm = reinterpret_cast<const float*>(
					reinterpret_cast<std::uintptr_t>(a_g) + 0x07C);
				SKSE::log::info(
					"v155: mesh[{}] child verts={} rd={} vb={} raw={} stride={} tr=({:.1f},{:.1f},{:.1f})",
					a_q, vc, static_cast<void*>(rd), vb, raw, stride, wm[9], wm[10], wm[11]);
				// v165：vertexDesc 属性偏移 dump——v162 盲写法线@16 破坏 normal map 光照
				// 实锤教训。打印真实布局（pos/uv0/normal/tangent/color），为正确重算
				// 法线（暗三角根治）提供权威偏移。
				const auto& vd = rtd.vertexDesc;
				using Attr = RE::BSGraphics::Vertex::Attribute;
				SKSE::log::info(
					"v165: mesh[{}] child offsets: pos={} uv0={} normal={} tangent={} color={} landdata={}",
					a_q,
					vd.GetAttributeOffset(Attr::VA_POSITION),
					vd.GetAttributeOffset(Attr::VA_TEXCOORD0),
					vd.GetAttributeOffset(Attr::VA_NORMAL),
					vd.GetAttributeOffset(Attr::VA_BINORMAL),
					vd.GetAttributeOffset(Attr::VA_COLOR),
					vd.GetAttributeOffset(Attr::VA_LANDDATA));
				// v177：顶点原始字节 dump（前 2 顶点前 64B）——SmoothTerrain 细分网格用
				// 自己的 LandVertex 紧凑布局（非 40B float4），确认 stride/位置/法线偏移。
				// 注意：__try 内禁止对象展开 → 用 char 数组不用 std::string
				if (raw && vc > 1 && stride > 0) {
					char hex0[256]{}, hex1[256]{};
					auto* b0 = static_cast<const std::uint8_t*>(raw);
					auto* b1 = static_cast<const std::uint8_t*>(raw) + stride;
					std::size_t off0 = 0, off1 = 0;
					for (int i = 0; i < 64 && i < static_cast<int>(stride) && off0 < sizeof(hex0) - 4; i++) {
						off0 += static_cast<std::size_t>(std::snprintf(hex0 + off0, sizeof(hex0) - off0, "%02X ", b0[i]));
						off1 += static_cast<std::size_t>(std::snprintf(hex1 + off1, sizeof(hex1) - off1, "%02X ", b1[i]));
					}
					SKSE::log::info("v177: mesh[{}] stride={} v[0] hex: {}", a_q, stride, hex0);
					SKSE::log::info("v177: mesh[{}] stride={} v[1] hex: {}", a_q, stride, hex1);
				}
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				SKSE::log::info("v155: mesh[{}] child ACCESS VIOLATION (skip)", a_q);
			}
		}

		// v158：取消雪层抬高（kSnowLayer=0）——用户否决：抬高后 5×5 边缘外未抬高，
		// 走过去看到地面突然变高很怪异。纯变形（坑下陷），碰撞同步（Havok）另行攻。
		constexpr float kSnowLayer = 0.0f;

		// 递归遍历节点树，找我们的网格（BSTriShape）
		// v117：用户已手动删除 beard（NIF 只剩 SnowShell 平面）——简化匹配：
		// 找第一个 BSTriShape 且顶点数>0（SnowShell 2304）
		// v308：**跳过 beard**——二进制实锤（2026-08-22）用户改的 NIF 里
		// HumanBeardLong01（250 顶点 BSDynamicTriShape）还在；As<>(n,"BSTriShape")
		// 的 RTTI 名字链让 beard 也命中 → 永远先选中 beard（250）→ 雪壳挂载跑在
		// beard 上（日志 numVert=250 MATCH 实锤）。要求 nv>=1000 跳过 beard，
		// 只选中主雪壳（2304）。
		[[nodiscard]] RE::BSDynamicTriShape* FindDynamicMesh(RE::NiAVObject* a_node)
		{
			if (!a_node)
				return nullptr;
			if (auto* dyn = As<RE::BSDynamicTriShape>(a_node, "BSTriShape")) {
				const auto nv = dyn->GetTrishapeRuntimeData().vertexCount;
				SKSE::log::info("FindDynamicMesh: BSTriShape numVert={}", nv);
				if (nv >= 1000) {  // v308：跳过 beard（250）
					SKSE::log::info("FindDynamicMesh: MATCH (SnowShell)");
					return dyn;
				} else {
					SKSE::log::info("FindDynamicMesh: skip (beard? {})", nv);
				}
			}
			if (auto* niNode = As<RE::NiNode>(a_node, "NiNode")) {
				for (auto& child : niNode->GetChildren()) {
					if (auto* d = FindDynamicMesh(child.get())) {
						return d;
					}
				}
			}
			return nullptr;
		}

		// 递归遍历节点树，找 NiTriShape（v83：PyNifly 生成的 NiTriShape，引擎传统
		// 网格类型，v48 手推车验证渲染 OK；BSTriShape/BSDynamicTriShape 都失败了）
		[[nodiscard]] RE::NiTriShape* FindNiTriShape(RE::NiAVObject* a_node)
		{
			if (!a_node)
				return nullptr;
			if (auto* tri = As<RE::NiTriShape>(a_node, "NiTriShape")) {
				// v92 实验：放宽匹配（numVert>0）——LoadPath + 手推车对照（v89 是 LoadStream）
				if (auto* gd = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(tri) + 0x120)) {
					auto nv = *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uintptr_t>(gd) + 0x10);
					SKSE::log::info("FindNiTriShape: NiTriShape numVert={}", nv);
					if (nv > 0) {
						SKSE::log::info("FindNiTriShape: MATCH (first NiTriShape, nv={})", nv);
						return tri;
					}
				} else {
					SKSE::log::warn("FindNiTriShape: spModelData null");
				}
			}
			if (auto* niNode = As<RE::NiNode>(a_node, "NiNode")) {
				for (auto& child : niNode->GetChildren()) {
					if (auto* t = FindNiTriShape(child.get())) {
						return t;
					}
				}
			}
			return nullptr;
		}

		// 递归遍历节点树，找 shader property（用于克隆材质）
		// v55：只接受 BSLightingShaderProperty（普通光照材质）——BSEffectShaderProperty
		// （效果着色器）克隆后渲染崩（资源引用无效，转镜头实测）
		// v67：恢复 GetGeometryRuntimeData()（v62 实测安全）——v64 裸偏移 0x118 对
		// 树网格（如 L2_Branches04）读到 float 当指针 → RTTIIsA 解引用崩（v66 崩溃实锤）
		[[nodiscard]] RE::BSShaderProperty* FindAnyShader(RE::NiAVObject* a_node)
		{
			if (!a_node)
				return nullptr;
			if (auto* geom = As<RE::BSGeometry>(a_node, "BSGeometry")) {
				const auto& rt = geom->GetGeometryRuntimeData();
				auto* prop = rt.properties[RE::BSGeometry::States::kEffect].get();
				if (prop && RTTIIsA(prop, "BSLightingShaderProperty")) {
					return static_cast<RE::BSShaderProperty*>(prop);
				}
			}
			if (auto* n = As<RE::NiNode>(a_node, "NiNode")) {
				for (auto& ch : n->GetChildren()) {
					if (auto* sp = FindAnyShader(ch.get())) {
						return sp;
					}
				}
			}
			return nullptr;
		}

		// 从玩家周围 REF 找任意 shader（克隆材质源）
		[[nodiscard]] RE::BSShaderProperty* FindWorldShader()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				return nullptr;
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				SKSE::log::warn("FindWorldShader: TES singleton is null");
				return nullptr;
			}
			RE::BSShaderProperty* result = nullptr;
			tes->ForEachReferenceInRange(player, 800.0f,
				[&](RE::TESObjectREFR* a_ref) {
					if (!a_ref || result)
						return RE::BSContainer::ForEachResult::kContinue;
					if (auto* root = a_ref->Get3D()) {
						result = FindAnyShader(root);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			return result;
		}

		// 给 mesh 挂克隆材质（v76：**重大修正**——v64-v75 一直裸写 0x118 是 NiGeometry
		// 的 m_spEffectState 偏移，但我们的网格是 BSGeometry(BSTriShape)，材质在
		// GetGeometryRuntimeData().properties[kEffect]（AE@0x160+2*8=0x170）——
		// 裸写 0x118 污染的是 modelBound，材质从未挂上 → 网格一直不可见（实锤）。
		// 正确写法：properties[kEffect] = sp（NiPointer 赋值自动维护引用计数）。
		void AttachShader(RE::BSDynamicTriShape* a_mesh, RE::BSShaderProperty* a_src)
		{
			if (!a_mesh)
				return;
			// v104：安全版——v100 崩溃实锤（static_cast 无 RTTI 检查 + emissive 写入破坏
			// 材质对象 → call [rax+0x10]）。改为：RTTI 检查确认 BSLightingShaderProperty
			// 后再转换，**不写 emissive**（品红诊断已完成使命），只克隆+挂载。
			if (!a_src) {
				SKSE::log::warn("AttachShader: no shader source found (mesh invisible)");
				return;
			}
			if (!RTTIIsA(a_src, "BSLightingShaderProperty")) {
				SKSE::log::warn("AttachShader: src rtti {} not BSLightingShaderProperty — skip",
					a_src->GetRTTI() ? a_src->GetRTTI()->GetName() : "?");
				return;
			}
			RE::NiCloningProcess cp;
			auto* clone = a_src->CreateClone(cp);
			if (!clone) {
				SKSE::log::warn("AttachShader: clone failed (mesh invisible)");
				return;
			}
			auto* sp = static_cast<RE::BSLightingShaderProperty*>(clone);
			// v105：**分配新 NiColor 替换 emissiveColor**——v98 写入的品红污染了共享
			// 材质对象（emissiveColor 是指向堆内存的指针，多个材质共享）→ 所有克隆
			// 出的材质都变品红。新分配 (0,0,0) 关闭自发光，材质回到原本漫反射颜色。
			sp->emissiveColor = new RE::NiColor(0.0f, 0.0f, 0.0f);
			RE::NiPointer<RE::NiProperty> nprop(sp);
			a_mesh->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect] = nprop;
			SKSE::log::info("AttachShader: cloned shader {} to properties[kEffect] (src rtti {})",
				static_cast<void*>(sp),
				a_src->GetRTTI() ? a_src->GetRTTI()->GetName() : "?");
		}

		// v106：找 BSLightingShaderProperty（雪材质 NIF 树内递归）
		[[nodiscard]] RE::BSLightingShaderProperty* FindShaderInTree(RE::NiAVObject* a_node)
		{
			if (!a_node)
				return nullptr;
			if (auto* geom = As<RE::BSGeometry>(a_node, "BSGeometry")) {
				auto* sp = geom->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get();
				if (sp && RTTIIsA(sp, "BSLightingShaderProperty"))
					return static_cast<RE::BSLightingShaderProperty*>(sp);
			}
			if (RTTIIsA(a_node, "NiNode")) {
				auto* nd = static_cast<RE::NiNode*>(a_node);
				for (auto& ch : nd->GetChildren()) {
					if (auto* r = FindShaderInTree(ch.get()))
						return r;
				}
			}
			return nullptr;
		}
		// v85：NiTriShape 材质挂载——裸偏移 **0x118**（SKSE 源码布局 spEffectState@0x118，
		// v84 实测验证；CommonLibSSE-NG GetRuntimeData() 对 NiGeometry 在 AE 不可靠）
		void AttachShaderNiTriShape(RE::NiTriShape* a_mesh)
		{
			if (!a_mesh)
				return;
			if (auto* src = FindWorldShader()) {
				RE::NiCloningProcess cp;
				if (auto* clone = src->CreateClone(cp)) {
					auto* sp = static_cast<RE::BSShaderProperty*>(clone);
					RE::NiPointer<RE::NiProperty> nprop(sp);
					auto* slot = reinterpret_cast<RE::NiPointer<RE::NiProperty>*>(
						reinterpret_cast<std::uintptr_t>(a_mesh) + 0x118);
					*slot = nprop;
					SKSE::log::info("AttachShaderNiTriShape: cloned shader {} to spEffectState@0x118 (src rtti {})",
						static_cast<void*>(sp),
						src->GetRTTI() ? src->GetRTTI()->GetName() : "?");
				} else {
					SKSE::log::warn("AttachShaderNiTriShape: clone failed (mesh invisible)");
				}
			} else {
				SKSE::log::warn("AttachShaderNiTriShape: no world shader found (mesh invisible)");
			}
		}
	}
	// attrs 位：bit4=Vertex(16) bit5=UVs(32) bit7=Normals(128) bit8=Tangents(256)
	//        bit9=Colors(512) bit10=Skinned(1024) bit12=EyeData(4096)
	[[nodiscard]] std::uint32_t ComputeVertexStride(std::uint16_t a_attrs)
	{
		std::uint32_t s = 0;
		if (a_attrs & 0x10) {         // Vertex
			s += 12;                  // Vector3
			s += 4;                   // Bitangent X (float) 或 Unknown Int
		}
		if (a_attrs & 0x20)           // UVs
			s += 4;                   // HalfTexCoord
		if (a_attrs & 0x80) {         // Normals
			s += 4;                   // ByteVector3 + Bitangent Y
			if (a_attrs & 0x100)      // Tangents
				s += 4;               // Tangent ByteVector3 + Bitangent Z
		}
		if (a_attrs & 0x200)          // Colors
			s += 4;
		if (a_attrs & 0x400)          // Skinned
			s += 12;                  // Bone Weights hfloat×4 + Bone Indices byte×4
		if (a_attrs & 0x1000)         // EyeData
			s += 4;
		return s;
	}

	SnowShellMesh& GetSnowShellMesh()
	{
		static SnowShellMesh instance;
		return instance;
	}




	void SnowShellMesh::UpdatePlayerPos(const RE::NiPoint3& a_pos)
	{
		// v63：每帧记录玩家位置（Present hook 内调用，与 LANDSCAPE 更新同线程）。
		// 顶点写世界坐标 + 挂世界节点（单位变换）→ 网格以玩家为中心跟随。
		playerPos = a_pos;
		// v122：地形贴合采样请求（渲染线程发起，实际采样 AddTask 到游戏线程）
		RequestTerrainSample(a_pos);
	}

	// v122：地形采样请求（渲染线程）——网格水平移动 > 0.5 单位时，把实际采样
	// 转发到游戏线程（GetLandHeight 是引擎函数，只能在游戏线程调用；铁律：游戏
	// 数据只在游戏线程访问，渲染线程直接调会闪退）。terrainSampling 防重入。
	void SnowShellMesh::RequestTerrainSample(const RE::NiPoint3& a_playerPos)
	{
		if (!initialized || !root)
			return;
		if (terrainSampling.exchange(true))  // v569：防重入改 atomic（渲染线程写 true / 游戏线程写 false 跨线程）
			return;
		const float dx = a_playerPos.x - lastTerrainPos.x;
		const float dy = a_playerPos.y - lastTerrainPos.y;
		if (terrainVersion.load() == 0 || dx * dx + dy * dy > 0.5f * 0.5f) {
			lastTerrainPos = a_playerPos;
			SKSE::GetTaskInterface()->AddTask([this]() { DoTerrainSample(); });
		} else {
			// v609：**停摆修复**——位移 ≤0.5 且已采过样 → 不调度，标志位必须复位，
			// 否则 exchange(true) 已置位且永不释放 → 后续所有调用提前返回，地形高度
			// 缓存永久停更（站立一帧即死锁）。
			terrainSampling.store(false);
		}
	}

	// v122：地形高度采样（游戏线程，AddTask 转发执行）——对每个顶点算世界水平
	// 坐标（dynMesh world 矩阵），GetLandHeight 查真实地形高度写入 terrainH 缓存。
	void SnowShellMesh::DoTerrainSample()
	{
		if (!initialized || !root || terrainH.size() != meshVertexCount)
			terrainH.resize(meshVertexCount);
		// v316：NiTriShape 分支不设 dynMesh → 用 root 兜底（world 变换相同——
		// root 与子网格同变换，NiAVObject world @0x07C）
		auto* srcNode = dynMesh ? static_cast<RE::NiAVObject*>(dynMesh) : root;
		auto* worldM = reinterpret_cast<const float*>(
			reinterpret_cast<std::uintptr_t>(srcNode) + 0x07C);
		auto* worldT = reinterpret_cast<const float*>(
			reinterpret_cast<std::uintptr_t>(srcNode) + 0x07C + 0x24);
		auto* tes = RE::TES::GetSingleton();
		const float fallbackH = worldT[2];
		for (std::uint32_t vi = 0; vi < meshVertexCount; vi++) {
			const float ox = origXZ[vi * 2 + 0];
			const float oa = origXZ[vi * 2 + 1];  // 平面内第二分量（XY: y / XZ: z）
			// v312：布局自适应世界水平坐标——XY 布局用 (x,y) 列，XZ 布局用 (x,z) 列
			float wx, wy;
			if (origIsXZ) {
				wx = worldM[0] * ox + worldM[2] * oa + worldT[0];
				wy = worldM[3] * ox + worldM[5] * oa + worldT[1];
			} else {
				wx = worldM[0] * ox + worldM[1] * oa + worldT[0];
				wy = worldM[3] * ox + worldM[4] * oa + worldT[1];
			}
			float h = fallbackH;
			if (tes && tes->GetLandHeight(RE::NiPoint3{ wx, wy, 0.0f }, h)) {
				terrainH[vi] = h;
			} else {
				terrainH[vi] = fallbackH;
			}
		}
		terrainVersion.fetch_add(1);  // 版本号发布（写端游戏线程，读端渲染线程）
		terrainSampling.store(false);
		if (terrainVersion.load() == 1)
			SKSE::log::info("v122: terrain sampled ({} verts) h0={:.1f} fallback={:.1f}",
				meshVertexCount, terrainH[0], fallbackH);
	}

	// v170：加载高密度地形网格 NIF（游戏线程，auto-create 时调一次）。
	// Blender 导出 highres_quadrant.nif（129/257 平面，z=0）→ NiStream::LoadPath
	// → 引擎加载器自动建 rendererData/vb/ib（v128 手工创建缺 rendererData 教训，
	// 只有 NIF 加载器会建）→ 找几何块（BSTriShape 优先，NiTriShape 兜底）。
	void SnowShellMesh::HandleProjectileImpact(RE::Projectile* self, const RE::NiPoint3& a_loc, int a_shape)
	{
		(void)self;  // v565：C4100
		// 玩家距离过滤（远处命中无视觉，省场写入）
		const auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc)
			return;
		const auto pp = pc->GetPosition();
		if ((a_loc - pp).Length() > 2048.0f)
			return;
		// 冷却：爆炸 250ms（连发法术不爆场）、箭 100ms——v569：**分离**（原共用
		// lastProjT：爆炸后 250ms 内的箭被吞、箭后 100ms 内爆炸被吞）
		static unsigned long lastArrowT = 0, lastBoomT = 0;
		const unsigned long nowT = GetTickCount();
		if (a_shape == 13) {
			if (nowT - lastBoomT < 250)
				return;
			lastBoomT = nowT;
		} else {
			if (nowT - lastArrowT < 100)
				return;
			lastArrowT = nowT;
		}
		// 盖章参数：箭 = 物品同款小坑（rL/rS=8 战壕 40 宽）；爆炸 = 大坑（r=40）+ 环形雪堆
		const float depth = (a_shape == 13) ? 1.2f : 0.5f;
		const float rL = (a_shape == 13) ? 40.0f : 8.0f;
		{
			auto& shell = SnowDeform::GetSnowShellMesh();  // 静态函数经单例访问成员
			std::lock_guard<std::mutex> lk(shell.footMtx);
			shell.footprints.push_back({ a_loc.x, a_loc.y, depth, 0.0f, 0.0f, 0.0f,
				rL, rL, a_loc.x, a_loc.y, a_shape, GetTickCount() });
			shell.landFootDirty.store(true);
		}
		SKSE::log::info("v559: projectile impact shape={} at=({:.0f},{:.0f}) r={:.0f} dist={:.0f}",
			a_shape, a_loc.x, a_loc.y, rL, (a_loc - pp).Length());
	}

	void SnowShellMesh::AddImpactHookArrow(RE::Projectile* self, RE::TESObjectREFR* a_ref,
		const RE::NiPoint3& a_loc, const RE::NiPoint3& a_vel, RE::hkpCollidable* a_col,
		std::int32_t a6, std::uint32_t a7)
	{
		HandleProjectileImpact(self, a_loc, 12);
		if (g_addImpactArrow)
			g_addImpactArrow(self, a_ref, a_loc, a_vel, a_col, a6, a7);
	}

	void SnowShellMesh::AddImpactHookMissile(RE::Projectile* self, RE::TESObjectREFR* a_ref,
		const RE::NiPoint3& a_loc, const RE::NiPoint3& a_vel, RE::hkpCollidable* a_col,
		std::int32_t a6, std::uint32_t a7)
	{
		HandleProjectileImpact(self, a_loc, 13);
		if (g_addImpactMissile)
			g_addImpactMissile(self, a_ref, a_loc, a_vel, a_col, a6, a7);
	}

	// v565：static 数据成员定义（原在高密死链内被连带删除——AddImpactHook* 活函数引用）
	SnowShellMesh::AddImpactFn SnowShellMesh::g_addImpactArrow = nullptr;
	SnowShellMesh::AddImpactFn SnowShellMesh::g_addImpactMissile = nullptr;

	void SnowShellMesh::InstallProjectileHook()
	{
		constexpr std::size_t kSlot = 0xBD;  // AddImpact
		// ArrowProjectile（箭矢）
		{
			REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_ArrowProjectile[0] };
			auto* vtable = reinterpret_cast<std::uintptr_t*>(vt.address());
			g_addImpactArrow = reinterpret_cast<AddImpactFn>(vtable[kSlot]);
			DWORD oldProtect;
			if (VirtualProtect(&vtable[kSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				vtable[kSlot] = reinterpret_cast<std::uintptr_t>(&AddImpactHookArrow);
				VirtualProtect(&vtable[kSlot], sizeof(void*), oldProtect, &oldProtect);
				SKSE::log::info("v559: ArrowProjectile AddImpact hooked (slot {} old={:x})", kSlot,
					reinterpret_cast<std::uintptr_t>(g_addImpactArrow));
			}
		}
		// MissileProjectile（法术弹——火球/冰锥/闪电弹等）
		{
			REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_MissileProjectile[0] };
			auto* vtable = reinterpret_cast<std::uintptr_t*>(vt.address());
			g_addImpactMissile = reinterpret_cast<AddImpactFn>(vtable[kSlot]);
			DWORD oldProtect;
			if (VirtualProtect(&vtable[kSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				vtable[kSlot] = reinterpret_cast<std::uintptr_t>(&AddImpactHookMissile);
				VirtualProtect(&vtable[kSlot], sizeof(void*), oldProtect, &oldProtect);
				SKSE::log::info("v559: MissileProjectile AddImpact hooked (slot {} old={:x})", kSlot,
					reinterpret_cast<std::uintptr_t>(g_addImpactMissile));
			}
		}
	}

	// v609：删 InstallQuadBuildHook 空实现（v199 已弃用 detour——只剩 warn 日志，
	// main.cpp 调用点同步移除）

	// v563：v172/v442-v444 动态视差（diffuse alpha 注入 / Terrain Helper 接入）已全部
	// 移除（用户拍板关闭 + v563 代码清理）——纯几何变形版。

	// v370：**诊断遍历**（root 子树结构 + 每个 BSTriShape 的 properties 状态）——定位材质
	// 到底在哪（v369 NOT found：所有 BSTriShape 的 properties[kProperty] 全 null 的谜团）。
	static void WalkDiagBoxTex(RE::NiAVObject* n, int& a_nodes, int& a_shapes, int& a_props)
	{
		if (!n)
			return;
		a_nodes++;
		if (RTTIIsA(n, "BSTriShape")) {
			a_shapes++;
			auto* geo = static_cast<RE::BSTriShape*>(n);
			auto& grd = geo->GetGeometryRuntimeData();
			auto* p0 = grd.properties[RE::BSGeometry::States::kProperty].get();
			auto* p1 = grd.properties[RE::BSGeometry::States::kEffect].get();
			const char* p0n = "(null)";
			if (p0 && p0->GetRTTI() && p0->GetRTTI()->name)
				p0n = p0->GetRTTI()->name;
			const char* p1n = "(null)";
			if (p1 && p1->GetRTTI() && p1->GetRTTI()->name)
				p1n = p1->GetRTTI()->name;
			SKSE::log::info("v371: BSTriShape {} props[0]={} ({}) props[1]={} ({})",
				static_cast<void*>(n), static_cast<void*>(p0), p0n,
				static_cast<void*>(p1), p1n);
			if (p0 || p1)
				a_props++;
		}
		if (!RTTIIsA(n, "NiNode"))
			return;
		auto* node = static_cast<RE::NiNode*>(n);
		for (auto& ch : node->GetChildren())
			WalkDiagBoxTex(ch.get(), a_nodes, a_shapes, a_props);
	}


	// v126：真地形变形——诊断玩家 cell 的 landscape 结构（游戏线程调用）
	// 链路：TES::GetCell(pos) → TESObjectCELL::cellLand → TESObjectLAND::loadedData
	// → LoadedLandData.geom[4]（BSTriShape 渲染网格）+ heights[4][289]（高度数据）
	void SnowShellMesh::DebugLandscape()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return;
		const auto pos = player->GetPosition();
		auto* tes = RE::TES::GetSingleton();
		auto* cell = tes ? tes->GetCell(pos) : nullptr;
		if (!cell) {
			SKSE::log::info("v126-dbg: no cell at ({:.0f},{:.0f})", pos.x, pos.y);
			return;
		}
		auto* land = cell->GetRuntimeData().cellLand;
		if (!land) {
			SKSE::log::info("v126-dbg: cell has no landscape");
			return;
		}
		auto* ld = land->loadedData;
		if (!ld) {
			SKSE::log::info("v126-dbg: no loaded land data");
			return;
		}
		// v157：移除 v149 实验触发（4 象限抬高 300）——v156 实锤 mesh.child 为
		// 真正渲染对象（用户实测 4 块全抬高 ✓）。F9 只保留缓存+诊断。
		// v150/v155：遍历 mesh[4]（NiNode）子节点——真正被引擎渲染的几何对象
		// （v149 实验证明只有 65×65 的 geom vb 生效，17×17 引擎不用 → 真正渲染的
		// 网格挂在 mesh[q] NiNode 下）。v155：完整结构诊断 + geom 共享关系对比：
		// 若 geom[q] 与 mesh[q].child[0] 共享同一 rendererData → 解释"为什么只有
		// geom[3]（65×65）有效"——它改的恰好就是渲染缓冲；同时确认 mesh.child
		// 的 raw/vb/stride/worldT 是否可安全操作（v152 直接缓存崩，本版只读）。
		for (int q = 0; q < 4; q++) {
			auto* node = ld->mesh[q];
			RE::BSTriShape* mc = nullptr;
			if (node) {
				const auto cc = node->GetChildren().size();
				SKSE::log::info("v155: mesh[{}] node={} children={}",
					q, static_cast<void*>(node), cc);
				if (cc > 0)
					mc = As<RE::BSTriShape>(node->GetChildren()[0].get(), "BSTriShape");
			} else {
				SKSE::log::info("v155: mesh[{}] = null", q);
			}
			SafeDumpMeshChild(q, mc);
			auto* g = ld->geom[q].get();
			if (g) {
				auto& rtd = g->GetGeometryRuntimeData();
				bool sameRd = mc && rtd.rendererData == mc->GetGeometryRuntimeData().rendererData;
				SKSE::log::info("v155: geom[{}] verts={} rd={} sameRdAsMesh={}",
					q, g->GetTrishapeRuntimeData().vertexCount,
					static_cast<void*>(rtd.rendererData), sameRd ? "YES" : "no");
			} else {
				SKSE::log::info("v155: geom[{}] = null", q);
			}
		}
		// v166：normals[4][289]（CHAR_NORM int8 压缩法线，引擎地形法线数据源）
		// dump——"边花/暗三角"根因：改了几何顶点但引擎法线数据源没动 → 插值出的
		// 法线还是平面法线 → 坑壁光照错乱。先读真实数据确认格式（int8/127=单位
		// 向量分量），下一步同步改它（小步验证防 v147 式崩溃）。
		for (int qn = 0; qn < 4; qn++) {
			const auto& na = ld->normals[qn][0];
			const auto& nb = ld->normals[qn][100];
			const auto& nc = ld->normals[qn][144];
			SKSE::log::info("v166: normals[{}] [0]=({},{},{}) [100]=({},{},{}) [144]=({},{},{})",
				qn,
				static_cast<int>(na.x), static_cast<int>(na.y), static_cast<int>(na.z),
				static_cast<int>(nb.x), static_cast<int>(nb.y), static_cast<int>(nb.z),
				static_cast<int>(nc.x), static_cast<int>(nc.y), static_cast<int>(nc.z));
		}
		// v168：删 v167 normals 翻转实验——实测画面无变化 → normals[4][289] 不驱动
		// 渲染法线（引擎法线来自 heights/shader 计算），法线同步路线关闭。
		// v184：quadTextures 链路诊断——地形材质 = LoadedLandData::quadTextures[4][6]
		// （每象限 6 层）→ TESLandTexture::textureSet（BGSTextureSet）→ diffuse 路径。
		// 这是 v172 动态视差挂载的正解（绕开失败的地形网格 BSShaderProperty）——
		// 先确认链路 + 找 snow01 雪层（ENB TerrainParallax 读其 alpha）。
		for (int qt = 0; qt < 4; qt++) {
			auto* defTex = ld->defQuadTextures[qt];
			const char* defPath = "?";
			if (defTex && defTex->textureSet) {
				defPath = defTex->textureSet->GetTexturePath(RE::BSTextureSet::Textures::kDiffuse);
			}
			SKSE::log::info("v184: quad[{}] def={} defDiffuse={}",
				qt, static_cast<void*>(defTex), defPath ? defPath : "?");
			for (int li = 0; li < 6; li++) {
				auto* lt = ld->quadTextures[qt][li];
				if (!lt)
					continue;
				const char* dPath = "?";
				if (lt->textureSet)
					dPath = lt->textureSet->GetTexturePath(RE::BSTextureSet::Textures::kDiffuse);
				SKSE::log::info("v184: quad[{}] layer[{}] lt={} ts={} diffuse={}",
					qt, li, static_cast<void*>(lt), static_cast<void*>(lt->textureSet),
					dPath ? dPath : "?");
			}
		}
		SKSE::log::info("v126-dbg: cell={} land={} loadedData={}",
			static_cast<void*>(cell), static_cast<void*>(land), static_cast<void*>(ld));
		for (int q = 0; q < 4; q++) {
			auto* g = ld->geom[q].get();
			if (!g) {
				SKSE::log::info("v126-dbg: geom[{}] null", q);
				continue;
			}
			const auto vc = g->GetTrishapeRuntimeData().vertexCount;
			const auto tc = g->GetTrishapeRuntimeData().triangleCount;
			auto& rtd = g->GetGeometryRuntimeData();
			const auto stride = rtd.vertexDesc.GetSize();
			const auto posOff = rtd.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			SKSE::log::info("v126-dbg: geom[{}] verts={} tris={} stride={} posOff={} rendererData={}",
				q, vc, tc, stride, posOff, static_cast<void*>(rtd.rendererData));
			// v609：**判空保护**——geom 无 rendererData（空 LOD/未加载）时跳过采样，
			// 否则 `*(rendererData+0x20)` 空指针解引用崩溃（诊断路径 F9 触发）
			if (!rtd.rendererData) {
				SKSE::log::info("v126-dbg:   geom[{}] no rendererData, skip", q);
				continue;
			}
			// 顶点采样 + world 变换
			auto* rawV = *reinterpret_cast<std::uint8_t**>(
				reinterpret_cast<std::uintptr_t>(rtd.rendererData) + 0x20);
			auto* wm = reinterpret_cast<const float*>(
				reinterpret_cast<std::uintptr_t>(g) + 0x07C);
			SKSE::log::info("v126-dbg:   world tr=({:.0f},{:.0f},{:.0f})",
				wm[9], wm[10], wm[11]);
			// v127c：查 vertexBuffer 真实字节大小（ByteWidth/顶点数 = 真实 stride）
			if (rtd.rendererData) {
				auto* vb = *reinterpret_cast<ID3D11Buffer**>(
					reinterpret_cast<std::uintptr_t>(rtd.rendererData) + 0x00);
				if (vb) {
					D3D11_BUFFER_DESC bd{};
					vb->GetDesc(&bd);
					SKSE::log::info("v127c: geom[{}] vb ByteWidth={} → stride/vert={}",
						q, bd.ByteWidth, vc ? bd.ByteWidth / vc : 0);
				}
			}
			if (rawV && vc > 0 && q == 3) {
				// v127d：stride=40（ByteWidth/vc 实测）dump geom[3] 顶点完整布局
				std::uint32_t realStride = 40;
				if (rtd.rendererData) {
					auto* vb2 = *reinterpret_cast<ID3D11Buffer**>(
						reinterpret_cast<std::uintptr_t>(rtd.rendererData) + 0x00);
					if (vb2) {
						D3D11_BUFFER_DESC bd2{};
						vb2->GetDesc(&bd2);
						realStride = bd2.ByteWidth / vc;
					}
				}
				for (std::uint32_t i = 0; i < 5; i++) {
					const float* f = reinterpret_cast<const float*>(rawV + i * realStride);
					SKSE::log::info("v127d: geom[3] v[{}] = [{:.1f} {:.1f} {:.1f} {:.1f} {:.1f} | {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}]",
						i, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
				}
			}
			if (rawV && vc > 0) {
				for (std::uint32_t i = 0; i < std::min<std::uint32_t>(vc, 6); i++) {
					const float x = *reinterpret_cast<const float*>(rawV + i * stride + posOff + 0);
					const float y = *reinterpret_cast<const float*>(rawV + i * stride + posOff + 4);
					const float z = *reinterpret_cast<const float*>(rawV + i * stride + posOff + 8);
					SKSE::log::info("v126-dbg:   v[{}]=({:.1f},{:.1f},{:.1f})", i, x, y, z);
				}
			}
		}
		SKSE::log::info("v126-dbg: heights[0][0..3]={:.1f} {:.1f} {:.1f} {:.1f}",
			ld->heights[0][0], ld->heights[0][1], ld->heights[0][2], ld->heights[0][3]);
	}

	// v130：**真地形踩雪变形**——游戏线程缓存玩家周围 3×3 cell 的 landscape
	// geom（每 cell 4 quadrant：q=3 高分辨率 65×65 当前 quadrant 局部坐标+worldT，
	// q=0-2 低分辨率 17×17 世界坐标 tr=0）。POSITION=float4@0 stride=40（ByteWidth/vc
	// 实测）；世界坐标统一 = 局部 + worldT。原始副本存 orig（防每帧累积）。
	// 双缓冲：填备用 landBuf[1-idx] → 原子切换 idx → landReady=true（渲染线程
	// 只在 landReady 后读 idx 指向的缓冲——FindLandscape 改写与遍历零竞争）。
	void SnowShellMesh::FindLandscape(bool a_skipBuild)
	{
		(void)a_skipBuild;  // v565：C4100
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return;
		const auto pos = player->GetPosition();
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		// v131：玩家 cell 坐标（EXTERIOR_DATA/XCLC——定位玩家 quadrant 归属）
		int pcx = 0, pcy = 0;
		if (auto* cellMain = tes->GetCell(pos)) {
			if (auto* cd = cellMain->GetCoordinates()) {
				pcx = cd->cellX;
				pcy = cd->cellY;
				SKSE::log::info("v131: player cell=({},{}) pos=({:.0f},{:.0f})",
					pcx, pcy, pos.x, pos.y);
			}
		}
		// v333：**重建不暂停渲染**——原 2453 `landReady.store(false)` 在 re-caching
		//（玩家移动 >512 单位触发）期间让渲染线程 3108 直接 return → 地形/盒子
		// 暂停更新 → "一闪一闪、一会有一会没有"（用户 14:32 反馈）。双缓冲
		// landBuf[2] + landBufIdx 原子切换本就保证渲染线程永远读到完整缓冲
		// （旧 idx 指向旧缓冲，新缓冲填完才切换）→ landReady=false 冗余且有害。
		// 删除：re-caching 期间渲染线程继续用旧缓存（旧 landAnchor，盒子偏移
		// ≤512 仍在 4700² 覆盖内），新缓冲就绪原子切换 → 无消失、无闪。
		landPaused.store(false);  // v149：新缓存恢复（退出实验模式）

		const int oldIdx = landBufIdx.load();
		const int newIdx = 1 - oldIdx;
		auto& cells = landBuf[newIdx];
		int found = 0;
		int cellCount = 0;
		// v136：5×5 cell 缓存（dx/dy ∈ -2..2）——覆盖玩家视野（3-4 cell），避免
		// 抬高 28 的"平台"边界出现在视野内造成 28 单位的明显裂缝。
		// 索引 ci = (dy+2)*5 + (dx+2) ∈ 0..24
		// v258：7×7 缓存（dx/dy ∈ -3..3）——覆盖视野（3-4 cell），129² 交界移出视野
		for (int dy = -3; dy <= 3; dy++) {
			for (int dx = -3; dx <= 3; dx++) {
				const int ci = (dy + 3) * 7 + (dx + 3);
				for (int q = 0; q < 4; q++)
					cells[ci][q] = LandCellGeom{};
				RE::NiPoint3 probe{ pos.x + dx * kCellSize, pos.y + dy * kCellSize, pos.z };
				auto* cell = tes->GetCell(probe);
				auto* land = cell ? cell->GetRuntimeData().cellLand : nullptr;
				auto* ld = (land && land->loadedData) ? land->loadedData : nullptr;
				if (!ld) {
					// v136：5×5 边缘 cell 常无 landscape（世界边缘/玩家刚加载），
					// warn 刷屏改为静默（避免日志爆炸）
					continue;
				}
				// v145：cell 锚点 = 世界坐标公式直接算——不再依赖 geom[3] worldT！
				// Skyrim cell 世界原点 = (cellX×4096, cellY×4096)（v131 实测 cell(27,24)
				// → (110592,98304)）；4 个 quadrant 共享锚点 = 原点 + (2048,2048)：
				//   局部 [-2048,0]²（q0-2 17×17）+ 锚点 = cell 西南/东南/西北 quadrant
				//   局部 [0,2048]²（q3 65×65）+ 锚点 = cell 东北 quadrant
				// 之前用 geom[3] worldT 作锚点——缺 65×65 的 cell（边缘/低 LOD）锚点
				// 失败 → 17×17 worldT=0 → 整个 cell 被合法性检查丢弃 → "一整块 cell 有效，
				// 相邻 cell 断"。公式锚点根治（所有 cell 都能正确锚定）。
				int ccx = 0, ccy = 0;
				if (auto* cd = cell->GetCoordinates()) {
					ccx = cd->cellX;
					ccy = cd->cellY;
				}
				const float anchorX = static_cast<float>(ccx) * kCellSize + 2048.0f;
				const float anchorY = static_cast<float>(ccy) * kCellSize + 2048.0f;
				for (int q = 0; q < 4; q++) {
					// v156：缓存 mesh[q].child[0]（真正渲染对象）——v155 实锤 geom[3]
					// 与 mesh[3].child 共享 rd（sameRdAsMesh=YES）→ geom[3] 有效原因；
					// geom[0-2] rd 与 mesh[0-2].child 不同（sameRdAsMesh=no）→ 引擎不渲染。
					// mesh[0..3].child 全部 4225（65×65）、stride=40、worldT 锚点正确
					// （v155 数据 tr=(112640,100352,-8864)）→ 改它们 = 4 象限全有效！
					// v152 崩因排查：25 cell 全访问 children 边缘 cell null → 加严检查。
					RE::BSTriShape* g = nullptr;
					auto* meshNode = ld->mesh[q];
					if (meshNode && meshNode->GetChildren().size() > 0) {
						g = As<RE::BSTriShape>(meshNode->GetChildren()[0].get(), "BSTriShape");
					}
					if (!g)
						continue;
					auto& rtd = g->GetGeometryRuntimeData();
					const auto vc = g->GetTrishapeRuntimeData().vertexCount;
					if (vc == 0)
						continue;
					auto* rd = rtd.rendererData;
					auto* raw = rd ? *reinterpret_cast<std::uint8_t**>(
						reinterpret_cast<std::uintptr_t>(rd) + 0x20) : nullptr;
					if (!raw)
						continue;
					std::uint32_t stride = 40;
					auto* vb = rd ? *reinterpret_cast<ID3D11Buffer**>(
						reinterpret_cast<std::uintptr_t>(rd) + 0x00) : nullptr;
					if (!vb)
						continue;
					D3D11_BUFFER_DESC bd{};
					vb->GetDesc(&bd);
					stride = bd.ByteWidth / vc;
					if (stride == 0)  // v569：ByteWidth 异常时 stride=0 → orig 空 vector → 越界读
						continue;
					auto& lc = cells[ci][q];
					lc.geom = g;
					lc.raw = raw;
					lc.stride = stride;
					lc.verts = vc;
					// v206：雪分类——该 quad 是否雪材质（base + 6 层任一层含雪关键字）
					lc.surfaceClass = QuadSurfaceClass(ld, q);
					// v145：worldT = cell 公式锚点（统一所有 quadrant，不再读 geom world）
					lc.worldT[0] = anchorX;
					lc.worldT[1] = anchorY;
					lc.worldT[2] = 0.0f;
					// v224：高密度网格（rendererData 匹配 25×4 任一）→ orig 用 highResVerts[ci][q]
					// （v213 时代 hci<9 只匹配前 9 个 → 5×5 后部分 129² cell 被当作引擎网格
					// 重新缓存 orig = 读到我们建的 129² 而非 65² → 快照漂移 → 裂缝）
					int hrCi = -1, hrQ = -1;
					for (int hci = 0; hci < 49 && hrCi < 0; hci++) {
						for (int hq = 0; hq < 4; hq++) {
							if (highResRd[hci][hq] && rd == highResRd[hci][hq]) {
								hrCi = hci;
								hrQ = hq;
								break;
							}
						}
					}
					if (hrCi >= 0 && highResVerts[hrCi][hrQ].size() == static_cast<std::size_t>(stride) * vc) {
						lc.orig.assign(highResVerts[hrCi][hrQ].begin(), highResVerts[hrCi][hrQ].end());
					} else {
						lc.orig.assign(raw, raw + static_cast<std::size_t>(stride) * vc);
					}
					// v277：**5×5 边缘 cell 外 1 圈 GetLandHeight 覆盖 orig（静息贴地）**——
					// 用户截图 222233 实锤：裂缝是 5×5 边界（cell ±2,*）的**静息高度差**（无坑
					// 时 129² 与 ±3 圈引擎网格高度基准不一致），v275 淡出消除了"变形切"但
					// 静息差仍在 → 还有裂缝。orig 来源是 FindLandscape 缓存的引擎网格（可能
					// LOD 偏差/滞后）→ 用引擎权威 GetLandHeight 覆盖最外 1 圈 orig → 边界
					// 贴地 → 与 ±3 圈引擎网格（用真实地形高度）一致 → 静息差消除。
					// 数量 ~8256 顶点 × GetLandHeight ≈ 0.5s，在 FindLandscape AddTask
					// 内同步执行（玩家移动 512 才重缓存 → 0.5s 卡顿一次，可接受）。
					// v280：GetLandHeight 静息覆盖**只首次**（landHLogged）——v279 实测每次
					// rebuild（512 移动触发）都跑 0.5s + 改边界 orig → mesh.child 替换瞬间
					// 边界高度跳变 → "闪 + 凹凸消失一秒"（用户实锤）。首次覆盖后边界已贴地
					// （静态），后续 rebuild 用稳定 orig → 不跳变不闪。
					if ((std::abs(dx) == 2 || std::abs(dy) == 2) && !landHLogged) {  // v568：优先级修复——原 A||B&&C 使 dx==2 边缘 cell 每次都跑 GetLandHeight 0.5s（v280"只首次"失效）
						if (vc == highResDim * highResDim) {
							landHLogged = true;  // v280：只首次（0.5s 一次性）
							auto* tesG = RE::TES::GetSingleton();
							if (tesG) {
								const int nn = static_cast<int>(highResDim);
								const float wt2 = *reinterpret_cast<const float*>(
									reinterpret_cast<std::uintptr_t>(g) + 0x07C + 0x24 + 8);
								for (int r = 0; r < nn; r++) {
									for (int c = 0; c < nn; c++) {
										const int ring = std::min({ r, c, nn - 1 - r, nn - 1 - c });
										if (ring == 0) {
											auto* p8 = lc.orig.data() +
												static_cast<std::size_t>(r * nn + c) * stride;
											const float wx = *reinterpret_cast<const float*>(p8 + 0) + anchorX;
											const float wy = *reinterpret_cast<const float*>(p8 + 4) + anchorY;
											float lh = 0.0f;
											if (tesG->GetLandHeight(RE::NiPoint3{ wx, wy, 0.0f }, lh)) {
												*reinterpret_cast<float*>(p8 + 8) = lh - wt2;
											}
										}
									}
								}
							}
						}
					}
					// v134：工作副本初始化 = 原始顶点（变形写 work，引擎 raw 保持干净）
					lc.work = lc.orig;
					// v143：拷贝合法性检查（防 raw 竞态 + worldT 锚点修正失败双重风险）。
					// 图1 拉伸真凶：17×17 顶点局部 x/y = (-2048..0)，若 worldT=0（锚点修正失败
					// 或该 cell 无 65×65 锚点）→ 世界 x = -2048 → 顶点飞到玩家 11 万单位外 →
					// 三角形一条边横跨整个屏幕渲染成白色光带。检查 z + 世界 x/y（必须在
					// 玩家 ±102400 单位内 = 5×5 cell 覆盖范围）→ 不合法丢弃，下轮再补。
					const float tx = lc.worldT[0];
					const float ty = lc.worldT[1];
					bool copyValid = true;
					const float px = pos.x, py = pos.y;
					const float margin = 102400.0f;
					for (std::uint32_t vi = 0; vi < vc; vi++) {
						const auto* f = reinterpret_cast<const float*>(
							lc.orig.data() + static_cast<std::size_t>(vi) * stride);
						const float z = f[2];
						const float wx = f[0] + tx;
						const float wy = f[1] + ty;
						// v144：NaN 检查（NaN 任何比较都 false → 之前范围检查漏掉）+
						// z/x/y 世界坐标合理性。任一非法 → 丢弃整个 geom（拉伸光带真凶）
						if (z != z || wx != wx || wy != wy ||
							z > 1.0e6f || z < -1.0e6f ||
							wx < px - margin || wx > px + margin ||
							wy < py - margin || wy > py + margin) {
							copyValid = false;
							break;
						}
					}
					if (!copyValid) {
						SKSE::log::warn("v144: cell{} geom[{}] invalid copy (vc={}) skipped", ci, q, vc);
						lc = LandCellGeom{};
						continue;
					}
					// v260：玩家 cell（7×7 中心 ci=24）4 quad 的**缓存源密度** dump——
					// 验证"同 cell 内 q3=65² 源（引擎高分辨率 quadrant）vs q0-2=17² 源"
					// 混合假设（= BuildCell 插值出的 129² 内部 quadrant 交界不一致 →
					// "地块交接拉伸"疑因）。首帧缓存玩家 cell 时打印一次。
					if (ci == 24 && !playerSrcLogged) {
						playerSrcLogged = true;
						SKSE::log::info("v260-dbg: player cell src verts q0={} q1={} q2={} q3={}",
							cells[24][0].verts, cells[24][1].verts,
							cells[24][2].verts, cells[24][3].verts);
					}
					// v159：mesh.child 顶点布局 dump（第一次缓存玩家 cell 时）——确认
					// stride=40 里 POSITION/NORMAL/UV/COLOR 偏移，为"法线视觉补偿"
					// （坑内法线倾斜→光照阴影感）做准备。
					if (ci == 24 && q == 0 && !landLayoutLogged) {
						landLayoutLogged = true;
						const auto* fv = reinterpret_cast<const float*>(lc.orig.data());
						SKSE::log::info(
							"v159: mesh.child v[0] layout: [{:.1f} {:.1f} {:.1f} {:.1f} | {:.1f} {:.1f} {:.1f} {:.1f} | {:.1f} {:.1f}] stride={}",
							fv[0], fv[1], fv[2], fv[3], fv[4], fv[5], fv[6], fv[7], fv[8], fv[9], stride);
						// v256：hex dump v[0..3]（40 字节原始字节）——推真实属性布局
						// （float 解释有误导：half/byte 压缩属性被当 float 显示成垃圾）。
						// 对比相邻顶点字节变化：@16-19 变=法线/TANGENT、@20 变=法线、
						// @32 变=UV/TEXCOORD、@36 变=LANDDATA/COLOR。
						const auto* raw8 = reinterpret_cast<const std::uint8_t*>(lc.orig.data());
						for (int vi = 0; vi < 4 && vi < static_cast<int>(lc.verts); vi++) {
							std::string hx;
							char buf[8];
							for (int bi = 0; bi < 40; bi++) {
								snprintf(buf, sizeof(buf), "%02X", raw8[static_cast<std::size_t>(vi) * stride + bi]);
								hx += buf;
								if ((bi & 3) == 3)
									hx += ' ';
							}
							SKSE::log::info("v256-dbg: v[{}] 40B: {}", vi, hx);
						}
					}
					// 脚印粗筛中心/半径：首尾顶点世界坐标包围盒 + margin
					const auto* v0 = reinterpret_cast<const float*>(raw);
					const auto* vn = reinterpret_cast<const float*>(
						raw + static_cast<std::size_t>(vc - 1) * stride);
					const float wx0 = v0[0] + lc.worldT[0], wy0 = v0[1] + lc.worldT[1];
					const float wxn = vn[0] + lc.worldT[0], wyn = vn[1] + lc.worldT[1];
					lc.centerX = (wx0 + wxn) * 0.5f;
					lc.centerY = (wy0 + wyn) * 0.5f;
					const float dxc = wxn - wx0, dyc = wyn - wy0;
					lc.halfDiag = std::sqrt(dxc * dxc + dyc * dyc) * 0.5f + 192.0f;
					found++;
				}
				cellCount++;
			}
		}
		if (found > 0) {
			// 新缓冲填完 → 原子切换 + 就绪
			landBufIdx.store(newIdx);
			// v569：**先切缓冲再更新 anchor**——原顺序 anchor 先 store、idx 后 store：
			// 渲染线程窗口内读"新 anchor + 旧 idx 缓冲" → cells 用旧缓冲但锚点已新
			// → 盖章偏移 1 帧。先切 idx（渲染线程 cells 引用新完整缓冲）再更新 anchor
			// （距离检测延迟 1 帧，无害）。
			landAnchorX.store(pos.x);
			landAnchorY.store(pos.y);
			landReady.store(true);
			landFootDirty.store(true);   // v197：新缓存 → 下一帧全量重算上传
			landRebuildPending.store(true);  // v567：修复——原 v197 注释说"FindLandscape 后置 true"但代码从未置位 → firstFullUp 永不重置 → 重缓存后沙丘基线不落 GPU
			// v435：场景雪堆场随重缓存重建（同线程，静态物碰撞只读安全）
			BuildSceneLift();
			SKSE::log::info("v130: landscape ready ({} geoms / {} cells at {:.0f},{:.0f})",
				found, cellCount, pos.x, pos.y);
			// v544d：**边界深色线诊断（v544b/v544c 后"依旧深色"）**——全量扫描玩家 cell
			// 4 quadrant 所有顶点：统计异常顶点（orig z==0 / |z|>5000 / 世界坐标越界）+
			// 边界线（cell 中心线 x/y = anchor±0）顶点高度对比。一次性。
			{
				static bool landEdgeLogged = false;
				if (!landEdgeLogged) {
					landEdgeLogged = true;
					for (int q = 0; q < 4; q++) {
						auto& lc = cells[24][q];
						if (!lc.orig.size())
							continue;
						int badZ0 = 0, badZhuge = 0, badCoord = 0;
						float minZ = 1.0e30f, maxZ = -1.0e30f;
						for (std::uint32_t vi = 0; vi < lc.verts; vi++) {
							const auto* f = reinterpret_cast<const float*>(
								lc.orig.data() + static_cast<std::size_t>(vi) * lc.stride);
							const float z = f[2];
							const float wx = f[0] + lc.worldT[0];
							const float wy = f[1] + lc.worldT[1];
							if (z == 0.0f)
								badZ0++;
							if (z != z || std::fabs(z) > 5000.0f)
								badZhuge++;
							if (wx != wx || wy != wy || std::fabs(wx) > 1.0e6f || std::fabs(wy) > 1.0e6f)
								badCoord++;
							minZ = std::min(minZ, z);
							maxZ = std::max(maxZ, z);
						}
						SKSE::log::info(
							"v544d: cell24 q{} verts={} z0={} zhuge={} badCoord={} zRange=({:.1f},{:.1f})",
							q, lc.verts, badZ0, badZhuge, badCoord, minZ, maxZ);
					}
					// 边界线（cell 中心线 x=anchorX / y=anchorY）顶点高度：q0|q1 垂直缝、
					// q0|q2 水平缝两侧顶点 z 对比（用世界坐标匹配，不依赖行列索引）
					for (int qa = 0; qa < 4; qa++) {
						auto& lc = cells[24][qa];
						if (!lc.orig.size())
							continue;
						int edgeX = 0, edgeY = 0;
						float exMin = 1.0e30f, exMax = -1.0e30f;
						float eyMin = 1.0e30f, eyMax = -1.0e30f;
						for (std::uint32_t vi = 0; vi < lc.verts; vi++) {
							const auto* f = reinterpret_cast<const float*>(
								lc.orig.data() + static_cast<std::size_t>(vi) * lc.stride);
							const float wx = f[0] + lc.worldT[0];
							const float wy = f[1] + lc.worldT[1];
							const float ax = lc.worldT[0], ay = lc.worldT[1];
							if (std::fabs(wx - ax) < 1.0f) {  // 中心线 x 上
								edgeX++;
								exMin = std::min(exMin, f[2]);
								exMax = std::max(exMax, f[2]);
							}
							if (std::fabs(wy - ay) < 1.0f) {  // 中心线 y 上
								edgeY++;
								eyMin = std::min(eyMin, f[2]);
								eyMax = std::max(eyMax, f[2]);
							}
						}
						SKSE::log::info(
							"v544d: cell24 q{} centerlineX n={} z=({:.1f},{:.1f}) centerlineY n={} z=({:.1f},{:.1f})",
							qa, edgeX, exMin, exMax, edgeY, eyMin, eyMax);
					}
				}
			}
			// v243：恢复高密网格重建触发（Smooth Terrain 已禁——引擎 65² 统一源，5×5 全 129²）
			// v273：**rebuild 阈值 2048 → 512（快照漂移修复）**——用户实锤"一开始走上去
			// v284b：**禁用 rebuild 触发**（Smooth Terrain 129² 直接改顶点，零替换零闪）
			// static float lastBuildX = -1.0e6f, lastBuildY = -1.0e6f;
			// const float buildDx = pos.x - lastBuildX;
			// const float buildDy = pos.y - lastBuildY;
			// if (!a_skipBuild &&
			// 	(!highResBuilt ||
			// 	 (buildDx * buildDx + buildDy * buildDy > 512.0f * 512.0f))) {
			// 	lastBuildX = pos.x;
			// 	lastBuildY = pos.y;
			// 	BuildHighResMesh(true);
			// }
			// v148：移除 v147 heights 实验（改引擎 LoadedLandData 数据 → 状态不同步
			// → 闪退，已回滚）——直接改引擎 heights 不是安全途径
			// v140：25 cell 状态矩阵——对照画面："低块"是否 = X（未缓存到）还是 O（缓存
			// 到了但被引擎刷新）→ 分清缓存不完整 vs 引擎刷新覆盖
			{
				std::string mat;
				for (int dy = -3; dy <= 3; dy++) {
					for (int dx = -3; dx <= 3; dx++) {
						const int ci = (dy + 3) * 7 + (dx + 3);
						bool ok = false;
						for (int q = 0; q < 4; q++)
							if (cells[ci][q].geom) {
								ok = true;
								break;
							}
						mat += ok ? 'O' : 'X';
					}
					if (dy < 2)
						mat += '|';
				}
				SKSE::log::info("v140: cells=[{}] (O=ok X=miss, rows N..S)", mat);
			}
			// v206：雪分类诊断——玩家 cell（ci=12）4 quad 的 isSnow（"深度不够"排查：
			// 若玩家脚下 quad 被判非雪 → 不挖坑 → 看起来浅）
			// v449：扩展——7×7 cells 全 quad 分类字符地图 + 玩家 cell 4 quad 的
			// diffuse 实际路径（材质检测失效排查：看哪些贴图被分错）。
			{
				std::string snowMat;
				for (int q = 0; q < 4; q++)
					snowMat += cells[24][q].surfaceClass == 1 ? 'S' :
						(cells[24][q].surfaceClass == 2 ? 'A' :
						(cells[24][q].surfaceClass == 3 ? 'M' : '-'));
				SKSE::log::info("v206: player-cell quads=[{}] (S=snow A=sand M=mud -=other)", snowMat);
				// 7×7 cells 分类地图（每 cell 4 quad 字符）
				std::string classMap;
				for (int dy = -3; dy <= 3; dy++) {
					for (int dx = -3; dx <= 3; dx++) {
						const int ci = (dy + 3) * 7 + (dx + 3);
						char cc = '?';
						for (int q = 0; q < 4; q++) {
							const int sc = cells[ci][q].surfaceClass;
							const char cq = sc == 1 ? 'S' : (sc == 2 ? 'A' : (sc == 3 ? 'M' : '.'));
							if (cq != '.') { cc = cq; break; }
						}
						classMap += cc;
					}
					classMap += '\n';
				}
				SKSE::log::info("v449: 7x7 surface map (rows N..S, cols W..E):\n{}", classMap);
				// v449-dbg：玩家 cell 4 quad 的实际 diffuse 路径 + 分类（材质误判定位）
				if (const auto playerD = RE::PlayerCharacter::GetSingleton()) {
					auto* tesD = RE::TES::GetSingleton();
					auto* cellD = tesD ? tesD->GetCell(playerD->GetPosition()) : nullptr;
					auto* ldd = (cellD && cellD->GetRuntimeData().cellLand &&
						cellD->GetRuntimeData().cellLand->loadedData) ?
						cellD->GetRuntimeData().cellLand->loadedData : nullptr;
					if (ldd) {
						for (int q = 0; q < 4; q++) {
							const char* p0 = nullptr;
							if (auto* dt = ldd->defQuadTextures[q])
								if (auto* ts = dt->textureSet)
									p0 = ts->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
							const char* p1 = nullptr;
							for (int li = 0; li < 6; li++) {
								if (auto* lt = ldd->quadTextures[q][li])
									if (auto* ts = lt->textureSet)
										if ((p1 = ts->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)))
											break;
							}
							SKSE::log::info("v449-dbg: q{} class={} def={} layer0={}",
								q, cells[24][q].surfaceClass,
								p0 ? p0 : "(none)", p1 ? p1 : "(none)");
						}
					}
				}
			}
			// v151：完整地块状态报告——25 cell 逐个打印（有无数据 / 4 geom 顶点数 /
			// raised 上传状态），对照画面定位"没用的地块"：
			//   land=N → 没缓存到（引擎无数据）
			//   g=[0...] → geom 缺失
			//   raised=N → 我们没上传成功
			//   raised=Y 但画面没用 → 引擎不用该 vb 渲染（渲染路径问题）
			{
				for (int dy = -3; dy <= 3; dy++) {
					for (int dx = -3; dx <= 3; dx++) {
						const int ci = (dy + 3) * 7 + (dx + 3);
						std::string gv, rv;
						bool any = false;
						for (int q = 0; q < 4; q++) {
							auto& cg = cells[ci][q];
							if (cg.geom && cg.verts > 0) {
								any = true;
								gv += std::to_string(cg.verts) + " ";
								rv += cg.raised ? "Y" : "N";
							} else {
								gv += "0 ";
								rv += "-";
							}
						}
						SKSE::log::info("v151: cell({},{}) land={} g=[{}] raised=[{}]",
							dx, dy, any ? "Y" : "N", gv, rv);
					}
				}
			}
			// v137：诊断——中央 cell（5×5 索引 ci=12）4 个 quadrant 的分辨率 +
			// 覆盖玩家（确认 65×65 是否跟随玩家 quadrant：玩家在 WS/WN/EN 时
			// 中央 cell 对应 quadrant 是否为 4225 = 跟随；恒为东北 = 固定）
			if (pcx != 0 || pcy != 0) {
				const float localX = pos.x - static_cast<float>(pcx) * kCellSize;
				const float localY = pos.y - static_cast<float>(pcy) * kCellSize;
				const char qn = localX > 2048.0f ? 'E' : 'W';
				const char qw = localY > 2048.0f ? 'N' : 'S';
				for (int qd = 0; qd < 4; qd++) {
					const auto& cg = cells[24][qd];
					if (!cg.geom || cg.verts < 2)  // v569：verts<2 防 (verts-1) uint32 下溢越界读
						continue;
					const float gx = cg.worldT[0], gy = cg.worldT[1];
					const auto* v0 = reinterpret_cast<const float*>(cg.orig.data());
					const auto* vn = reinterpret_cast<const float*>(
						cg.orig.data() + static_cast<std::size_t>(cg.verts - 1) * cg.stride);
					const float x0 = v0[0] + gx, y0 = v0[1] + gy;
					const float x1 = vn[0] + gx, y1 = vn[1] + gy;
					const bool cov = pos.x >= x0 && pos.x <= x1 && pos.y >= y0 && pos.y <= y1;
					SKSE::log::info("v137: center q{} verts={} bbox=({:.0f},{:.0f})..({:.0f},{:.0f}) covers={}",
						qd, cg.verts, x0, y0, x1, y1, cov ? "YES" : "no");
				}
				SKSE::log::info("v137: player local=({:.0f},{:.0f}) quad={}{}",
					localX, localY, qn, qw);
			}
		} else {
			landReady.store(false);
			SKSE::log::warn("v130: no landscape geoms found");
		}
	}

	// v266：**CPU 变形场**——用户方案"边界顶点链接一起变形"：所有网格（129² +
	// 引擎 289/65²）顶点从**同一张世界坐标变形场**采样 → 相邻 cell 边界顶点
	// （同一世界位置）变形量必然一致 → 无交界高低差 → 无裂缝，坑/雪堆可跨 cell
	// 连续（不再需要 v265 边缘 5 圈截断）。场覆盖玩家周围 7×7（28672 单位），
	// 每 32 单位一个采样点（896×896，坑半径 22 覆盖 2 采样点，双线性插值平滑）。
	// v465-dbg：RebuildField 物品脚印（shape==9）场写入统计（确认物品坑真实进场）
	// v573（用户"INI 玩家自行控制步数，默认最少"，2026-08-27）：**雪堆/脚印保留上限
	// 由 DynamicSnow.ini 的 MaxFootprints 控制**（LoadConfig 读取，默认 400）——
	// v545-perf 实锤 fp>400 后 RebuildField 5ms→10ms 拐点（成本∝脚印数），
	// 默认 400 = 保留 ~10s 轨迹 + 成本封顶 rf≤~5.5ms；玩家可调大（长轨迹）
	// 或调小（极致帧数）。范围 100-2000。
	static std::size_t gPlayerFpMax = 400;
	// v575 检测（用户"加检测代码看数据，别猜"，2026-08-27）：雪堆闪定位——
	// fadeMark=驱逐标记数（2s）、fadeWrote=淡出中写场脚印数、fadeAvg=平均 fade。
	// 判读：fadeMark 高 = 滚动快（尾部雪堆在换）；fadeAvg 高 = 淡出没生效；
	// fadeWrote≈0 且闪 = 闪不是淡出问题（查 geom 重建/场原点）。
	static std::atomic<long long> gFadeMarkN{ 0 };  // v574：驱逐淡出标记计数（v596 统一驱逐用）
	// v576（用户"直接改"，2026-08-27）：盖章→重建延迟检测——gDirtySetT 在盖章置
	// dirty 时记录，dirtyDue 帧重建时算差值。预期 v576 后 delayAvg ≈ 1 帧（16ms），
	// 之前 150ms 限频时 ≈150ms（= 雪堆"突然冒出"的闪的延迟来源）。
	static std::atomic<unsigned long> gDirtySetT{ 0 };
	static std::atomic<long long>     gDelaySum{ 0 }, gDelayN{ 0 };
	static std::atomic<unsigned long> gDelayMax{ 0 };
	// v604：**盖章同步精准检测（用户"保证移动和雪堆同步精准，不会延迟偏差"）**——
	// 按类型统计盖章数（0=玩家 1=NPC 2=马 3=狼/其他动物）+ 盖章→场重建延迟
	//（RebuildField 遍历脚印最新 tMs，now - maxTms = 最后盖章到本次重建的延迟，
	// 不依赖额外原子，天然覆盖所有盖章路径）。2s 输出供数据对比。
	static std::atomic<long long>     gStmpType[4]{ 0, 0, 0, 0 };
	static std::atomic<long long>     gDelay2Sum{ 0 }, gDelay2N{ 0 };
	static std::atomic<unsigned long> gDelay2Max{ 0 };
	static std::atomic<long long> gRebObjFoot{ 0 };
	static std::atomic<long long> gRebObjWrite{ 0 };

	// v573：**INI 配置加载**（游戏线程 SKSEPlugin_Load 调一次）——
	// Data/SKSE/Plugins/DynamicSnow.ini：
	//   [General]
	//   MaxFootprints=400   ; 雪堆/脚印保留上限（默认 400，fp>400 后场重建 5ms→10ms
	//                        ; 拐点；范围 100-2000，调大=长轨迹更吃帧，调小=更流畅）
	void SnowShellMesh::LoadConfig()
	{
		gPlayerFpMax = 400;  // 默认（性能拐点内）
		std::ifstream f("Data/SKSE/Plugins/DynamicSnow.ini");
		if (!f.is_open()) {
			SKSE::log::info("v573: DynamicSnow.ini not found, MaxFootprints=400 (default)");
			return;
		}
		std::string line;
		while (std::getline(f, line)) {
			if (line.find("MaxFootprints") == std::string::npos)
				continue;
			const auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const int v = std::atoi(line.c_str() + eq + 1);
			if (v >= 100 && v <= 2000) {
				gPlayerFpMax = static_cast<std::size_t>(v);
				SKSE::log::info("v573: MaxFootprints={} (from ini)", gPlayerFpMax);
			} else {
				SKSE::log::warn("v573: MaxFootprints={} out of range [100,2000], using 400", v);
			}
		}
	}
	void SnowShellMesh::RebuildField()
	{
		const int dim = kFieldDim;
		// v550：**obj 场条件清空（用户"极致省帧数，不损害效果"；rf 3.7-5.9ms 最大头
		// = 4 场 assign 20MB + 脚印循环）**——物品/武器脚印（shape>3）不存在时 obj
		// 场恒 0，跳过 assign（省一半内存带宽）。上次有本次无 → 清一次残留（场归 0）。
		// 玩家脚印为主（无物品滚动/武器敲击）时 rf 显著下降；有物品时行为不变。
		// v574：**清理淡出完成**（dieAt 标记后 2s 渐隐结束 → 真正删除）——
		// 淡出期间脚印保留在列表（场写入乘 fade 渐隐），完成后 remove_if 清除。
		// v592：**动物脚印过期淡出（狼群 rf 22ms 修复）**——v591 的 30s 半衰只让
		// 场写入值变小，但脚印项仍在列表撑大影响框（驱逐只在盖章时触发，盖章停后
		// 无人清理 → fp 涨到 420、rf 18-22ms 实锤）。shape=14 且存在 >30s → 标记
		// dieAt（2s 淡出）→ 下一轮 erase 删除 → 框收缩 rf 恢复。玩家/物品/武器
		//（shape<=3 或 9-13）不受影响。
		{
			std::lock_guard<std::mutex> lkF03(footMtx);
			const unsigned long nowDie = GetTickCount();
			for (auto& f : footprints) {
				// v601：30s→60s（尸体压痕/动物脚印痕迹更持久，用户要看效果；
				// obj 80 上限 + 驱逐兜底防堆积）
				if (f.shape == 14 && f.dieAt == 0 && f.tMs != 0 && nowDie - f.tMs > 60000)
					f.dieAt = nowDie;
			}
			// v596：**统一驱逐（玩家跑动卡顿根因修复）**——v553 的驱逐只在盖章处
			// 触发且 `objCnt>80 永远删 obj` → 只要生物多（obj 顶到 80 以上），玩家
			// 脚印（shape<=3）**无任何上限**无限堆积（fp 801→1184 实锤；玩家跑才
			// 盖章 → 玩家脚印堆积 → 全链路变慢 = "玩家跑才卡"）。改：玩家脚印
			// 独立上限 200、obj 独立上限 80，各自触发驱逐（最老优先，淡出 2s）。
			// 盖章处旧驱逐保留（双保险，只删 obj，不再影响玩家脚印）。
			int plyAlive = 0, objAlive = 0;
			for (const auto& f : footprints) {
				if (f.dieAt == 0) {
					if (f.shape <= 3)
						plyAlive++;
					else
						objAlive++;
				}
			}
			if (plyAlive > 200) {
				if (auto itP = std::find_if(footprints.begin(), footprints.end(),
					[](const Footprint& f) { return f.shape <= 3 && f.dieAt == 0; });
					itP != footprints.end())
					{ itP->dieAt = nowDie; gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }
			}
			if (objAlive > 60) {  // v605：80→60（动物/尸体脚印收紧）
				if (auto itO = std::find_if(footprints.begin(), footprints.end(),
					[](const Footprint& f) { return f.shape > 3 && f.dieAt == 0; });
					itO != footprints.end())
					{ itO->dieAt = nowDie; gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }
			}
			footprints.erase(std::remove_if(footprints.begin(), footprints.end(),
								[&](const Footprint& f) { return f.dieAt != 0 && nowDie - f.dieAt > 1600; }),  // v606：1200→1600（脚印持久些，视觉更好）
				footprints.end());
			// v604：**盖章→场重建延迟**（遍历最新 tMs：最后盖章到本次重建，覆盖
			// 玩家/动物/尸体所有盖章路径，天然无额外原子开销）——量化"移动和雪堆
			// 是否同步精准"。v603 去限频后预期 ≈ 1 帧（5-16ms）。
			unsigned long maxTms = 0;
			for (const auto& f : footprints) {
				if (f.tMs > maxTms)
					maxTms = f.tMs;
			}
			if (maxTms != 0) {
				const unsigned long nowT = GetTickCount();
				const unsigned long d2 = nowT > maxTms ? nowT - maxTms : 0;
				gDelay2Sum.fetch_add(d2, std::memory_order_relaxed);
				gDelay2N.fetch_add(1, std::memory_order_relaxed);
				unsigned long mx2 = gDelay2Max.load(std::memory_order_relaxed);
				while (d2 > mx2 && !gDelay2Max.compare_exchange_weak(mx2, d2, std::memory_order_relaxed)) {}
			}
		}
		bool hasObjFp = false;
		{
			std::lock_guard<std::mutex> lkF00(footMtx);
			for (const auto& fp : footprints) {
				if (fp.shape > 3) {
					hasObjFp = true;
					break;
				}
			}
		}
		static bool lastHasObj = false;
		const bool needObjClear = hasObjFp || lastHasObj;
		lastHasObj = hasObjFp;
		// v550b：**场分配始终确保（v550 闪退实锤：needObjClear=false 时跳过 assign
		// → obj 场空 vector → 平滑写回越界写崩 mov [r11+rdx*4]）**——4 场分配**始终**
		// 确保（size 检查，无条件——持续无 obj 时 obj 场也必须有效分配，采样/写回
		// 才不越界），清零按 v580 增量框（见下）。
		if (deformField.size() != static_cast<std::size_t>(dim) * dim)
			deformField.assign(static_cast<std::size_t>(dim) * dim, 0.0f);
		if (ridgeField.size() != static_cast<std::size_t>(dim) * dim)
			ridgeField.assign(static_cast<std::size_t>(dim) * dim, 0.0f);
		if (deformFieldObj.size() != static_cast<std::size_t>(dim) * dim)
			deformFieldObj.assign(static_cast<std::size_t>(dim) * dim, 0.0f);
		if (ridgeFieldObj.size() != static_cast<std::size_t>(dim) * dim)
			ridgeFieldObj.assign(static_cast<std::size_t>(dim) * dim, 0.0f);
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			fieldReady.store(false);
			return;
		}
		const auto pos = player->GetPosition();
		// 场原点对齐 kFieldStep 网格（世界坐标 → 采样索引稳定，跨 cell 连续）
		// v401：场覆盖一半 = kFieldDim*kFieldStep/2（@16 = 7168，玩家居中；旧
		// 3.5*4096=14336 是 @32 的值，@16 会导致玩家在场边缘）
		const float halfSpan = static_cast<float>(kFieldDim) * kFieldStep * 0.5f;
		fieldOriginX = std::floor((pos.x - halfSpan) / kFieldStep) * kFieldStep;
		fieldOriginY = std::floor((pos.y - halfSpan) / kFieldStep) * kFieldStep;
		const float step = kFieldStep;
		// v409：R 120→96（性能：land 4-5ms = RebuildField 循环量；脚印实际影响
		// 范围 = 战壕 17.8 + 雪脊 1.8×17.8≈32 + 椭圆 15 ≈ 64，R=96 覆盖有余 →
		// 循环量 (192²/240²) = 0.64×）
		// v438：**R 96→56（场分辨率 16→8 后控制循环量）**——影响区 (2×56/8)²
		// = 14²=196 格/脚印；坑 18 + 雪堆 2.2×18≈40 + margin → R=56 覆盖雪堆边缘。
		const float R = 56.0f;  // 脚印影响半径（坑 + 坑沿雪堆 + margin）
		// v580：**影响框增量清零（帧数优化，用户"继续检测帧数"）**——原全量 assign
		// 4 场（3584²×4B≈49MB ×4 ≈196MB 清零 ≈ rf 基础 ~6ms，与脚印数无关 = 最大
		// 浪费）。改：预扫本次影响框（curSmin/curSmax）→ 只清（上次框按场原点位移
		// 平移 ∪ 本次框）+ 1 格余量。轨迹框（脚印包围盒）通常 < 全场 1% → 基础
		// 成本 ~6ms → <1ms。安全网：首次（prev 未初始化）/ 传送（位移 > 2048 单位）
		// → 全量清零兜底。帧间 fieldOrigin 恒 step 整数倍位移（floor 对齐）→
		// shiftX/Y 用 lround 无损。淡出 erase 的脚印区域由 prev 框平移覆盖。
		int curSminGx = dim, curSminGy = dim, curSmaxGx = -1, curSmaxGy = -1;
		{
			std::lock_guard<std::mutex> lkF01(footMtx);
			for (const auto& fp : footprints) {
				const float minX = fp.x - R, maxX = fp.x + R;
				const float minY = fp.y - R, maxY = fp.y + R;
				int gx0 = static_cast<int>(std::floor((minX - fieldOriginX) / step));
				int gy0 = static_cast<int>(std::floor((minY - fieldOriginY) / step));
				int gx1 = static_cast<int>(std::ceil((maxX - fieldOriginX) / step));
				int gy1 = static_cast<int>(std::ceil((maxY - fieldOriginY) / step));
				gx0 = std::max(gx0, 0);
				gy0 = std::max(gy0, 0);
				gx1 = std::min(gx1, dim - 1);
				gy1 = std::min(gy1, dim - 1);
				if (gx0 < curSminGx) curSminGx = gx0;
				if (gy0 < curSminGy) curSminGy = gy0;
				if (gx1 > curSmaxGx) curSmaxGx = gx1;
				if (gy1 > curSmaxGy) curSmaxGy = gy1;
			}
		}
		const float dOx = fieldOriginX - prevFieldOriginX;
		const float dOy = fieldOriginY - prevFieldOriginY;
		const bool teleport = std::abs(dOx) > 2048.0f || std::abs(dOy) > 2048.0f;
		if (prevSminGx < 0 || teleport) {
			// 首次 / 传送（场原点跳变）→ 全量清零兜底
			std::fill(deformField.begin(), deformField.end(), 0.0f);
			std::fill(ridgeField.begin(), ridgeField.end(), 0.0f);
			if (needObjClear) {
				std::fill(deformFieldObj.begin(), deformFieldObj.end(), 0.0f);
				std::fill(ridgeFieldObj.begin(), ridgeFieldObj.end(), 0.0f);
			}
		} else {
			// 增量：并集（prev 框按位移平移 ∪ cur 框）+ 1 格余量（防平滑邻域越界）
			const int shiftX = static_cast<int>(std::lround(dOx / step));
			const int shiftY = static_cast<int>(std::lround(dOy / step));
			int clrX0 = std::min(curSminGx, prevSminGx + shiftX);
			int clrY0 = std::min(curSminGy, prevSminGy + shiftY);
			int clrX1 = std::max(curSmaxGx, prevSmaxGx + shiftX);
			int clrY1 = std::max(curSmaxGy, prevSmaxGy + shiftY);
			clrX0 = std::max(clrX0 - 1, 0);
			clrY0 = std::max(clrY0 - 1, 0);
			clrX1 = std::min(clrX1 + 1, dim - 1);
			clrY1 = std::min(clrY1 + 1, dim - 1);
			// v609：**越界保护**——快速旅行后残留脚印可能在当前场外（gx0/gx1 分别
			// 只钳下/上界 → curSmin>curSmax 框倒置），并集后 clrX0 可能 > clrX1 → 
			// std::fill 首指针越过尾指针 = 越界野写（v550b 同款 mov 写崩）。框倒置
			// 即无区域需清，跳过。
			if (clrX0 <= clrX1 && clrY0 <= clrY1) {
				for (int gy = clrY0; gy <= clrY1; gy++) {
					const auto row = static_cast<std::size_t>(gy) * dim;
					float* df = deformField.data() + row;
					float* rf = ridgeField.data() + row;
					std::fill(df + clrX0, df + clrX1 + 1, 0.0f);
					std::fill(rf + clrX0, rf + clrX1 + 1, 0.0f);
					if (needObjClear) {
						float* dfo = deformFieldObj.data() + row;
						float* rfo = ridgeFieldObj.data() + row;
						std::fill(dfo + clrX0, dfo + clrX1 + 1, 0.0f);
						std::fill(rfo + clrX0, rfo + clrX1 + 1, 0.0f);
					}
				}
			}
		}
		prevSminGx = curSminGx;
		prevSminGy = curSminGy;
		prevSmaxGx = curSmaxGx;
		prevSmaxGy = curSmaxGy;
		prevFieldOriginX = fieldOriginX;
		prevFieldOriginY = fieldOriginY;
		// v581：**影响框世界坐标（顶点循环框剔除用）**——框 = 预扫 curSmin/curSmax
		//（含脚印影响半径 R）→ 场只在框内非零 → 框外顶点跳过场采样/变形/ConeCS。
		// 无脚印（预扫哨兵）→ valid=false → 各循环走全量（安全兜底，理论不发生：
		// RebuildField 只在 dirty 帧调用，dirty 必有脚印）。
		if (curSminGx <= curSmaxGx && curSminGy <= curSmaxGy) {
			fieldBoxMinX = fieldOriginX + static_cast<float>(curSminGx) * step;
			fieldBoxMaxX = fieldOriginX + static_cast<float>(curSmaxGx + 1) * step;
			fieldBoxMinY = fieldOriginY + static_cast<float>(curSminGy) * step;
			fieldBoxMaxY = fieldOriginY + static_cast<float>(curSmaxGy) * step;
			fieldBoxValid = true;
		} else {
			fieldBoxValid = false;
		}
		// v438：脚印影响边界框（拉普拉斯平滑范围）——复用预扫 cur 框初始化，
		// 正式脚印循环内仍 min/max 记录（幂等，双保险）。
		int sminGx = curSminGx, sminGy = curSminGy, smaxGx = curSmaxGx, smaxGy = curSmaxGy;
		// v342：鞋底 mask 快照（mutex 读，渲染线程 RebuildField 每帧一次）
		ShapeStamp shL, shR;
		{
			std::lock_guard<std::mutex> lk(shapeMtx);
			shL = bootShape[0];
			shR = bootShape[1];
		}
		// v382：footprints 读锁（渲染线程 vs 游戏线程 ScanColliders 写）
		{
		std::lock_guard<std::mutex> lkF(footMtx);
		for (const auto& fp : footprints) {
			const float minX = fp.x - R, maxX = fp.x + R;
			const float minY = fp.y - R, maxY = fp.y + R;
			int gx0 = static_cast<int>(std::floor((minX - fieldOriginX) / step));
			int gy0 = static_cast<int>(std::floor((minY - fieldOriginY) / step));
			int gx1 = static_cast<int>(std::ceil((maxX - fieldOriginX) / step));
			int gy1 = static_cast<int>(std::ceil((maxY - fieldOriginY) / step));
			gx0 = std::max(gx0, 0);
			gy0 = std::max(gy0, 0);
			gx1 = std::min(gx1, dim - 1);
			gy1 = std::min(gy1, dim - 1);
			// v438：记录脚印影响边界框（拉普拉斯平滑范围）
			if (gx0 < sminGx) sminGx = gx0;
			if (gy0 < sminGy) sminGy = gy0;
			if (gx1 > smaxGx) smaxGx = gx1;
			if (gy1 > smaxGy) smaxGy = gy1;
			for (int gy = gy0; gy <= gy1; gy++) {
				for (int gx = gx0; gx <= gx1; gx++) {
					const float wx = fieldOriginX + static_cast<float>(gx) * step;
					const float wy = fieldOriginY + static_cast<float>(gy) * step;
					float d = 0.0f, r = 0.0f;
					// v477：**物品沟壑独立算法（用户"重写，宽深长可控"）**——
					// shape==9 完全独立于玩家战壕/椭圆/mask 逻辑，用 CS pr2659
					// 的干净胶囊公式：点到线段（prev→current）距离 < 宽度半轴 →
					// 中心满深 + smoothstep 边缘。
					//   **宽度** = fp.rS（盖章处 kObjWidth，直径 2×rS）
					//   **深度** = fp.depth（盖章处 kObjDepth）
					//   **长度** = prev→current 线段长（移动距离，投影 clamp 自动圆头）
					const float segDx = fp.x - fp.prevX;
					const float segDy = fp.y - fp.prevY;
					const float segLenSq = segDx * segDx + segDy * segDy;
					// v490：**删除物品独立分支——物品（shape==9）直接走玩家战壕段**
					//（同一段代码 = 分毫不差，物理上不可能有差异）。回填衰减在下方
					// 统一处理（shape==9 时 depth 乘 decay，玩家 tMs=0 不衰减）。
					float decay = 1.0f;
					// v574：**淡出渐隐**——dieAt 标记后 2s 线性 fade→0，乘入 decay
					//（decay 已覆盖全部 d/r 写入：战壕/椭圆/mask/雪堆环）→ 老脚印
					// 雪堆平滑消失，不再瞬间消失闪（erase 已改淡出标记）。
					if (fp.dieAt) {
						const float fade = std::clamp(1.0f - static_cast<float>(GetTickCount() - fp.dieAt) / 1600.0f, 0.0f, 1.0f);  // v606：1200→1600（脚印持久）
						decay *= fade;
					}
					if (fp.tMs != 0) {
						// v554：**玩家脚印也按时间回填（用户"玩家脚印也是啊"实锤）**——
						// 玩家脚印原来 tMs=0 永久 + fp>1000 erase 删最老 → 走路 40/s
						// 约 25 秒最早的脚印被挤掉（"几秒消失"真凶）。玩家脚印也记录
						// tMs + **600s 半衰**（≈30 分钟慢慢消失）→ fp 列表自然平衡
						//（600s 内脚印 ~1000 个），erase 几乎不触发 → 不再容量挤掉。
						// 物品/武器坑（shape>3）300s 半衰不变。
						const float age = static_cast<float>(GetTickCount() - fp.tMs) / 1000.0f;
						if (age > 0.0f) {
							// v591：**动物/NPC 脚印（shape=14）半衰 300→30s（狼群卡顿
							// 修复）**——狼群盖章多 + 300s 半衰 → obj 场堆积 + 影响框
							// 膨胀（拉普拉斯平滑/清零/顶点遍历全爆炸，rf 2ms→22ms
							// 实锤）。30s 半衰：狼群走过的痕迹 ~1 分钟内变浅消失 →
							// 场框自动收缩 → rf 恢复。玩家 600s / 物品武器 300s 不变。
							const float halfLife = (fp.shape <= 3) ? 600.0f : ((fp.shape == 14) ? 30.0f : 300.0f);
							decay = std::exp(-age / halfLife);
						}
					}
					// v499：**回退 v494 结构（用户"回退v494，环宽变成15试试"）**——
					// 撤销 v495-v498（16×16 印章 + 雪堆填壁 + m=4 方案——雪堆太
					// 高把坑填平/形状怪异）。回到 v494：**战壕椭圆横截面**
					//（rA=max(objR×2.2,30)=30、rC=max(objR×2.5,12)=20）+ falloff
					// (0.35,0.65) + 雪堆环 m=16。**环宽从 2.2rC(44) 改为 15 单位**
					//（环 1.0rC → 1.75rC，窄环贴坑）。间距 8/冷却 100（v497 连续
					// 条带）保留。
					// v511：**武器雪坑效果最大化（用户"效果最大看得清楚，后面再调"）**——
					// 独立检测（ScanPlayerMining）+ 独立分支（shape=10）。数值拉满：
					// objD 2.22→3.0（顶点 -54）、战壕 rA/rC 放大（rC≥24 → 48 宽）、
					// 雪堆 m 9→15（峰值 15×3=45）——绝对看得见，再逐步回调。
					// v519：**深度修正 + 雪堆不盖坑（v510b 数据实锤：ridgeMax=43.8 >>
					// SINK=18.7——雪堆把坑盖成雪包，用户"看不到坑"真凶）**：
					// 深坑 objD 3.0→**1.0**（-18 = 玩家脚印同深，用户"深度太大"）；
					// 拖痕 objD **0.5**（-9 单独浅）；雪堆 m **6**×objD×(1+0.15n)
					//（n=5 → 10.5，始终 < 坑 18——雪堆变化但不盖坑）。
					if (fp.shape == 10) {
						// v532：**攻击只堆雪不挖坑（用户"攻击时雪堆变化但是不要深坑"）**——
						// 敲击把雪敲隆起来（雪堆环），不再砸出下陷坑。雪堆 m=8×objD×(1+0.2n)
						//（n=同格连击次数）：n=1 → 峰值 9.6（顶点 ~6）、n=2 → 11.5、n=3 →
						// 13.4——**每次连击 +20%**（顶点 ~1.5/次，肉眼可见"每次攻击都有变化"）。
						const float objD = 1.0f;
						const float ridgeMul = 1.0f + 0.2f * fp.depth;
						const float segLen = std::sqrt(segLenSq);
						const float sdx = segLen > 0.0f ? segDx / segLen : 0.0f;
						const float sdy = segLen > 0.0f ? segDy / segLen : 0.0f;
						// 玩家战壕同款：rA/rC 由 rL/rS（武器半轴）计算
						const float rA = std::max(fp.rL * 2.2f, 30.0f);
						const float rC = std::max(fp.rS * 2.5f, 12.0f);
						float bestR = 0.0f;  // v532：只堆雪（bestD 移除）
						if (segLenSq > 1.0e-4f) {
							const float proj = (wx - fp.prevX) * segDx + (wy - fp.prevY) * segDy;
							const float t = std::clamp(proj / segLenSq, 0.0f, 1.0f);
							const float npx = fp.prevX + segDx * t;
							const float npy = fp.prevY + segDy * t;
							const float qx = wx - npx, qy = wy - npy;
							const float qAlong = qx * sdx + qy * sdy;
							const float qAcross = qx * (-sdy) + qy * sdx;
							const float dNorm2 = (qAlong / rA) * (qAlong / rA) + (qAcross / rC) * (qAcross / rC);
							if (dNorm2 >= 1.0f && dNorm2 < 2.6f * 2.6f) {
								const float tt = std::sqrt(dNorm2);
								const float st = (tt - 1.0f) / 1.6f;
								const float sinT = std::sin(st * 3.14159265f);
								bestR = sinT * sinT * 8.0f * objD * ridgeMul;
							}
						} else {
							const float dist = std::sqrt((wx - fp.x) * (wx - fp.x) + (wy - fp.y) * (wy - fp.y));
							if (dist > rC && dist < 2.6f * rC) {
								const float st = (dist - rC) / (1.6f * rC);
								const float sinT = std::sin(st * 3.14159265f);
								bestR = sinT * sinT * 8.0f * objD * ridgeMul;
							}
						}
						if (bestR > r) r = bestR;  // v532：只写雪堆，不写坑
						// v520：**拖痕（shape=11）对标物品（shape=9）所有数据（用户
						// "拖痕效果对标物体的所有数据"）**——拖痕不再用独立简化公式，
						// 直接走物品分支（同一段代码 = 分毫不差）：战壕 rA=30/rC=20
						//（40 宽）+ falloff(0.35,0.65) + 雪堆环 6rC（120 单位）
						// 峰值 m=9×objD（≈10）+ 坑深 objD=0.6×1.85 → -20 + 回填
						// 30s 半衰。盖章处已把拖痕 depth/rL/rS 改为物品同款（0.6/8/8）。
					// v559：**箭矢命中（shape=12）并入物品分支（同款参数：战壕 40 宽 +
					// 雪堆环 6rC + 300s 回填）**——箭落点 = 小坑 + 小雪堆（rL/rS=8 由盖章处传）。
					} else if (fp.shape == 9 || fp.shape == 11 || fp.shape == 12) {
						const float objR = fp.rS > 0.1f ? fp.rS : 8.0f;
						const float objD = fp.depth * 1.85f;  // v502：**坑深 20**（0.6×1.85=1.11 → -20，原 -28.8）
						const float segLen = std::sqrt(segLenSq);
						const float sdx = segLen > 0.0f ? segDx / segLen : 0.0f;
						const float sdy = segLen > 0.0f ? segDy / segLen : 0.0f;
						const float rA = std::max(objR * 2.2f, 30.0f);
						const float rC = std::max(objR * 2.5f, 12.0f);
						const float ridgeW = 5.0f;  // v503：**环外缘 4rC → 6rC（用户"变成6RC就可以"）**——雪堆铺到 120 单位
						float bestD = 0.0f, bestR = 0.0f;
						if (segLenSq > 1.0e-4f) {
							const float proj = (wx - fp.prevX) * segDx + (wy - fp.prevY) * segDy;
							const float t = std::clamp(proj / segLenSq, 0.0f, 1.0f);
							const float npx = fp.prevX + segDx * t;
							const float npy = fp.prevY + segDy * t;
							const float qx = wx - npx, qy = wy - npy;
							const float qAlong = qx * sdx + qy * sdy;
							const float qAcross = qx * (-sdy) + qy * sdx;
							const float dNorm2 = (qAlong / rA) * (qAlong / rA) + (qAcross / rC) * (qAcross / rC);
							if (dNorm2 < 1.0f) {
								const float tt = std::sqrt(dNorm2);
								const float s = (tt - 0.35f) / 0.65f;
								const float cl = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
								bestD = (1.0f - cl * cl * (3.0f - 2.0f * cl)) * objD * decay;
							}
							if (dNorm2 >= 1.0f && dNorm2 < (1.0f + ridgeW) * (1.0f + ridgeW)) {
								const float tt = std::sqrt(dNorm2);
								const float st = (tt - 1.0f) / ridgeW;
								const float sinT = std::sin(st * 3.14159265f);
								bestR = sinT * sinT * 9.0f * objD * decay;  // v502：m 16→9（峰值 9×1.11≈10）
							}
						} else {
							const float dist = std::sqrt((wx - fp.x) * (wx - fp.x) + (wy - fp.y) * (wy - fp.y));
							if (dist < rC) {
								const float s = (dist / rC - 0.35f) / 0.65f;
								const float cl = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
								bestD = (1.0f - cl * cl * (3.0f - 2.0f * cl)) * objD * decay;
							}
							if (dist > rC && dist < 6.0f * rC) {
								const float st = (dist - rC) / (5.0f * rC);
								const float sinT = std::sin(st * 3.14159265f);
								bestR = sinT * sinT * 9.0f * objD * decay;  // v503：环 4→6rC
							}
						}
						if (bestD > d) d = bestD;
						if (bestR > r) r = bestR;
						// v559：**法术爆炸（shape=13）**——大坑（半径 = fp.rS=40）+ 环形
						// 雪堆（环到 2.5rC，m=12×objD 爆炸隆起）——爆炸坑 + 炸起雪环。
					} else if (fp.shape == 13) {
						const float objR = fp.rS > 0.1f ? fp.rS : 40.0f;
						const float objD = fp.depth * 1.85f;  // 1.2×1.85=2.22 → -40 深爆炸坑
						const float rC = objR;
						const float ridgeW = 1.5f;  // 雪堆环到 2.5rC（爆炸隆起收敛，不铺太远）
						const float dist = std::sqrt((wx - fp.x) * (wx - fp.x) + (wy - fp.y) * (wy - fp.y));
						if (dist < rC) {
							const float s = (dist / rC - 0.35f) / 0.65f;
							const float cl = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
							const float bd = (1.0f - cl * cl * (3.0f - 2.0f * cl)) * objD * decay;
							if (bd > d) d = bd;
						}
						if (dist > rC && dist < (1.0f + ridgeW) * rC) {
							const float st = (dist - rC) / (ridgeW * rC);
							const float sinT = std::sin(st * 3.14159265f);
							const float br = sinT * sinT * 12.0f * objD * decay;
							if (br > r) r = br;
						}
						// v589：**NPC/动物脚印 = 玩家同款雪沟壑（用户"要和玩家的雪沟壑
						// 一样"）**——删除 v560 独立小坑分支（rL=6/rS=4 深-10），shape=14
						// 落入下方玩家战壕分支（胶囊线段 rA/rC 椭圆横截面 + 坑沿雪堆环）
						// → 与玩家脚印分毫不差（宽 40 深-18 + 雪堆 9.6）。场通道不变
						//（shape>3 → obj 场，玩家优先合成互不覆盖）。v588-dbg 统计保留
						//（场写入处 if fp.shape==14）。
					} else if (segLenSq > 1.0e-4f) {
						const float proj = (wx - fp.prevX) * segDx + (wy - fp.prevY) * segDy;
						const float t = std::clamp(proj / segLenSq, 0.0f, 1.0f);
						// v325：**胶囊覆盖 90% 线段**（原 0.25~0.75 只画中段 50% → 步距 65
						// 时战壕只有 33 单位长一段，两端 25% 靠椭圆（椭圆朝鞋头方向、不朝
						// 移动方向）→ 战壕断成"凹进去一块"（用户 13:06 反馈）。扩到
						// 0.05~0.95：战壕几乎覆盖整段 prev→curr（每步连续），两端 5% 由
						// 端点椭圆收尾（防线段末端穿帮）。
							const float npx = fp.prevX + segDx * t;
							const float npy = fp.prevY + segDy * t;
							const float qx = wx - npx, qy = wy - npy;
							// v294：胶囊横截面 = 真实鞋宽半轴 ×1.5（战壕宽 = 3×rS；
							// CS 战壕比单脚宽，挤出+滑动形成宽沟），保底 18（宽 36）
							// v341：**×1.5 去掉**——rS=18 → rTr=27 → 战壕宽 54，但端点
							// 椭圆短轴 eS=18 → 椭圆宽 36 → 中段宽两端窄 = 葫芦形怪异
							// （用户"形状奇怪"）。统一 rTr=eS=18 → 战壕宽 36=脚宽，
							// 与椭圆一致，坑形连续自然。
							// v343：战壕半径 = 脚宽半轴×1.15（脚 14 宽 → 战壕 ~16，挤出感）
							// v399：×1.15→×2.2（直径 35.6 = 1.1 场 texel）→ 顶点采样连续
							// （用户"走路沟壑不连续会断掉"实锤，1075 verts 连续验证）
							// v401：场 32→16 后 v410 收回 ×1.15（CS 对齐：CS stamp.w=真实
							// 半径 8.1，falloff 0.2-1.0 → 战壕直径 ~16；我们 rS=8.1×1.15=9.3
							// 直径 18.6 接近）——**但 CS 是像素级采样（密度无限），我们是
							// 几何顶点网格（129² 间距 16）**：战壕 18.6 < 2×16 → 每脚印只
							// 影响 0.5 顶点 → 坑稀疏点状断裂（v432 实测 319 stamps 只 169
							// verts = 用户"依旧没效果"视觉真相）。v433：**恢复 ×2.2**（直径
							// 35.6 ≈ 2.2 网格间距 → 每脚印 2-3 顶点 → 连续沟壑，v399 验证）。
							// v436：**鞋形战壕（mask 扫掠，以鞋 MESH 为基准）**——碰撞体盖章
							// 已带 fp.shape=1/2（v436 ScanColliders 匹配 bootShape）。战壕不再
							// 是圆胶囊：采样点相对轨迹最近点 (npx,npy)，用鞋固定朝向 (sh.dir)
							// 投影到鞋局部 (u,v) → 查鞋底 mask（64×64）：
							//   mask 内 = 鞋形沟壑满深；mask 边 1 cell = 鞋边雪堆。
							// 轨迹线上每点铺一个鞋形切片 = 鞋沿移动方向扫出的真实沟壑
							// （鞋头窄/鞋跟窄/足弓空，全按真实鞋底形状）。
							const bool maskMode = (fp.shape == 1 || fp.shape == 2);
							const ShapeStamp& shT = maskMode ? ((fp.shape == 1) ? shL : shR) : shL;
							if (maskMode && shT.valid && shT.maskDim > 0 && !shT.mask.empty()) {  // v569：与 1853 防御一致
								const float al = (wx - npx) * shT.dirX + (wy - npy) * shT.dirY;
								const float ac = (wx - npx) * (-shT.dirY) + (wy - npy) * shT.dirX;
								const float uf = (al / shT.len + 0.5f) * shT.maskDim;
								const float vf = (ac / shT.wid + 0.5f) * shT.maskDim;
								const int u = static_cast<int>(uf);
								const int v = static_cast<int>(vf);
								if (u >= 0 && u < shT.maskDim && v >= 0 && v < shT.maskDim) {
									if (shT.mask[static_cast<std::size_t>(v) * shT.maskDim + u] > 0.5f) {
										// v609：漏乘 decay（鞋形主坑与周边渐变不一致）——与 2109 鞋底分支一致
										if (fp.depth * decay > d) d = fp.depth * decay;
									} else {
										bool isNear = false;
										for (int dv = -1; dv <= 1 && !isNear; ++dv)
											for (int du = -1; du <= 1 && !isNear; ++du) {
												const int nu = u + du, nv = v + dv;
												if (nu >= 0 && nu < shT.maskDim && nv >= 0 && nv < shT.maskDim &&
													shT.mask[static_cast<std::size_t>(nv) * shT.maskDim + nu] > 0.5f)
													isNear = true;
											}
									if (isNear) {
											const float m = 9.0f * fp.depth * decay;  // v442b：鞋边雪堆 16→9（随坑沿 22→12 同比例降，别盖坑）；v609 补 decay
										if (m > r) r = m;
									}
									}
								}
								// mask 边缘外不做任何（鞋形战壕无圆环；端点由椭圆鞋印收尾）
							} else {
							// v437b：**椭圆战壕（碰撞体形状半轴，安全替代网格 mask）**
							// q 投影到轨迹方向（prev→curr）：沿轨迹半轴 rA（≈鞋长半，
							// 放宽让战壕沿线平滑）、垂直半轴 rC（≈鞋宽半，战壕宽度）。
							// 横截面椭圆 = 脚碰撞体（胶囊/盒）水平截面 → 鞋形感。
							// fp.rL/rS 来自 v437b 盖章的 GetMaximumProjection 投影。
							const float segLen = std::sqrt(segLenSq);
							const float sdx = segDx / segLen, sdy = segDy / segLen;
							const float qAlong = qx * sdx + qy * sdy;
							const float qAcross = qx * (-sdy) + qy * sdx;
							// v457：**物品脚印（shape==9）独立窄 falloff（用户"物品沟壑
							// 宽度非常大/老样子"实锤）**——原 rC=max(rS×2.5,12) 把物品
							// 盖章放大 2.5 倍 + 保底 12 → 直径 54 大块。物品滚动是**线
							// 接触**：rA/rC = rS×1.2、无保底（贴合物品实际宽度）。
							// v462：**物品 rC 保底 24→20（用户"起码和人走路沟壑差不多"）**——
							// 玩家脚印 rC=max(8.1×2.5,12)=20.25（直径 ~40，用户认可的宽度）；
							// 物品保底 20 = 同宽。且 20>16（顶点间距）覆盖 2 顶点圆润。
							// v490：删物品特判——物品/玩家统一战壕公式（物品 rL=rS=8 → rA=30/rC=20）
							const float rA = std::max(fp.rL * 2.2f, 30.0f);  // 沿轨迹放宽（端点椭圆收尾）
							const float rC = std::max(fp.rS * 2.5f, 12.0f);  // v438b：×2.2→×2.5、保底 10→12（用户"宽一点点"）——垂直轨迹 = 战壕半宽
							// v438：**端点去收缩（修复"不连续"）**——v437b 的 t>0.9 突变
							// 收窄（rC→eS）让战壕端点收成尖 + 与端点椭圆接合断裂。去掉：
							// rTr 恒 rC，战壕到端点自然被端点椭圆（eL/eS≈rC）覆盖 → 无缝。
							const float rTr = rC;
							const float dNorm2 = (qAlong / rA) * (qAlong / rA) +
								(qAcross / rTr) * (qAcross / rTr);
							if (dNorm2 < 1.0f) {
								const float tt = std::sqrt(dNorm2);
								// v438e：**坑壁改陡 0.2/0.8 → 0.35/0.65（让凹陷俯视可见）**——
								// 用户"需要真实地面凹进去"：缓坡（0.2-1.0 平滑）在俯视视角
								// 无阴影分界 → 凹陷看不清。陡壁（0.35 半径内满深、0.35-1.0
								// 陡降）→ 坑沿有明确明暗线（v297 近垂直被嫌太方，取中度）。
								const float s = (tt - 0.35f) / 0.65f;
								const float clamped = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
								const float fall = 1.0f - clamped * clamped * (3.0f - 2.0f * clamped);
								// v430：**坑乘 fp.depth（回填渐变）**——v250 压缩度模型
								// 定义 d = fp->depth×fall（脚印强度×形状），但此路径一直
								// 漏乘 depth（只雪脊/鞋边乘了）→ 回填只靠 erase（1 天后
								// 突然消失），坑不会渐变变浅（用户"回弹至少一天"= 渐变）。
								if (fall * fp.depth * decay > d) d = fall * fp.depth * decay;
							} else {
								// v343：**战壕两侧雪脊（CS berm）**——只留贴边细脊（1~1.8r，
								// 高 3×depth），删掉大环。CS 视频里雪是被脚边挤出的细长
								// 小脊，不是一圈宽包。
								// v435：**坑沿雪堆增强（用户"踩雪堆雪"）**——雪脊 3→14
								// （坑深 42，比例 ~1/3 接近 CS 视觉）、宽度 1.8→2.2rTr
								// （更宽更明显的雪堆环）。合成改叠加（v435 UpdateLandscape）
								// 后坑沿净隆起 ≈ 14——走出来的雪路 = 中间沟壑 + 两侧雪堆。
								// v437b：tt 用椭圆归一化距离（坑沿雪堆环沿椭圆）。
								const float tt = std::sqrt(dNorm2);
								if (tt > 1.0f && tt < 2.6f) {  // v438k：雪堤加宽 2.2→2.6r（真实挤出是宽矮堤，不是细脊）
									const float st = (tt - 1.0f) / 1.6f;
									const float sinT = std::sin(st * 3.14159265f);
									const float fade = sinT * sinT;
									// v470：**物品坑沿雪堆关闭（用户"宽而浅"实锤）**——
									// 战壕雪堆环到 2.6r（隆起区 1.6 倍半径）→ 视觉宽度
									// 2.6×r≈33（坑 12.6 + 隆起圈）——物品滚动不该有
									// 挤出雪堆，只留细坑。shape==9 → m=0（不隆起）。
									// v490：删物品特判——物品/玩家统一雪堆 12（物品滚过也是雪堆挤出）
									const float m = 12.0f * fade * fp.depth * decay;  // v442b：坑沿雪堆 22→12（v442 数据实锤：22 > 坑深 18，平滑摊进坑中心抵消凹陷 → deepest=-4/SINK=1.7。12 ≈ 坑深 26 的 46%，体积守恒）
									if (m > r) r = m;
								}
							}
							}
					}
					// 端点椭圆鞋印 + 雪堆（1~3r sin² 钟形）
					const float dx = wx - fp.x, dy = wy - fp.y;
					const float along = dx * fp.dirX + dy * fp.dirY;
					const float across = dx * (-fp.dirY) + dy * fp.dirX;
					// v294：椭圆半轴 = 检测到的真实鞋尺寸（v288 FindBootMesh 存入
					// fp.rL/rS；兜底 60/36 全长 → 半轴 30/18 = 原值）。**数据闭环**：
					// 之前写死 34/22 → 检测的鞋尺寸从未生效 → 雪堆也随固定椭圆失真。
					// v463：**物品盖章真正生效点**——物品脚印 prev==current（无线段）
					// 走"端点椭圆"分支（不走战壕 rA/rC——v457-462 改的战壕对物品无效
					// 实锤）。物品椭圆半轴 = max(sr, 20)（对齐玩家沟壑宽 40）；玩家
					// 脚印不受影响（eS=8.1 鞋宽保留）。
					float eL = fp.rL > 1.0f ? fp.rL : 15.0f;
					float eS = fp.rS > 1.0f ? fp.rS : 8.0f;
					// v490：删物品保底特判——物品/玩家统一椭圆（物品 rS=8 → eS=8 同玩家）
					const float dL = along / eL, dS = across / eS;
					const float dNorm2 = dL * dL + dS * dS;
					// v595：**武器（shape=10）端点椭圆不写坑（用户"武器挖坑太大，
					// 直接凹进去了"）**——v532 让 shape=10 分支只堆雪（bestR 无坑），
					// 但单点攻击（prev=current → segLenSq≈0）走端点椭圆，这里无条件
					// 写 d（坑）→ 武器砸出大深坑（rL=8 → 椭圆 ~30×16 + 满深）。排除
					// shape=10 后：武器只剩雪堆隆起（v532 设计），不再凹进去。
					if (dNorm2 < 1.0f && fp.shape != 10) {
						const float t = std::sqrt(dNorm2);
						// v329：**坑壁改平滑**——v297 改 (t-0.45)/0.55（45-100% 近垂直切面 =
						// CS 雪墙感）但用户反馈"凹陷没有很好看"（太方、不像自然雪坑）。
						// 改回 (t-0.2)/0.8 平滑过渡（20-100%，边缘缓坡更自然）。
						// v438e：**0.2/0.8 → 0.35/0.65（凹陷俯视可见，同战壕）**——
						// 深度已 100（v438d），缓坡在俯视视角无明暗分界 → 凹陷看不清；
						// 中度陡壁（35% 半径内满深）给坑沿明确阴影线，雪堆+凹陷都显眼。
						const float s = (t - 0.35f) / 0.65f;
						const float clamped = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
						float fall = 1.0f - clamped * clamped * (3.0f - 2.0f * clamped);
						// v285：**足弓马鞍形**——真实步态：脚跟着地→前掌蹬地，足弓
						// 不触地 → 前掌（aN>0.25）和后跟（aN<-0.45）满深，足弓（中间）
						// 只 50% 深。smoothstep 过渡避免硬边。
						const float aN = along / eL;  // -1..1（鞋头方向 +）
						const float archHi = std::clamp((aN - 0.0f) / 0.25f, 0.0f, 1.0f);   // 前掌 1
						const float archLo = std::clamp((aN + 0.45f) / 0.35f, 0.0f, 1.0f);  // 后跟 1
						const float arch = 0.5f + 0.5f * std::max(archHi, archLo);          // 足弓 0.5
						fall *= arch;
						// v430：椭圆鞋印同乘 fp.depth（回填渐变，见战壕注释）
						// v490b：**+decay**——物品首个盖章（prev=current 单点）走椭圆
						// 分支，之前漏乘 decay → 起始点永不回填（永久小点）。玩家
						// tMs=0 → decay=1 无影响。
						if (fall * fp.depth * decay > d) d = fall * fp.depth * decay;
					}
					// v599：**尸体雪丘环（用户选"结合"——中间凹坑 + 周围雪丘埋尸，
					// 对齐 CS CombineCS 埋尸雪丘思路）**——尸体压痕（shape=14 单点，
					// segLenSq≈0）在椭圆凹坑外堆一圈雪丘（sin² 钟形，坑边 rS →
					// 2.8×rS），模拟雪慢慢埋住尸体形成的鼓包。活体动物脚印（shape=14
					// 但 segLen>0）走战壕分支不进来。decay 乘入（30s 半衰雪丘同步变浅）。
					if (fp.shape == 14 && segLenSq <= 1.0e-4f) {
						const float dist2 = std::sqrt(dx * dx + dy * dy);
						const float rC2 = std::max(fp.rS, 8.0f);
						if (dist2 > rC2 && dist2 < 2.8f * rC2) {
							const float st2 = (dist2 - rC2) / (1.8f * rC2);
							const float sinT2 = std::sin(st2 * 3.14159265f);
							const float mound = sinT2 * sinT2 * 24.0f * fp.depth * decay;  // v601：雪丘 20→24（配合 depth 0.6 → ~14 隆起，埋尸感）
							if (mound > r) r = mound;
						}
					}
					// v342：**鞋底形状 mask**（fp.shape=1/2，游戏线程扫描的鞋网格投影）——
					// 精确鞋形凹陷（鞋底内满深）+ 鞋边挤出雪堆。mask 用鞋朝向（sh.dir）
					// 栅格化（鞋形朝鞋头），叠加在胶囊/椭圆之上（鞋形满深嵌在战壕里）。
					if (fp.shape == 1 || fp.shape == 2) {
						const ShapeStamp& sh = (fp.shape == 1) ? shL : shR;
						if (sh.valid && sh.maskDim > 0 && !sh.mask.empty()) {
							const float al = (wx - fp.x) * sh.dirX + (wy - fp.y) * sh.dirY;
							const float ac = (wx - fp.x) * (-sh.dirY) + (wy - fp.y) * sh.dirX;
							const float uf = (al / sh.len + 0.5f) * sh.maskDim;
							const float vf = (ac / sh.wid + 0.5f) * sh.maskDim;
							const int u = static_cast<int>(uf);
							const int v = static_cast<int>(vf);
							if (u >= 0 && u < sh.maskDim && v >= 0 && v < sh.maskDim) {
								const float mv = sh.mask[static_cast<std::size_t>(v) * sh.maskDim + u];
								if (mv > 0.5f) {
									// v430：鞋底满深同乘 fp.depth（回填渐变，见战壕注释）
									// v569：漏乘 decay——战壕/椭圆分支都乘了，鞋形主坑永不衰减 → 永久坑
									d = fp.depth * decay;  // 鞋底内满深（真实鞋形，比椭圆更精确）
								} else {
									// 鞋边 1 cell 内 → 挤出雪堆（黑神话鞋印周围隆起）
									bool isNear = false;
									for (int dy2 = -1; dy2 <= 1 && !isNear; ++dy2)
										for (int dx2 = -1; dx2 <= 1 && !isNear; ++dx2) {
											const int nu = u + dx2, nv = v + dy2;
											if (nu >= 0 && nu < sh.maskDim && nv >= 0 && nv < sh.maskDim &&
												sh.mask[static_cast<std::size_t>(nv) * sh.maskDim + nu] > 0.5f)
												isNear = true;
										}
									if (isNear) {
										const float m = 9.0f * fp.depth * decay;  // v442b：鞋边雪堆 16→9（椭圆 mask 分支）；v609 补 decay
										if (m > r) r = m;
									}
								}
							}
						}
					}
					// v529：**场通道分离（用户"武器盖章覆盖脚下导致脚印变化，互不影响"）**——
					// 玩家脚印（shape≤3）写 deformField/ridgeField；物品/拖痕/深坑
					//（shape 9/10/11）写 deformFieldObj/ridgeFieldObj——各自独立，
					// 合成时玩家优先（UpdateLandscape）→ 脚印区域显示脚印，无脚印
					// 区域显示拖痕/物品/深坑，互不覆盖。
					auto& df = (fp.shape > 3) ? deformFieldObj[static_cast<std::size_t>(gy) * dim + gx] :
						deformField[static_cast<std::size_t>(gy) * dim + gx];
					auto& rf = (fp.shape > 3) ? ridgeFieldObj[static_cast<std::size_t>(gy) * dim + gx] :
						ridgeField[static_cast<std::size_t>(gy) * dim + gx];
					if (d > df) df = d;
					if (r > rf) rf = r;
					// v465-dbg：物品脚印（shape==9）实际场写入统计
					if (fp.shape == 9) {
						gRebObjFoot.fetch_add(1, std::memory_order_relaxed);
						if (d > 0.0f || r > 0.0f)
							gRebObjWrite.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
		}
		}  // v382: footMtx unlock (fp loop scope)

		// v438：**拉普拉斯平滑（对齐 CS HeightMapProcessCS blur）**——用户"三角尖刺
		// 感很大/不圆润"：坑半径 10~18 在场 8 格上只覆盖 3-5 点，双线性采样仍会
		// 出小尖峰；CS 深度图有 blur pass 摊平。这里对 deform/ridge 做 1 次轻平滑：
		// new = 0.5×old + 0.5×(4 邻域均值)——保留峰值（坑深不塌）同时抹平三角尖。
		// 只做脚印影响边界框（smin..smax），避开全场 320 万格。
		// v593：**平滑限幅到玩家 ±1024 单位（rf 3ms→68ms 帧数波动修复）**——
		// 平滑遍历影响框 O(面积)：玩家跑图越广（脚印 600s 半衰散布越远）→ 框
		// 越大 → 平滑越慢（实测 rf 随跑图 4 分钟从 3ms 单调涨到 68ms 实锤）。
		// 平滑只在视觉重点区（玩家 ±1024 单位 = 256 格）做，远处脚印的三角尖刺
		// 玩家看不见，不平滑无妨 → 平滑成本 O(框) → O(256²) 封顶 ~3ms。
		{
			const int limX0 = static_cast<int>(std::floor((pos.x - 1024.0f - fieldOriginX) / step));
			const int limY0 = static_cast<int>(std::floor((pos.y - 1024.0f - fieldOriginY) / step));
			const int limX1 = static_cast<int>(std::ceil((pos.x + 1024.0f - fieldOriginX) / step));
			const int limY1 = static_cast<int>(std::ceil((pos.y + 1024.0f - fieldOriginY) / step));
			sminGx = std::max(sminGx, limX0);
			sminGy = std::max(sminGy, limY0);
			smaxGx = std::min(smaxGx, limX1);
			smaxGy = std::min(smaxGy, limY1);
		}
		if (smaxGx >= sminGx && smaxGy >= sminGy) {
			const int bw = smaxGx - sminGx + 1;
			const int bh = smaxGy - sminGy + 1;
			std::vector<float> dSmooth(static_cast<std::size_t>(bw) * bh, 0.0f);
			std::vector<float> rSmooth(static_cast<std::size_t>(bw) * bh, 0.0f);
			std::vector<float> dSmoothObj(static_cast<std::size_t>(bw) * bh, 0.0f);  // v529：物体场平滑
			std::vector<float> rSmoothObj(static_cast<std::size_t>(bw) * bh, 0.0f);
			auto boxIdx = [&](int bx, int by) {
				return static_cast<std::size_t>(by) * bw + bx;
			};
			for (int by = 0; by < bh; by++) {
				for (int bx = 0; bx < bw; bx++) {
					const int gx = sminGx + bx;
					const int gy = sminGy + by;
					const auto i = static_cast<std::size_t>(gy) * dim + gx;
					const float dv = deformField[i];
					const float rv = ridgeField[i];
					// v550：无 obj 脚印时跳过 obj 平滑（obj 场已清 0，结果仍 0）
					float dvO = 0.0f, rvO = 0.0f;
					if (hasObjFp) {
						dvO = deformFieldObj[i];  // v529
						rvO = ridgeFieldObj[i];
					}
					// 4 邻域均值（边界 clamp）
					float dSum = dv, rSum = rv;
					float dSumO = dvO, rSumO = rvO;
					int cnt = 1;
					if (gx > 0) { dSum += deformField[i - 1]; rSum += ridgeField[i - 1]; if (hasObjFp) { dSumO += deformFieldObj[i - 1]; rSumO += ridgeFieldObj[i - 1]; } cnt++; }
					if (gx < dim - 1) { dSum += deformField[i + 1]; rSum += ridgeField[i + 1]; if (hasObjFp) { dSumO += deformFieldObj[i + 1]; rSumO += ridgeFieldObj[i + 1]; } cnt++; }
					if (gy > 0) { dSum += deformField[i - dim]; rSum += ridgeField[i - dim]; if (hasObjFp) { dSumO += deformFieldObj[i - dim]; rSumO += ridgeFieldObj[i - dim]; } cnt++; }
					if (gy < dim - 1) { dSum += deformField[i + dim]; rSum += ridgeField[i + dim]; if (hasObjFp) { dSumO += deformFieldObj[i + dim]; rSumO += ridgeFieldObj[i + dim]; } cnt++; }
					dSmooth[boxIdx(bx, by)] = 0.5f * dv + 0.5f * (dSum / static_cast<float>(cnt));
					rSmooth[boxIdx(bx, by)] = 0.5f * rv + 0.5f * (rSum / static_cast<float>(cnt));
					dSmoothObj[boxIdx(bx, by)] = 0.5f * dvO + 0.5f * (dSumO / static_cast<float>(cnt));
					rSmoothObj[boxIdx(bx, by)] = 0.5f * rvO + 0.5f * (rSumO / static_cast<float>(cnt));
				}
			}
			for (int by = 0; by < bh; by++) {
				for (int bx = 0; bx < bw; bx++) {
					const int gx = sminGx + bx;
					const int gy = sminGy + by;
					const auto i = static_cast<std::size_t>(gy) * dim + gx;
					deformField[i] = dSmooth[boxIdx(bx, by)];
					ridgeField[i] = rSmooth[boxIdx(bx, by)];
					deformFieldObj[i] = dSmoothObj[boxIdx(bx, by)];  // v529
					ridgeFieldObj[i] = rSmoothObj[boxIdx(bx, by)];
				}
			}
		}
		fieldReady.store(true);
	}

	void SnowShellMesh::SampleField(float wx, float wy, float& deformOut, float& ridgeOut) const
	{
		deformOut = 0.0f;
		ridgeOut = 0.0f;
		if (!fieldReady.load() || deformField.empty())
			return;
		// v429（0.5 分支）：**恢复 v296 双线性采样**（v423 的 B-spline 撤销）——
		// 0.5 原版就是双线性（v296 无 B-spline），恢复原版采样行为。
		const float step = kFieldStep;
		const float fx = (wx - fieldOriginX) / step;
		const float fy = (wy - fieldOriginY) / step;
		const int x0 = static_cast<int>(std::floor(fx));
		const int y0 = static_cast<int>(std::floor(fy));
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const int dim = kFieldDim;
		if (x0 < 0 || y0 < 0 || x0 >= dim - 1 || y0 >= dim - 1) {
			// 场外（7×7 外）：clamp 到边缘值（引擎网格此处不渲染，兜底）
			const int cx = std::clamp(x0, 0, dim - 1);
			const int cy = std::clamp(y0, 0, dim - 1);
			const auto i0 = static_cast<std::size_t>(cy) * dim + cx;
			deformOut = deformField[i0];
			ridgeOut = ridgeField[i0];
			return;
		}
		const auto i00 = static_cast<std::size_t>(y0) * dim + x0;
		const auto i10 = static_cast<std::size_t>(y0) * dim + x0 + 1;
		const auto i01 = static_cast<std::size_t>(y0 + 1) * dim + x0;
		const auto i11 = static_cast<std::size_t>(y0 + 1) * dim + x0 + 1;
		deformOut = (deformField[i00] * (1.0f - tx) + deformField[i10] * tx) * (1.0f - ty) +
			(deformField[i01] * (1.0f - tx) + deformField[i11] * tx) * ty;
		ridgeOut = (ridgeField[i00] * (1.0f - tx) + ridgeField[i10] * tx) * (1.0f - ty) +
			(ridgeField[i01] * (1.0f - tx) + ridgeField[i11] * tx) * ty;
	}

	// v529：**SampleFieldObj（物品/拖痕/深坑场采样，与 SampleField 同款双线性）**——
	// 场通道分离：玩家脚印在 deformField/ridgeField，物体变形在 obj 场。
	void SnowShellMesh::SampleFieldObj(float wx, float wy, float& deformOut, float& ridgeOut) const
	{
		deformOut = 0.0f;
		ridgeOut = 0.0f;
		if (!fieldReady.load() || deformFieldObj.empty())
			return;
		const float step = kFieldStep;
		const float fx = (wx - fieldOriginX) / step;
		const float fy = (wy - fieldOriginY) / step;
		const int x0 = static_cast<int>(std::floor(fx));
		const int y0 = static_cast<int>(std::floor(fy));
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const int dim = kFieldDim;
		if (x0 < 0 || y0 < 0 || x0 >= dim - 1 || y0 >= dim - 1) {
			const int cx = std::clamp(x0, 0, dim - 1);
			const int cy = std::clamp(y0, 0, dim - 1);
			const auto i0 = static_cast<std::size_t>(cy) * dim + cx;
			deformOut = deformFieldObj[i0];
			ridgeOut = ridgeFieldObj[i0];
			return;
		}
		const auto i00 = static_cast<std::size_t>(y0) * dim + x0;
		const auto i10 = static_cast<std::size_t>(y0) * dim + x0 + 1;
		const auto i01 = static_cast<std::size_t>(y0 + 1) * dim + x0;
		const auto i11 = static_cast<std::size_t>(y0 + 1) * dim + x0 + 1;
		deformOut = (deformFieldObj[i00] * (1.0f - tx) + deformFieldObj[i10] * tx) * (1.0f - ty) +
			(deformFieldObj[i01] * (1.0f - tx) + deformFieldObj[i11] * tx) * ty;
		ridgeOut = (ridgeFieldObj[i00] * (1.0f - tx) + ridgeFieldObj[i10] * tx) * (1.0f - ty) +
			(ridgeFieldObj[i01] * (1.0f - tx) + ridgeFieldObj[i11] * tx) * ty;
	}

	// v435：前向声明（GetColliderBound 定义在 2449 行——v346 Havok 碰撞体包围球，
	// 只读碰撞体安全，BuildSceneLift 需在本定义之前调用）
	static bool GetColliderBound(RE::bhkNiCollisionObject* a_obj, RE::NiPoint3& a_center, float& a_radius);

	// v564（帧数优化，2026-08-27）：**最近邻采样**——原双线性 4 次场读取（tx/ty
	// 插值）。kFieldStep=4 且场原点对齐 4 网格（v2147），quadrant 顶点世界坐标 =
	// cellX*4096 + 2048 + i*step（32/64/128 全 4 的倍数）→ 顶点精确落在场点上 →
	// 最近邻 = 双线性在网格点上的精确值（零误差）。floor(fx+0.5) 四舍五入取最近
	// 场点（防浮点误差差半格）。热路径（顶点循环每帧 8-15 万次 ×2 场）4 读→1 读。
	void SnowShellMesh::SampleFieldNearest(float wx, float wy, float& deformOut, float& ridgeOut) const
	{
		deformOut = 0.0f;
		ridgeOut = 0.0f;
		if (!fieldReady.load() || deformField.empty())
			return;
		const float fx = (wx - fieldOriginX) / kFieldStep;
		const float fy = (wy - fieldOriginY) / kFieldStep;
		const int x0 = static_cast<int>(std::floor(fx + 0.5f));
		const int y0 = static_cast<int>(std::floor(fy + 0.5f));
		const int dim = kFieldDim;
		if (x0 < 0 || y0 < 0 || x0 >= dim || y0 >= dim) {
			const int cx = std::clamp(x0, 0, dim - 1);
			const int cy = std::clamp(y0, 0, dim - 1);
			const auto i0 = static_cast<std::size_t>(cy) * dim + cx;
			deformOut = deformField[i0];
			ridgeOut = ridgeField[i0];
			return;
		}
		const auto i0 = static_cast<std::size_t>(y0) * dim + x0;
		deformOut = deformField[i0];
		ridgeOut = ridgeField[i0];
	}

	// v564：物体场（物品/拖痕/深坑）最近邻，同款（v529 场通道分离保持）。
	void SnowShellMesh::SampleFieldObjNearest(float wx, float wy, float& deformOut, float& ridgeOut) const
	{
		deformOut = 0.0f;
		ridgeOut = 0.0f;
		if (!fieldReady.load() || deformFieldObj.empty())
			return;
		const float fx = (wx - fieldOriginX) / kFieldStep;
		const float fy = (wy - fieldOriginY) / kFieldStep;
		const int x0 = static_cast<int>(std::floor(fx + 0.5f));
		const int y0 = static_cast<int>(std::floor(fy + 0.5f));
		const int dim = kFieldDim;
		if (x0 < 0 || y0 < 0 || x0 >= dim || y0 >= dim) {
			const int cx = std::clamp(x0, 0, dim - 1);
			const int cy = std::clamp(y0, 0, dim - 1);
			const auto i0 = static_cast<std::size_t>(cy) * dim + cx;
			deformOut = deformFieldObj[i0];
			ridgeOut = ridgeFieldObj[i0];
			return;
		}
		const auto i0 = static_cast<std::size_t>(y0) * dim + x0;
		deformOut = deformFieldObj[i0];
		ridgeOut = ridgeFieldObj[i0];
	}

	// v435：**场景雪堆（墙边/岩石边自动堆雪）**——游戏线程（FindLandscape 末尾）重建：
	// 1) ForEachReferenceInRange 收集玩家周围静态（kStatic）REFR 的最大碰撞包围球
	//    （GetColliderBound，只读碰撞体安全铁律同 v346）——墙/岩石/建筑/树干都是雪堆源；
	// 2) 224²@64 稀疏场：贴墙（d ≤ r+kSceneSpan）隆起 kSceneLiftMax，向外平滑衰减到 0。
	// 双缓冲 + 原子 idx 切换：重建写 1-idx 备用缓冲，算完原子切 → 渲染线程永远读
	// 完整缓冲（与 landBuf 双缓冲同模式），零竞争零闪烁。
	void SnowShellMesh::BuildSceneLift()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return;
		const auto pos = player->GetPosition();
		// ---- 1) 收集静态遮挡物（上限 64 个，防密集城镇爆量）----
		std::vector<SceneObstacle> obs;
		obs.reserve(64);
		if (auto* tes = RE::TES::GetSingleton()) {
			int seen = 0;
			tes->ForEachReferenceInRange(player, 2500.0f,
				[&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
					if (++seen > 400)
						return RE::BSContainer::ForEachResult::kStop;
					if (!ref)
						return RE::BSContainer::ForEachResult::kContinue;
					auto* base = ref->GetBaseObject();
					if (!base || base->GetFormType() != RE::FormType::Static)
						return RE::BSContainer::ForEachResult::kContinue;
					auto* root = ref->Get3D(false);
					if (!root)
						return RE::BSContainer::ForEachResult::kContinue;
					RE::NiPoint3 c{ 0.0f, 0.0f, 0.0f };
					float r = 0.0f;
					RE::BSVisit::TraverseScenegraphCollision(root,
						[&](RE::bhkNiCollisionObject* obj) -> RE::BSVisit::BSVisitControl {
							RE::NiPoint3 cc;
							float rr = 0.0f;
							if (GetColliderBound(obj, cc, rr) && rr > r) {
								r = rr;
								c = cc;
							}
							return RE::BSVisit::BSVisitControl::kContinue;
						});
					// 过滤：半径 40~400（小物件/超大场景忽略）、中心贴地（与玩家 z 差 ±300）
					if (r < 40.0f || r > 400.0f)
						return RE::BSContainer::ForEachResult::kContinue;
					if (std::abs(c.z - pos.z) > 300.0f)
						return RE::BSContainer::ForEachResult::kContinue;
					if (obs.size() < 64)
						obs.push_back(SceneObstacle{ c.x, c.y, r });
					return RE::BSContainer::ForEachResult::kContinue;
				});
		}
		// ---- 2) 重建雪堆场到备用缓冲（写完原子切换）----
		const int oldIdx = sceneLiftIdx.load();
		const int newIdx = 1 - oldIdx;
		auto& sb = sceneLiftBuf[newIdx];
		sb.field.assign(static_cast<std::size_t>(kSceneDim) * kSceneDim, 0.0f);
		const float halfSpan = static_cast<float>(kSceneDim) * kSceneStep * 0.5f;
		sb.ox = std::floor((pos.x - halfSpan) / kSceneStep) * kSceneStep;
		sb.oy = std::floor((pos.y - halfSpan) / kSceneStep) * kSceneStep;
		// v437：**场景雪堆降强度**——v436 数据 + 截图实锤：sceneLiftMax=21.7（22 高）
		// + 坑沿雪堆 14 → 净隆起 36 盖住凹陷（-40.9），玩家视角"鼓包不是沟"。
		// 22→10、span 100→60：墙脚仍有雪坡但凹陷（-42）明显深于雪堆（+10），
		// 沟壑感恢复。参数独立可调（kSceneLiftMax / kSceneSpan）。
		constexpr float kSceneSpan = 60.0f;    // 雪堆向远离墙方向延伸宽度
		constexpr float kSceneLiftMax = 10.0f;  // v442b：贴墙雪堆 16→10（同比例降，别盖凹陷）
		float maxLift = 0.0f;
		for (int gy = 0; gy < kSceneDim; gy++) {
			for (int gx = 0; gx < kSceneDim; gx++) {
				const float wx = sb.ox + static_cast<float>(gx) * kSceneStep;
				const float wy = sb.oy + static_cast<float>(gy) * kSceneStep;
				float best = 0.0f;
				for (const auto& ob : obs) {
					const float dx = wx - ob.x;
					const float dy = wy - ob.y;
					const float lim = ob.radius + kSceneSpan;
					if (dx * dx + dy * dy < lim * lim) {
						float t = 1.0f - std::sqrt(dx * dx + dy * dy) / lim;
						t = t * t * (3.0f - 2.0f * t);  // smoothstep（贴墙满高 → 外沿 0）
						const float lift = kSceneLiftMax * t;
						if (lift > best)
							best = lift;
					}
				}
				sb.field[static_cast<std::size_t>(gy) * kSceneDim + gx] = best;
				if (best > maxLift)
					maxLift = best;
			}
		}
		sceneLiftIdx.store(newIdx);
		SKSE::log::info("v435: scene lift built ({} obstacles, maxLift={:.1f}, origin=({:.0f},{:.0f}))",
			obs.size(), maxLift, sb.ox, sb.oy);
	}

	// v435：场景雪堆场双线性采样（渲染线程，与 SampleField 同模式）
	void SnowShellMesh::SampleSceneLift(float wx, float wy, float& out) const
	{
		out = 0.0f;
		const auto& sb = sceneLiftBuf[sceneLiftIdx.load()];
		if (sb.field.empty())
			return;
		const float fx = (wx - sb.ox) / kSceneStep;
		const float fy = (wy - sb.oy) / kSceneStep;
		const int x0 = static_cast<int>(std::floor(fx));
		const int y0 = static_cast<int>(std::floor(fy));
		if (x0 < 0 || y0 < 0 || x0 >= kSceneDim - 1 || y0 >= kSceneDim - 1) {
			const int cx = std::clamp(x0, 0, kSceneDim - 1);
			const int cy = std::clamp(y0, 0, kSceneDim - 1);
			out = sb.field[static_cast<std::size_t>(cy) * kSceneDim + cx];
			return;
		}
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const auto i00 = static_cast<std::size_t>(y0) * kSceneDim + x0;
		out = (sb.field[i00] * (1.0f - tx) + sb.field[i00 + 1] * tx) * (1.0f - ty) +
			(sb.field[i00 + kSceneDim] * (1.0f - tx) + sb.field[i00 + kSceneDim + 1] * tx) * ty;
	}

	// v290：自写递归找节点（替代引擎 GetObjectByName 虚函数——实测在渲染线程
	// 对玩家 3D 树恒返回 nullptr：v286 时代日志反推 R 脚位置 = 玩家中心±20 兜底
	// 偏移完全吻合 → 引擎虚函数从未找到 "L Foot"，一直走兜底 → "怪异不跟脚"
	// 的真正根因。自写递归用 GetChildren + RTTI 名字链（NiUtils 安全模式）。
	// v291：**子串匹配**（strstr）——v290 树 dump 实锤玩家脚节点真实名字是
	// "NPC L Foot [Lft ]"（XPMSE 风格骨骼，带 [缩写] 后缀），精确匹配永远失败。
	static RE::NiAVObject* FindNodeByName(RE::NiAVObject* a_root, const char* a_name)
	{
		if (!a_root || !a_name)
			return nullptr;
		if (a_root->name.size() > 0 &&
			std::strstr(a_root->name.c_str(), a_name) != nullptr)
			return a_root;
		if (auto* nd = As<RE::NiNode>(a_root, "NiNode")) {
			for (auto& child : nd->GetChildren()) {
				if (child) {
					if (auto* r = FindNodeByName(child.get(), a_name))
						return r;
				}
			}
		}
		return nullptr;
	}

	// 递归遍历子形状，取最大半径（武器 ListShape 展开用）
	static float GetShapeRadiusRecursive(const RE::hkpShape* shape, float inv)
	{
		if (!shape)
			return 0.0f;
		if (auto* list = skyrim_cast<const RE::hkpListShape*>(shape)) {
			float best = 0.0f;
			for (const auto& ci : list->childInfo) {
				if (ci.shape) {
					const float r = GetShapeRadiusRecursive(ci.shape, inv);
					if (r > best)
						best = r;
				}
			}
			return best;
		}
		auto project = [shape, inv](float x, float y, float z) {
			return shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * inv;
		};
		const float hx = 0.5f * (project(1.0f, 0.0f, 0.0f) + project(-1.0f, 0.0f, 0.0f));
		const float hy = 0.5f * (project(0.0f, 1.0f, 0.0f) + project(0.0f, -1.0f, 0.0f));
		const float hz = 0.5f * (project(0.0f, 0.0f, 1.0f) + project(0.0f, 0.0f, -1.0f));
		const auto type = shape->type;
		if (type == RE::hkpShapeType::kCapsule || type == RE::hkpShapeType::kSphere)
			return std::max(hx, std::max(hy, hz));
		if (type == RE::hkpShapeType::kBox || type == RE::hkpShapeType::kCylinder ||
			type == RE::hkpShapeType::kConvexVertices || type == RE::hkpShapeType::kTriangle)
			return std::sqrt(hx * hx + hy * hy + hz * hz);
		return 0.0f;
	}

	// v346：**Havok 碰撞体包围球（CS Dynamic Snow Util::GetShapeBound 移植）**——
	// bhkRigidBody → hkpRigidBody → collidable.GetShape() → 质心 + 按形状类型投影
	// 算半径。**安全铁律：只读碰撞体（不读 NiGeometryData.vertex）**——碰撞体由
	// Havok 管理，换装/皮肤变形不重建 → 游戏线程可读（v342 读网格顶点崩实锤）。
	// v527：**恢复 v524 前原版（用户"恢复之前的代码，后面的修改仅针对拖拽"）**——
	// ListShape 直接 return false（v524 让 ListShape 成功 → ScanColliders 玩家脚印
	// 收录了 ListShape 碰撞体 → 脚印变样）。ListShape 支持只保留在
	// GetColliderBoundList（武器盖章专用）。
	static bool GetColliderBound(RE::bhkNiCollisionObject* a_obj, RE::NiPoint3& a_center, float& a_radius)
	{
		if (!a_obj)
			return false;
		RE::bhkRigidBody* rigid = a_obj->body.get() ? a_obj->body.get()->AsBhkRigidBody() : nullptr;
		auto* hkp = rigid ? skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get()) : nullptr;
		if (!rigid || !hkp)
			return false;
		if (skyrim_cast<RE::hkpListShape*>(hkp))
			return false;  // ListShape 不支持（v524 前行为）
		RE::hkVector4 massCenter;
		rigid->GetCenterOfMassWorld(massCenter);
		const float inv = RE::bhkWorld::GetWorldScaleInverse();
		float mc[4];
		_mm_storeu_ps(mc, massCenter.quad);
		a_center = RE::NiPoint3(mc[0], mc[1], mc[2]) * inv;
		auto* shape = hkp->collidable.GetShape();
		if (!shape)
			return false;
		auto project = [shape, inv](float x, float y, float z) {
			return shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * inv;
		};
		const float hx = 0.5f * (project(1.0f, 0.0f, 0.0f) + project(-1.0f, 0.0f, 0.0f));
		const float hy = 0.5f * (project(0.0f, 1.0f, 0.0f) + project(0.0f, -1.0f, 0.0f));
		const float hz = 0.5f * (project(0.0f, 0.0f, 1.0f) + project(0.0f, 0.0f, -1.0f));
		const auto type = shape->type;
		if (type == RE::hkpShapeType::kCapsule || type == RE::hkpShapeType::kSphere) {
			a_radius = std::max(hx, std::max(hy, hz));  // 胶囊沿主轴最长半轴
		} else if (type == RE::hkpShapeType::kBox || type == RE::hkpShapeType::kCylinder ||
			type == RE::hkpShapeType::kConvexVertices || type == RE::hkpShapeType::kTriangle) {
			a_radius = std::sqrt(hx * hx + hy * hy + hz * hz);  // 半对角
		} else {
			return false;
		}
		return a_radius > 0.5f;
	}

	// v527：**GetColliderBoundList（武器盖章专用，支持 ListShape）**——v524 的
	// ListShape 递归版，只给 ScanPlayerMining 用（武器碰撞体是 ListShape →
	// 原版 GetColliderBound 失败 → 拖拽不触发）。玩家脚印/物品/场景雪堆保持
	// 用原版 GetColliderBound（v524 前行为）。
	static bool GetColliderBoundList(RE::bhkNiCollisionObject* a_obj, RE::NiPoint3& a_center, float& a_radius)
	{
		if (!a_obj)
			return false;
		RE::bhkRigidBody* rigid = a_obj->body.get() ? a_obj->body.get()->AsBhkRigidBody() : nullptr;
		auto* hkp = rigid ? skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get()) : nullptr;
		if (!rigid || !hkp)
			return false;
		RE::hkVector4 massCenter;
		rigid->GetCenterOfMassWorld(massCenter);
		const float inv = RE::bhkWorld::GetWorldScaleInverse();
		float mc[4];
		_mm_storeu_ps(mc, massCenter.quad);
		a_center = RE::NiPoint3(mc[0], mc[1], mc[2]) * inv;
		auto* shape = hkp->collidable.GetShape();
		if (!shape)
			return false;
		a_radius = GetShapeRadiusRecursive(shape, inv);
		return a_radius > 0.5f;
	}

	// v437b：**碰撞形状指针（安全铁律：只读碰撞体）**——v342b 铁律：网格顶点
	// 不可读（换装悬空崩溃），碰撞体（hkpShape）由 Havok 管理不随换装重建 →
	// 可安全保存指针、按任意方向 GetMaximumProjection 投影半轴（鞋形椭圆战壕）。
	// v524：递归解包 ListShape → 第一个有效非 ListShape 子形状
	// v526：**恢复 v524 前行为（用户"我的修改导致脚印沟壑也变化了"实锤）**——
	// ScanColliders（玩家脚印盖章）用 GetColliderShapePtr 提取鞋形半轴：v524 让
	// ListShape 返回子形状 → 玩家脚部 ListShape 的鞋形投影变了 → 战壕形状/宽度
	// 变样。**ListShape 一律返回 nullptr（圆 fallback = v524 前行为）**——玩家
	// 脚印完全恢复；武器盖章的半轴提取退化（hitHalfL/W=crad 兜底，拖痕 shape=11
	// 固定 8 不受影响）。ListShape 半径支持保留在 GetColliderBound（武器触发必需）。
	static const RE::hkpShape* GetColliderShapePtr(RE::bhkNiCollisionObject* a_obj)
	{
		if (!a_obj)
			return nullptr;
		RE::bhkRigidBody* rigid = a_obj->body.get() ? a_obj->body.get()->AsBhkRigidBody() : nullptr;
		auto* hkp = rigid ? skyrim_cast<RE::hkpRigidBody*>(rigid->referencedObject.get()) : nullptr;
		if (!rigid || !hkp)
			return nullptr;
		auto* shape = hkp->collidable.GetShape();
		if (!shape)
			return nullptr;
		// v526：ListShape 一律返回 nullptr（v524 前行为，见上方注释）
		if (skyrim_cast<const RE::hkpListShape*>(shape))
			return nullptr;
		return shape;
	}

	// v592：**尸体压痕（TESDeathEvent 死亡事件版）**——死亡事件在死亡瞬间必触发
	//（游戏主线程）。footMtx 保护与 RebuildField（渲染线程）无竞争。
	// v597：**延迟盖章（"尸体不会在地上出现浅浅的坑"修复）**——死亡事件触发时
	// actor 还没倒地（站立位置），直接盖 = 坑在站立点，尸体倒下后坑被尸体压住/
	// 错位露不出来（实锤）。改为：只把 actor 加入队列（NiPointer 强引用保活 2s），
	// ScanAnimalFeet 每 150ms 检查，死亡 2s 后（ragdoll 落定）取尸体最终位置盖
	// 浅坑（depth 0.35 → -6.3，浅浅的坑，同法术坑的形状思路）。
	static std::vector<std::pair<RE::NiPointer<RE::Actor>, unsigned long>> g_corpseQ;  // actor + 死亡时间

	// v589：**每 actor 独立上次位置**（formID -> 位置 + 首见标记）——连续战壕
	// 需要 prev=上次位置（玩家盖章同款）；v587 的全局 lastAX/lastAY 多 actor
	// 互相干扰（prev 错位 → 战壕乱连）。v609：从函数内 static 提升为文件级
	//（ResetForLoadGame 读档时清空——formID 复用会让新 actor 继承旧位置/wasDead，
	// 误盖坑/战壕错连）。
	struct LastP {
		float x = 0.0f, y = 0.0f;
		bool init = false;
		bool wasDead = false;  // v590：尸体状态（活体→尸体转变时盖压痕）
	};
	static std::unordered_map<std::uint32_t, LastP> lastPos;

	// v592：**尸体压痕（TESDeathEvent 死亡事件版）**——死亡事件在死亡瞬间必触发
	void SnowShellMesh::OnActorDeath(RE::Actor* a)
	{
		if (!a || a == RE::PlayerCharacter::GetSingleton() || a->IsPlayer())
			return;
		// v609：**队列上限**——死亡事件突发（群战/屠村）时防无限累积；超 64 丢最老
		//（老尸体 2s 延迟坑无价值，保留最新响应性）
		if (g_corpseQ.size() >= 64)
			g_corpseQ.erase(g_corpseQ.begin());
		g_corpseQ.emplace_back(RE::NiPointer<RE::Actor>(a), GetTickCount());
		SKSE::log::info("v597-dbg: death queued={}", a->GetDisplayFullName());
	}

	// v598：**尸体抓取释放盖坑**——引擎 Z 键抓取尸体 → 释放瞬间 TESGrabReleaseEvent
	//（grabbed=false）→ 在释放位置盖浅椭圆坑（同尸体压痕参数）。物品滚动盖章
	//（shape=9）是滚动路径；抓取释放是独立路径（尸体被举起来换地方丢 → 坑在丢处）。
	// 防重复：同位置反复抓丢 → 场 max 合成不加深；列表受 v596 统一驱逐。
	void SnowShellMesh::OnGrabRelease(RE::TESObjectREFR* a_ref, bool a_grabbed)
	{
		if (a_grabbed || !a_ref)
			return;
		auto* actor = a_ref->As<RE::Actor>();
		if (!actor || !actor->IsDead())
			return;  // 只处理尸体（活体抓取/普通物体不盖）
		const auto ap = actor->GetPosition();
		const float yaw = actor->GetAngle().z * 0.017453292f;  // 度→弧度
		const float cY = std::cos(yaw), sY = std::sin(yaw);
		{
			std::lock_guard<std::mutex> lk(footMtx);
			// v601：**尸体压痕对齐物品（头盔）落地强度（用户"尸体不能像头盔一样么"）**——
			// depth 0.35→0.6（物品 kObjDepth 同款）→ 坑 -0.6×1.85×18≈-20，和物品/
			// 玩家坑同深度，明显可见；雪丘环（v599 24×depth≈14）埋尸感更强。
			footprints.push_back({ ap.x, ap.y, 0.6f, 0.0f, cY, sY,
				50.0f, 25.0f, ap.x, ap.y, 14, GetTickCount() });
			landFootDirty.store(true);
			// v600-dbg：盖章后列表状态（定位"盖了但看不到"——是否被驱逐/重叠/丢失）
			int near14 = 0;
			for (const auto& f : footprints) {
				if (f.shape == 14) {
					const float ddx = f.x - ap.x, ddy = f.y - ap.y;
					if (ddx * ddx + ddy * ddy < 2500.0f)
						near14++;
				}
			}
			SKSE::log::info("v600-dbg: dropped={} at=({:.0f},{:.0f}) fp={} near14={}",
				actor->GetDisplayFullName(), ap.x, ap.y, footprints.size(), near14);
		}
	}

	void SnowShellMesh::ScanAnimalFeet()
	{
		auto* pl = RE::ProcessLists::GetSingleton();
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pl || !pc)
			return;
		const auto pp = pc->GetPosition();
		static unsigned long lastAT = 0;
		// v597：**尸体压痕延迟盖章**——死亡事件只入队（OnActorDeath），这里每
		// 150ms 检查：死亡 >2s（ragdoll 已落定）→ 取尸体最终位置盖浅椭圆压痕
		//（depth 0.35 → -6.3 浅浅的坑）→ 出队。NiPointer 强引用保证 actor 存活。
		for (auto it = g_corpseQ.begin(); it != g_corpseQ.end();) {
			const unsigned long nowC = GetTickCount();
			if (nowC - it->second <= 2000) {
				++it;
				continue;
			}
			auto* corpse = it->first.get();
			if (corpse) {
				const auto cap = corpse->GetPosition();
				const float yaw = corpse->GetAngle().z * 0.017453292f;  // 度→弧度
				const float cY = std::cos(yaw), sY = std::sin(yaw);
				{
					std::lock_guard<std::mutex> lk(footMtx);
					// v601：depth 0.35→0.6（对齐物品落地强度，坑 -20 明显可见）
					footprints.push_back({ cap.x, cap.y, 0.6f, 0.0f, cY, sY,
						50.0f, 25.0f, cap.x, cap.y, 14, GetTickCount() });
					landFootDirty.store(true);
				}
				SKSE::log::info("v597-dbg: corpse={} at=({:.0f},{:.0f})", corpse->GetDisplayFullName(), cap.x, cap.y);
			}
			it = g_corpseQ.erase(it);
		}
		// v587：节流在调用级（一次遍历盖全部 actor）
		// v594：300→150ms（动物/NPC 轨迹更连续——300ms 间隔 + 35 门控慢速动物
		// 盖不出连续沟壑 = "像一个个小洞"实锤）
		// v603：150→100ms（用户"延迟太大要精确"——动物/尸体痕迹跟随更及时）
		// v607：100→50ms（**骑马脚印"都是小坑"修复**）——马走 4m/s → 100ms 只移
		// 40 单位 < 战壕长轴 60（rA=30×2）→ 战壕段断裂 → 看起来像独立椭圆坑。
		// 50ms → 段 20 单位 → 战壕首尾相接连续（玩家 20ms 检测连续同理）。盖章
		// 率 ×2（马/狼 ~40/s obj）→ obj 60 上限 + 驱逐兜底。
		const unsigned long nowA = GetTickCount();
		if (nowA - lastAT < 50)
			return;
		// v587-dbg：汇总统计（2s 输出一次）
		static int sDbgStamped = 0, sDbgActors = 0;
		static unsigned long lastDbgT = 0;
		static int sDbgStampLog = 0;
		// v590：**尸体压痕坑**——IsDead 不再跳过；尸体（含骷髅等不死族尸体）倒地面
		// 盖一个沿 actor 朝向的椭圆压痕（长 100 宽 50 深-10.8，纯凹陷无雪堆）：
		// ① 首见即尸体（走进看到）② 活体被打死倒下瞬间（wasDead false→true）。
		// 尸体不动 → 盖一次即可，之后跳过（20 单位门控天然防重复）。
		auto stampCorpse = [&](RE::Actor* a, const RE::NiPoint3& ap) {
			const float yaw = a->GetAngle().z * 0.017453292f;  // 度→弧度（玩家 yaw 同款）
			const float cY = std::cos(yaw), sY = std::sin(yaw);
			{
				auto& shell = SnowDeform::GetSnowShellMesh();
				std::lock_guard<std::mutex> lk(shell.footMtx);
				shell.footprints.push_back({ ap.x, ap.y, 0.6f, 0.0f, cY, sY,
					50.0f, 25.0f, ap.x, ap.y, 14, GetTickCount() });
				shell.landFootDirty.store(true);
			}
			sDbgStamped++;
			if (sDbgStampLog < 10) {
				sDbgStampLog++;
				const float dPx = ap.x - pp.x, dPy = ap.y - pp.y;
				SKSE::log::info("v590-dbg: corpse={} at=({:.0f},{:.0f}) distP={:.0f}",
					a->GetDisplayFullName(), ap.x, ap.y,
					std::sqrt(dPx * dPx + dPy * dPy));
			}
		};
		// v591：**ForEachHighActor → ForAllActors（尸体压痕修复）**——ForEachHighActor
		// 只遍历高优先级活动 actor，**尸体被降级移出 high list** → v590 的
		// stampCorpse 从未触发（v590-dbg corpse=0 实锤）。ForAllActors 遍历 process
		// list 全部 actor（含尸体/低优先级），1000 单位过滤在内部 → 覆盖尸体。
		pl->ForAllActors([&](RE::Actor* a) -> RE::BSContainer::ForEachResult {
			if (!a || a == pc)
				return RE::BSContainer::ForEachResult::kContinue;
			// v587：人形/动物/骑乘全盖（只排除玩家自己）；v590：尸体也盖压痕
			const auto ap = a->GetPosition();
			// v587：范围 1000（用户选更大）→ **v594：1000→500（fp 915 卡顿修复）**——
			// v591 ForAllActors 把玩家周围全部 actor（含远处低优先级生物）纳入盖章 →
			// 盖章率爆炸（fp 190→915、rf 68ms 实锤）。500 单位 = 视觉重点区（配合
			// 平滑 ±1024 限幅）；远处生物不盖（玩家看不到）。尸体压痕不依赖遍历
			//（v592 死亡事件在任何距离都盖）→ 无损失。
			if ((ap - pp).Length() > 500.0f)
				return RE::BSContainer::ForEachResult::kContinue;
			const bool isDead = a->IsDead();
			auto& lp = lastPos[a->formID];
			// v590：首见——尸体直接盖压痕；活体只记录（移动后才盖战壕）
			if (!lp.init) {
				lp.x = ap.x;
				lp.y = ap.y;
				lp.init = true;
				lp.wasDead = isDead;
				if (isDead)
					stampCorpse(a, ap);
				return RE::BSContainer::ForEachResult::kContinue;
			}
			// v590：活体→尸体转变（被打死倒下瞬间）→ 盖压痕
			// v602：**被抓取的尸体按活体处理（拖动沿途盖章，像头盔滚动）**——
			// 头盔（物品）被抓着移动 → ScanMovingObjects 沿途盖章（滚动拖痕）；
			// 尸体此前"尸体持续"直接 return → 拖动过程零痕迹（只有释放那一下）
			// = "头盔有效果尸体没有"的根因。被抓取（pc->GetGrabbedRef()==a）的
			// 尸体移动 >25 → 沿途盖 shape=14 战壕拖痕（rL/rS=8 宽 40 沟，同活体
			// 动物）；释放点由 v598 盖最终压痕。
			if (isDead) {
				if (!lp.wasDead) {
					lp.wasDead = true;
					lp.x = ap.x;
					lp.y = ap.y;
					stampCorpse(a, ap);
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (pc->GetGrabbedRef().get() == a) {
					const float dx = ap.x - lp.x, dy = ap.y - lp.y;
					if (dx * dx + dy * dy > 25.0f * 25.0f) {
						const float px = lp.x, py = lp.y;
						lp.x = ap.x;
						lp.y = ap.y;
						{
							auto& shell = SnowDeform::GetSnowShellMesh();
							std::lock_guard<std::mutex> lk(shell.footMtx);
							shell.footprints.push_back({ ap.x, ap.y, 0.6f, 0.0f, 0.0f, 0.0f,
								8.0f, 8.0f, px, py, 14, GetTickCount() });
							shell.landFootDirty.store(true);
							// v604：盖章类型计数（0 玩家 1 NPC 2 马 3 狼/其他动物）
							if (auto* r = a->GetRace()) {
								if (r->data.flags.any(RE::RACE_DATA::Flag::kFaceGenHead))
									gStmpType[1].fetch_add(1, std::memory_order_relaxed);
								else {
									const char* eid = r->GetFormEditorID();
									if (eid && std::strstr(eid, "Horse"))
										gStmpType[2].fetch_add(1, std::memory_order_relaxed);
									else
										gStmpType[3].fetch_add(1, std::memory_order_relaxed);
								}
							} else {
								gStmpType[3].fetch_add(1, std::memory_order_relaxed);
							}
						}
						sDbgStamped++;
						if (sDbgStampLog < 10) {
							sDbgStampLog++;
							const float dPx = ap.x - pp.x, dPy = ap.y - pp.y;
							SKSE::log::info("v602-dbg: drag corpse={} at=({:.0f},{:.0f}) distP={:.0f}",
								a->GetDisplayFullName(), ap.x, ap.y,
								std::sqrt(dPx * dPx + dPy * dPy));
						}
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}
			lp.wasDead = false;
			// v589：**actor 中心位置盖章（不再细分脚节点）**——用户"马也要和玩家
			// 骑马时候的雪沟壑一样"：actor 位置连续轨迹 → 玩家同款战壕（马 4 蹄
			// 合成 1 条沟壑，与玩家骑马一致）。移动 >20 单位盖（prev=上次位置 →
			// 连续胶囊战壕，玩家盖章同款逻辑）。
			// v591：门控 20→35（狼群跑得快盖章密 → rf 22ms 卡顿修复；35 单位间隔
			// 视觉仍连续，盖章量降 ~40%）→ **v594：35→25（"像一个个小洞"修复）**——
			// 35 门控 + 150ms 节流：慢速动物/绕圈 AI 每次移动 <35 不盖 → 偶发单点
			// = 独立小洞。25 门控 + 150ms：慢走（80/s → 12 单位/150ms）也盖 → 段
			// 首尾相接 → 连续沟壑（同玩家）。
			const float dx = ap.x - lp.x, dy = ap.y - lp.y;
			if (dx * dx + dy * dy > 25.0f * 25.0f) {
				const float px = lp.x, py = lp.y;
				lp.x = ap.x;
				lp.y = ap.y;
				{
					auto& shell = SnowDeform::GetSnowShellMesh();
					std::lock_guard<std::mutex> lk(shell.footMtx);
					shell.footprints.push_back({ ap.x, ap.y, 0.8f, 0.0f, 0.0f, 0.0f,
						8.0f, 8.0f, px, py, 14, GetTickCount() });
					shell.landFootDirty.store(true);
					// v604：盖章类型计数（0 玩家 1 NPC 2 马 3 狼/其他动物）
					if (auto* r = a->GetRace()) {
						if (r->data.flags.any(RE::RACE_DATA::Flag::kFaceGenHead))
							gStmpType[1].fetch_add(1, std::memory_order_relaxed);
						else {
							const char* eid = r->GetFormEditorID();
							if (eid && std::strstr(eid, "Horse"))
								gStmpType[2].fetch_add(1, std::memory_order_relaxed);
							else
								gStmpType[3].fetch_add(1, std::memory_order_relaxed);
						}
					} else {
						gStmpType[3].fetch_add(1, std::memory_order_relaxed);
					}
				}
				sDbgStamped++;
				// v589-dbg：盖章位置（首 10 个）——确认沟壑盖在 actor 轨迹上
				if (sDbgStampLog < 10) {
					sDbgStampLog++;
					const float dPx = ap.x - pp.x, dPy = ap.y - pp.y;
					SKSE::log::info("v589-dbg: stamp actor={} at=({:.0f},{:.0f}) distP={:.0f}",
						a->GetDisplayFullName(), ap.x, ap.y,
						std::sqrt(dPx * dPx + dPy * dPy));
				}
			}
			sDbgActors++;
			return RE::BSContainer::ForEachResult::kContinue;
		});
		lastAT = nowA;  // v569/v587：调用后更新（一次遍历盖全部 actor）
		// v587-dbg：2s 汇总（防多 actor 刷屏）
		const unsigned long nowDbg = GetTickCount();
		if (nowDbg - lastDbgT >= 2000) {
			lastDbgT = nowDbg;
			SKSE::log::info("v589-dbg: actors2s={} stamped2s={}", sDbgActors, sDbgStamped);
			sDbgActors = 0;
			sDbgStamped = 0;
		}
	}

	// v562：**脚印贴花全部移除（用户拍板）**——v561 系列（手动构造 BSTempEffectSimpleDecal
	// + NMN TXST 贴图集）删除：引擎 Initialize 不建几何（v561h 实测 geom3d=0x0），手动构造
	// 路线不可控。footprint_decals.nif / NMN 贴图复制文件残留（无害，不引用）。

	// v346：**玩家脚印（碰撞体盖章）**——游戏线程每 20ms（AddTask 调度）扫描玩家
	// 3D 树鞋底碰撞体（BSMultiBoundNode→形状），生成 ShapeStamp（椭圆半轴投影）
	// 盖章（shape 0-3）。v527：**恢复原版 GetColliderBound**（v524 共享改动级联影响
	// 玩家脚印 → 用户"脚印变了"）——玩家脚印形状检测只走 GetColliderBound（不碰
	// ListShape），武器检测走 GetColliderBoundList。
	// > 地面+40 跳过：头/手臂路过不盖）+ 半径 4~128 合理范围。px/py 保留上次
	// 盖章点（渲染线程盖章后更新）→ 位移 > 步长时盖连续胶囊轨迹。
	void SnowShellMesh::ScanColliders()
	{
		// v360：**reentrancy guard**——上一次未跑完直接跳过（防止 AddTask 多线程并发）
		if (scanning.exchange(true))
			return;
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !pc->Get3D()) {
			scanning.store(false);
			return;
		}
		// v606：**骑马时跳过玩家盖章（骑马卡顿修复）**——骑马时玩家脚不落地，但
		// 马移动 → 玩家 position 动 → 玩家碰撞体盖章（85-125/s）+ 马自身 shape=14
		// 盖章（ForAllActors）双重盖章 → 盖章率爆表 → 每帧重建 → 卡。骑马时玩家
		// 盖章无意义（马已经盖了轨迹）→ 直接跳过。GetMount 检测骑乘。
		{
			RE::NiPointer<RE::Actor> mount;
			if (pc->GetMount(mount) && mount) {
				scanning.store(false);
				return;
			}
		}
		const float groundZ = pc->GetPosition().z;
		std::unordered_map<std::uint32_t, ColliderStamp> next;
		std::uint32_t shapeIdx = 0;
		RE::BSVisit::TraverseScenegraphCollision(pc->Get3D(), [&](RE::bhkNiCollisionObject* obj) {
			RE::NiPoint3 center;
			float radius = 0.0f;
			if (GetColliderBound(obj, center, radius)) {
				if (center.z - radius <= groundZ + 40.0f && radius >= 4.0f && radius <= 128.0f) {
					const std::uint32_t key = (pc->formID & 0xFFFF) | (shapeIdx << 16);
					auto& st = next[key];
					st.x = center.x;
					st.y = center.y;
					st.z = center.z;
					st.radius = radius;
					// v437b：保存碰撞形状指针（Havok 管理长存，不随换装重建——
					// v342b 铁律"只读碰撞体不读网格顶点"）。盖章时按轨迹方向投影
					// 半轴 → 椭圆战壕（鞋形感，比包围球圆写实且安全）。
					st.shape = GetColliderShapePtr(obj);
				}
			}
			shapeIdx++;
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		{
			std::lock_guard<std::mutex> lk(colliderMtx);
			for (auto& [k, st] : next) {
				auto it = colliders.find(k);
				if (it != colliders.end()) {
					st.px = it->second.px;  // 保留上次盖章点（非旧位置）
					st.py = it->second.py;
				} else {
					st.px = st.x;
					st.py = st.y;
				}
				// v359：**CS 同款碰撞体盖章（游戏线程内，CS Stamping 移植）**——
				// 碰撞体位移 > 12 → 胶囊 prev→curr 连续轨迹（真实脚/小腿轨迹）。
				// v346 失败教训：盖章在渲染线程遍历 colliders map 与游戏线程
				// move 竞争 → 迭代器失效崩溃（18:51 实锤）+ stamps=0（门槛 24
				// 太高）。现在扫描+盖章同线程（ScanColliders 由 AddTask 调度在
				// 游戏线程）→ 渲染线程只读 footprints/场，零竞争。
				// 半径过滤 5~16：跳过身体/大腿大碰撞体（防超宽沟，测试阶段）。
				const float dx = st.x - st.px;
				const float dy = st.y - st.py;
				const float dist = std::sqrt(dx * dx + dy * dy);
				// v410：**轨迹断裂 256（CS kTrailBreakDistance 对齐）**——传送/加载/
				// 大跳位移 >256 时 prev=当前（只盖独立脚印不连战壕），防横穿长战壕
				if (dist > 256.0f) {
					st.px = st.x;
					st.py = st.y;
				}
				// v410：门 4→3（CS kStampMovementGate=3 对齐）+ r<=16
				// v577（用户"走路雪堆突凹闪"，2026-08-27）：**门 3→48**——CS 门 3 为
				// GPU 纹理盖章设计（廉价）；我们是 CPU 场（kFieldStep=4）+ 顶点变形，
				// 门 3 导致盖章 13-30/s、间距 3-8 单位 ≈ 场格 4 → 连续盖章落在交替
				// 场格 → 坑/雪堆位置 4 单位跳动 = "一会突出一会凹下去"闪（v576 延迟
				// 已修到 12ms 仍闪 = 实锤不是延迟）。门 48 = 间距 ≥12 场格 → 坑清晰
				// 不重叠（相邻坑边缘相切，胶囊战壕仍连续）。盖章 ~7/s @走路。
				if (dist > 48.0f && st.radius >= 4.0f && st.radius <= 16.0f) {
					const float ddx = dx / dist, ddy = dy / dist;
					// 加深（v357：越踩越深；v410 cap 1.5→1.0 对齐 CS stamp.z 恒 1.0）
					// v528：**只玩家脚印互相加深（shape<=3）**——原遍历所有 footprints：
					// 物品(0.6)/拖痕(0.8)/深坑(nW 递增)在旁边 55 单位内会把玩家脚印
					// depth 拉高（深坑 n=4 → min(1.0,4.12)=1.0 满深）→ 脚印变深/变样。
					float depth = 0.6f;
					constexpr float kOverlapR = 55.0f;
					{
						// v569：加锁——ScanColliders 游戏线程遍历 vs 渲染线程 RebuildField
						// 持 footMtx 读/写 → 无锁遍历 = 迭代器失效崩溃（push_back 扩容时）
						std::lock_guard<std::mutex> lkF3(footMtx);
						for (const auto& f2 : footprints) {
							if (f2.shape > 3)
								continue;  // v528：物品(9)/拖痕(11)/深坑(10)不参与玩家加深
							const float dx2 = f2.x - st.x, dy2 = f2.y - st.y;
							if (dx2 * dx2 + dy2 * dy2 < kOverlapR * kOverlapR)
								depth = std::max(depth, std::min(1.0f, f2.depth + 0.12f));
						}
					}
					const float r = std::max(5.0f, std::min(14.0f, st.radius));
					// v437b：**碰撞体形状半轴（椭圆战壕/椭圆鞋印）**——v436 想用鞋网格
					// mask（ScanContactShapes）但 v437 读 spModelData.vertex 闪退（v342b
					// 铁律实锤）。改用**碰撞体形状投影**（安全）：沿移动方向（ddx/ddy）
					// 与垂直方向投影 GetMaximumProjection → 沿轨迹半轴 rAlong（≈鞋长）
					// + 垂直半轴 rAcross（≈鞋宽）→ fp.rL≠fp.rS → 战壕横截面/鞋印从圆
					// 变椭圆（脚碰撞体是胶囊/盒，水平截面接近鞋形，比圆写实且不崩）。
					int fpShape = 0;
					float fpRL = r, fpRS = r;
					if (st.shape) {
						const float inv = RE::bhkWorld::GetWorldScaleInverse();
						auto proj = [&](float x, float y) {
							return st.shape->GetMaximumProjection(RE::hkVector4{ x, y, 0.0f, 0.0f }) * inv;
						};
						const float ra = 0.5f * (proj(ddx, ddy) + proj(-ddx, -ddy));
						const float rc = 0.5f * (proj(-ddy, ddx) + proj(ddy, -ddx));
						if (ra > 4.0f && rc > 4.0f && ra < 40.0f && rc < 40.0f) {
							fpRL = std::clamp(ra, 5.0f, 18.0f);  // 沿轨迹半轴（≈鞋长半）
							fpRS = std::clamp(rc, 4.0f, 14.0f);  // 垂直轨迹半轴（≈鞋宽半）
						}
					}
					// v382：footprints 写锁（游戏线程 vs 渲染线程读）
					{
						std::lock_guard<std::mutex> lockF(footMtx);
						footprints.push_back({ st.x, st.y, depth, 0.0f, ddx, ddy, fpRL, fpRS, st.px, st.py, fpShape, GetTickCount() });  // v554：玩家脚印记录 tMs（按时间回填 600s）
						gStmpType[0].fetch_add(1, std::memory_order_relaxed);  // v604：玩家盖章计数
						// v562：脚印贴花调用已移除（v561 系列全删）
						// v558q：**粒子特效全部移除（用户拍板"所有粒子特效内容全部删掉"）**——
						// v558~v558p 走路雪尘/烟云/自建颗粒全部删除（火焰验证走通 v558e 后
						// 各粒子方案效果均不理想：白色粒子雪地隐形、烟云体积难调、静态颗粒
						// 未验证）。BSTempEffectParticle 链路代码已移除。
					if (footprints.size() > gPlayerFpMax) {  // v573: INI 可调（默认 400，玩家可增——见 LoadConfig）
						// v553：**物品坑独立保护（用户"回填消失和时间无关，和长度有关"实锤）**——
						// 原 v528b 优先删 shape>3（物品坑）→ fp 满 1000 时物品坑被挤掉
						//（"几秒消失"真相 = 容量 erase 非回填）。改：物品坑独立上限 128
						// ——未超上限只删**最老玩家脚印**（物品坑保留到 300s 回填自然
						// 消失）；物品超上限才删最老物品。玩家脚印 1000 上限照常滚动。
						const std::size_t objCnt = std::count_if(footprints.begin(), footprints.end(),
							[](const Footprint& f) { return f.shape > 3; });
						constexpr std::size_t kObjFpMax = 60;  // v605：80→60（动物/尸体脚印上限收紧，fp 442 卡顿修复）
						if (objCnt > kObjFpMax) {
							if (auto itE = std::find_if(footprints.begin(), footprints.end(),
								[](const Footprint& f) { return f.shape > 3 && f.dieAt == 0; });  // v574：跳过淡出中
								itE != footprints.end())
								{ itE->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
						} else {
							if (auto itP = std::find_if(footprints.begin(), footprints.end(),
								[](const Footprint& f) { return f.shape <= 3 && f.dieAt == 0; });
								itP != footprints.end())
								{ itP->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
							else
								{ footprints.begin()->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 兜底标记最老淡出）
						}
					}
					}
					landFootDirty.store(true);
					gDirtySetT.store(GetTickCount(), std::memory_order_relaxed);  // v576：盖章→重建延迟检测
					st.px = st.x;
					st.py = st.y;
				}
			}
			// 诊断：每 2 秒打一次碰撞体分布（半径范围），数据驱动校准盖章尺寸
			static unsigned long lastDiag = 0;
			const unsigned long now = GetTickCount();
			if (now - lastDiag >= 2000) {
				lastDiag = now;
				float rmin = 1e30f, rmax = 0.0f;
				for (const auto& [k, st] : next) {
					rmin = std::min(rmin, st.radius);
					rmax = std::max(rmax, st.radius);
				}
				SKSE::log::info("v346: colliders={} radius=[{:.1f}..{:.1f}] z0={:.0f}",
					next.size(), rmin, rmax, groundZ);
				// v389b：**碰撞体明细**（一次 2 秒窗口内全打）——定位"奇怪东西凹陷"
				// 的盖章源（玩家 3D 树里哪些碰撞体贴地会盖章，位置是否偏离脚）。
				// stampGate：是否通过盖章门槛（r 4~16）
				for (const auto& [k, st] : next) {
					const bool gate = st.radius >= 4.0f && st.radius <= 16.0f;
					SKSE::log::info("v389b: collider key={:X} r={:.1f} at=({:.0f},{:.0f},{:.0f}) zBottom={:.0f} stamp={}",
						k, st.radius, st.x, st.y, st.z, st.z - st.radius, gate ? "Y" : "n");
				}
			}
			colliders = std::move(next);
			scanning.store(false);  // v360: reset guard (lock-held for next re-entry)
		}
	}

	// v451-dbg：物品扫描诊断计数器（文件级，ScanMovingObjects 统计）
	static std::atomic<int> gScanned{ 0 };
	static std::atomic<int> gDroppable{ 0 };
	static std::atomic<int> gMoved{ 0 };
	static std::atomic<int> gNearGround{ 0 };
	// v455-dbg：**物品盖章管线深诊**——GetPosition vs Get3D 位置差（验证"REFR
	// 滞后"理论）、碰撞形状统计、盖章次数
	static std::atomic<int>    gPosLagSum{ 0 };   // GetPosition-Get3D 差累计（×100）
	static std::atomic<int>    gPosLagN{ 0 };
	static std::atomic<int>    gShapesSum{ 0 };   // 碰撞形状数累计
	static std::atomic<int>    gShapesN{ 0 };
	static std::atomic<int>    gObjStamps{ 0 };   // 盖章触发次数
	static std::atomic<float>  gObjLastX{ 0.0f }; // v480c：最近盖章位置（场宽度实测用）
	static std::atomic<float>  gObjLastY{ 0.0f };
	static std::atomic<float>  gObjFirstX{ 0.0f }; // v480e：物品首个盖章（单点圆，宽度铁证）
	static std::atomic<float>  gObjFirstY{ 0.0f };
	static std::atomic<float>  gDragSpeed{ 0.0f };   // v530：最近武器盖章速度（单位/s）——速度门调参
	static std::atomic<float>  gPcSpeed{ 0.0f };     // v532：玩家移动速度（速度门调参）
	static std::atomic<int>    gDragSpeedGate{ 0 };  // v530：深坑被速度门挡次数（走路/站定下垂 = 不敲击）
	// v458-dbg：物品盖章位置偏差（3D 根 vs 碰撞质心）——验证"massCenter ≠ 3D 根"导致错位
	static std::atomic<int>    gObjPosDx{ 0 };   // dx 累计（×100，3D 根 - 质心）
	static std::atomic<int>    gObjPosDy{ 0 };
	static std::atomic<int>    gObjPosN{ 0 };
	// v467-dbg：**形状级链路检测（CS pr2659 逐条对齐验证）**——
	// gObjShapeTot=遍历到形状总数；gObjAirGate=贴地门挡（z-radius>ground+40）；
	// gObjRadGate=半径门挡（<4 或 >128）；gObjCradSum/N=通过门的原始碰撞半径分布；
	// gObjGapSum/N=盖章间距实际值（CS 无间距节流，我们 8）；gObjCapsule=有上次
	// 位置可画胶囊的次数（CS StampEnds 机制可行性）
	static std::atomic<int>    gObjShapeTot{ 0 };
	static std::atomic<int>    gObjAirGate{ 0 };
	static std::atomic<int>    gObjRadGate{ 0 };
	static std::atomic<int>    gObjCradSum{ 0 };
	static std::atomic<int>    gObjCradN{ 0 };
	static std::atomic<int>    gObjCradMin{ 999 };
	static std::atomic<int>    gObjCradMax{ 0 };
	static std::atomic<int>    gObjGapSum{ 0 };
	static std::atomic<int>    gObjGapN{ 0 };
	static std::atomic<int>    gObjCapsule{ 0 };
	// v469-dbg：**落点地块检测（用户"物品作用于什么地块，排查变量"）**——
	// 盖章时查物品位置所在 quad：qHigh/qLow=落高密 129²/低密 17² 盖章数、
	// s0/s1/s2/s3=落 surfaceClass 0(其他)/1(雪)/2(沙)/3(泥) 盖章数、
	// noQuad=未命中缓存 quad 次数（**s0 高 = 物品落在不可变形材质 → 算法再对也白搭**）
	static std::atomic<int>    gObjQHigh{ 0 };
	static std::atomic<int>    gObjQLow{ 0 };
	static std::atomic<int>    gObjS0{ 0 };
	static std::atomic<int>    gObjS1{ 0 };
	static std::atomic<int>    gObjS2{ 0 };
	static std::atomic<int>    gObjS3{ 0 };
	static std::atomic<int>    gObjNoQuad{ 0 };

	// v447：**移动物品盖章（用户"头盔掉地滚动也出效果"）**——游戏线程，200ms 节流。
	// ForEachReferenceInRange 玩家周围 1500 → 可掉落物（MISC/WEAP/ARMO/ALCH/INGR/
	// AMMO/KEYM/BOOK/SLGM，排除静态/建筑）→ 位置跟踪（lastObjPos）→ 移动 >20 单位
	// + 贴地（z - GetLandHeight ∈ [-50,100]）→ 盖章（圆形 r=10，与脚印同场/同合成，
	// 深度 0.7 压缩度）——掉落/滚动的头盔会在雪/沙/泥上拖出沟壑。
	void SnowShellMesh::ScanMovingObjects()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		auto* tes = RE::TES::GetSingleton();
		if (!player || !tes)
			return;
		const auto ppos = player->GetPosition();
		tes->ForEachReferenceInRange(player, 1500.0f, [&](RE::TESObjectREFR* ref) {
			if (!ref || ref->IsDeleted() || ref->formID == 0)
				return RE::BSContainer::ForEachResult::kContinue;
			auto* base = ref->GetBaseObject();
			if (!base)
				return RE::BSContainer::ForEachResult::kContinue;
			const auto ft = base->GetFormType();
			// 可掉落物类型（物品/武器/护甲/药剂/材料/弹药/钥匙/书/灵魂石）
			const bool droppable = (ft == RE::FormType::Misc || ft == RE::FormType::Weapon ||
				ft == RE::FormType::Armor || ft == RE::FormType::AlchemyItem ||
				ft == RE::FormType::Ingredient || ft == RE::FormType::Ammo ||
				ft == RE::FormType::KeyMaster || ft == RE::FormType::Book ||
				ft == RE::FormType::SoulGem || ft == RE::FormType::LeveledItem);
			if (!droppable)
				return RE::BSContainer::ForEachResult::kContinue;
			const auto p = ref->GetPosition();
			auto it = lastObjPos.find(ref->formID);
			if (it == lastObjPos.end()) {
				// 首见只记录不盖（防物品落地瞬间/初始位置误盖）
				// v455-dbg：记录 GetPosition vs Get3D 位置差（验证 REFR 滞后理论）
				if (auto* r3 = ref->Get3D(false)) {
					const float lag = (p - r3->world.translate).Length();
					gPosLagSum.fetch_add(static_cast<int>(lag * 100.0f), std::memory_order_relaxed);
					gPosLagN.fetch_add(1, std::memory_order_relaxed);
				}
				lastObjPos[ref->formID] = ObjTrack{ p, p, 0 };
				gScanned++;
				return RE::BSContainer::ForEachResult::kContinue;
			}
			gScanned++;
			gDroppable++;
			// v453：**CS 对齐（josef pr2659 Stamping.cpp 实锤）**——
			// 1) **位置源 = Get3D(false)->world.translate（3D 根世界变换）**——
			//    CS 注释：Havok 每帧移动场景图，REFR GetPosition 滞后到物体静止
			//    才同步 → 我们 v447-452 用 GetPosition → moved=0（踢动检测不到）
			// 2) **frozen anchor 累计位移**：静止时保持 prev 位置，位移累积过
			//    kStampMovementGate(3) 才触发（慢移动也能累计到门限）
			// 3) 贴地用 **worldBound.radius**（bound.z - radius > ground+40 跳过=空中）
			// 4) 盖章半径 = clamp(bound.radius, 4, 128)（CS kMin/MaxRadius）
			const auto objRoot = ref->Get3D(false);
			if (!objRoot)
				return RE::BSContainer::ForEachResult::kContinue;
			const auto position = objRoot->world.translate;
			// frozen anchor：位移累计（静止保持 prev，移动才更新）
			const bool propMoved =
				position.GetSquaredDistance(it->second.pos) >= 3.0f * 3.0f;
			if (!propMoved)
				return RE::BSContainer::ForEachResult::kContinue;  // 静止（回填会埋掉）
			it->second.pos = position;
			// v454：**形状级碰撞 bound（用户"盖章是很大很宽的大块"实锤）**——
			// worldBound 是**整棵树的外包围球**（头盔等 → 半径大 → 大块），CS 正确
			// 做法 = **TraverseScenegraphCollision 逐碰撞形状**取 GetColliderBound
			//（v346 玩家碰撞体同款），worldBound 只是无形状时的 fallback。
			float groundZ = position.z;
			tes->GetLandHeight(position, groundZ);
			const unsigned long nowMs = GetTickCount();
			// v497：**印章间距 25→8 + 冷却 200→100ms（用户图示"蓝线连续单沟，红线波浪多坑"）**——
			// 印章直径 = 2×objR = 16。间距 25 > 16 → 相邻印章独立方块，印章之间
			// 留小凹陷。间距改 **8**（<16）→ 相邻印章**重叠 8 单位**成连续条带，
			// 雪堆环连成一片 → 单峰单坑连续沟壑（贴合蓝线）。冷却也减半避免漏盖。
			const bool cooled = (nowMs - it->second.stampTime) >= 100;
			if (!cooled)
				return RE::BSContainer::ForEachResult::kContinue;
			{
				const float gdx = position.x - it->second.stampPos.x;
				const float gdy = position.y - it->second.stampPos.y;
				const float gd = std::sqrt(gdx * gdx + gdy * gdy);
				if (gd < 8.0f && it->second.stampTime != 0)
					return RE::BSContainer::ForEachResult::kContinue;  // 距上次盖章 <8 不盖（让印章重叠）
			}
			// v466：**删密度门（v465 实锤：stamps 61→5——物品踢出玩家高密
			// quadrant 全被挡 → 用户"没有沟壑了"）**。变形场是世界坐标的，
			// 低密 17² quad 顶点同样采样场（只是 128 间距视觉粗）——有痕迹
			// 优先于精细。低密区粗细问题后续用"盖章半径按 quad 密度缩放"解。
			if (!landReady.load())
				return RE::BSContainer::ForEachResult::kContinue;
			uint32_t shapeCount = 0;
			float lastR = 0.0f;
			// v456：**物品盖章半径压缩（用户"头盔沟壑宽度非常大"实锤）**——
			// CS 像素级变形图（2048² 平滑）同半径视觉小；我们几何顶点场（32 单位/
			// 格 + 双线性）视觉放大。且**滚动是线接触**（拖痕应比物品半径窄）——
			// 盖章半径 = 碰撞半径 × 0.5（10.2 → 5.1，直径贴合头盔）。
			// v477：**物品沟壑三参数集中控制（用户"重写，宽深长可控"）**——
			// kObjWidth = 宽度半轴（直径 = 2×kObjWidth，改 5=窄 10=中 15=宽）
			// kObjDepth = 深度场值（顶点下陷 = kObjDepth×18，改 0.3=浅 0.6=中 1.0=深）
			// 长度 = prev→current 移动距离（胶囊自动，不改常量）
			// v483：**真实雪堆效果（用户图例"方块不像自然雪堆"）**——自然雪被压
			// 是**碗形凹陷 + 周围雪堆隆起环**（不是切割平台）。改：
			//  1) **falloff 圆滑**：(0.4, 0.65) → (0.15, 0.85) 满深区更小、渐变更宽
			//  2) **加雪堆环**：dist ∈ (objR, 2.4×objR) 隆起（之前 v470b 关了）
			// v489：**最后差异——depth 1.0→0.6（玩家首踩同款）**——玩家脚印 depth 初始
			// 0.6（-10.8 凹 + 7.2 雪堆），重复踩才累加到 1.0。物品单次盖章用 1.0
			// = 比玩家首踩深（突兀）。改 0.6 = 玩家首踩分毫不差。
			// v492：**物品独立分支调参（用户"满深区深一点、雪堆环高一点"）**——
			// 物品不再与玩家共用参数。kObjDepth 恢复 0.6（与玩家首踩一致），
			// 物品分支内部 objD = 0.6×1.33 ≈ 0.8（-14.4，比玩家 -10.8 深）；
			// 雪堆环 m=16（玩家 12）。kObjWidth=8 → rC=20 战壕宽与玩家一致。
			constexpr float kObjWidth = 8.0f;
			constexpr float kObjDepth = 0.6f;
			RE::BSVisit::TraverseScenegraphCollision(objRoot,
				[&](RE::bhkNiCollisionObject* colObj) -> RE::BSVisit::BSVisitControl {
					RE::NiPoint3 cpos;
					float crad = 0.0f;
					if (!GetColliderBound(colObj, cpos, crad))
						return RE::BSVisit::BSVisitControl::kContinue;
					gObjShapeTot.fetch_add(1, std::memory_order_relaxed);
					// 贴地门：碰撞体底不高于地面 + 40（kStampSurfaceBand）→ 空中不盖
					if (cpos.z - crad > groundZ + 40.0f) {
						gObjAirGate.fetch_add(1, std::memory_order_relaxed);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// 半径门（原始碰撞半径）4..128
					if (crad < 4.0f || crad > 128.0f) {
						gObjRadGate.fetch_add(1, std::memory_order_relaxed);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// v467-dbg：通过门的形状——原始碰撞半径分布
					{
						const int ci = static_cast<int>(crad);
						gObjCradSum.fetch_add(ci, std::memory_order_relaxed);
						gObjCradN.fetch_add(1, std::memory_order_relaxed);
						int mn = gObjCradMin.load(std::memory_order_relaxed);
						while (ci < mn && !gObjCradMin.compare_exchange_weak(mn, ci, std::memory_order_relaxed)) {}
						int mx = gObjCradMax.load(std::memory_order_relaxed);
						while (ci > mx && !gObjCradMax.compare_exchange_weak(mx, ci, std::memory_order_relaxed)) {}
					}
					if (shapeCount >= 6)
						return RE::BSVisit::BSVisitControl::kStop;  // 每物品 ≤6 形状
					{
						// v480：**宽度 = 碰撞半径**（沟壑直径 = 物品实际大小，头盔 42）
						const float sr = kObjWidth;  // v482：固定宽度（边长 2×8=16）
						// v467-dbg：**间距/胶囊统计**
						{
							const float gdx = position.x - it->second.stampPos.x;
							const float gdy = position.y - it->second.stampPos.y;
							const float gap = std::sqrt(gdx * gdx + gdy * gdy);
							gObjGapSum.fetch_add(static_cast<int>(gap * 10.0f), std::memory_order_relaxed);
							gObjGapN.fetch_add(1, std::memory_order_relaxed);
							if (it->second.stampTime != 0 && gap > 0.01f && gap < 256.0f)
								gObjCapsule.fetch_add(1, std::memory_order_relaxed);
						}
						// v458：**盖章位置用 3D 根（objRoot->world.translate）不用质心**——
						// GetColliderBound 返回 massCenter（物理质心，与 3D 根位置
						// 不一致，物品盖章偏移；用户"头盔在左凹槽在右"实锤）。半径仍
						// 用碰撞形状半径（贴合形状）——位置+半径各取最佳源。
						std::lock_guard<std::mutex> lk(footMtx);
						// 位置诊断（v458-dbg）：3D 根 vs 碰撞质心 差（验证错位）
						gObjPosDx.fetch_add(static_cast<int>((position.x - cpos.x) * 100.0f), std::memory_order_relaxed);
						gObjPosDy.fetch_add(static_cast<int>((position.y - cpos.y) * 100.0f), std::memory_order_relaxed);
						gObjPosN.fetch_add(1, std::memory_order_relaxed);
						// v468：**prev = 上次盖章位置（CS StampEnds 胶囊）**——首个盖章
						//（stampTime==0）prev=current（单点）；之后 prev=stampPos →
						// RebuildField 战壕段（segLen>0）画胶囊 → 轨迹连续条带。
						const float pvx = (it->second.stampTime != 0) ? it->second.stampPos.x : position.x;
						const float pvy = (it->second.stampTime != 0) ? it->second.stampPos.y : position.y;
						// v477：depth = kObjDepth（0.6，顶点 -10.8 深）
						footprints.push_back({ position.x, position.y, kObjDepth, 0.0f, 0.0f, 0.0f, sr, sr, pvx, pvy, 9, GetTickCount() });
						if (footprints.size() > gPlayerFpMax) {  // v573: INI 可调（默认 400，玩家可增——见 LoadConfig）
							// v553：物品坑独立保护（同玩家脚印处）
							const std::size_t objCnt = std::count_if(footprints.begin(), footprints.end(),
								[](const Footprint& f) { return f.shape > 3; });
							constexpr std::size_t kObjFpMax = 60;  // v605：80→60（动物/尸体脚印上限收紧，fp 442 卡顿修复）
							if (objCnt > kObjFpMax) {
								if (auto itE = std::find_if(footprints.begin(), footprints.end(),
									[](const Footprint& f) { return f.shape > 3 && f.dieAt == 0; });
									itE != footprints.end())
									{ itE->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
							} else {
								if (auto itP = std::find_if(footprints.begin(), footprints.end(),
									[](const Footprint& f) { return f.shape <= 3 && f.dieAt == 0; });
									itP != footprints.end())
									{ itP->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
								else
									{ footprints.begin()->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 兜底标记最老淡出）
							}
						}
						lastR = sr;
						// v459：每次盖章详细日志（每 4 次打印 1 次，定位宽度来源）
						// v474b：rC 打印对齐 RebuildField 实际值（v463 时代写死 20 误导）
						if ((gObjStamps.load() & 0x3) == 0) {  // 每 4 次打印 1 次
							SKSE::log::info("v461-dbg: stamp root=({:.0f},{:.0f}) mass=({:.0f},{:.0f}) diff=({:.1f},{:.1f}) sr={:.2f} rC={:.2f} (max(sr×1.2,7))",
								position.x, position.y, cpos.x, cpos.y,
								position.x - cpos.x, position.y - cpos.y, sr,
								std::max(sr * 1.2f, 7.0f));
						}
					}
					shapeCount++;
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			if (shapeCount > 0) {
				landFootDirty.store(true);
				// v480f：**首个盖章判断必须在 stampTime 赋值前**（原顺序 stampTime=nowMs
				// 先执行 → ==0 永远 false → gObjFirstX 永不存 → v480c 不打印）
				if (it->second.stampTime == 0) {  // v480e：首个盖章（单点圆）位置
					gObjFirstX.store(position.x, std::memory_order_relaxed);
					gObjFirstY.store(position.y, std::memory_order_relaxed);
				}
				it->second.stampPos = { position.x, position.y, position.z };
				it->second.stampTime = nowMs;
				gObjStamps.fetch_add(1, std::memory_order_relaxed);
				gObjLastX.store(position.x, std::memory_order_relaxed);
				gObjLastY.store(position.y, std::memory_order_relaxed);
				gShapesSum.fetch_add(static_cast<int>(shapeCount), std::memory_order_relaxed);
				gShapesN.fetch_add(1, std::memory_order_relaxed);
				// v469-dbg：**落点地块查询**——物品盖章位置落在哪个 quad（密度/材质）
				{
					const int bidx = landBufIdx.load();
					const float cox = std::floor(ppos.x / 4096.0f) * 4096.0f;
					const float coy = std::floor(ppos.y / 4096.0f) * 4096.0f;
					const int dcx = static_cast<int>(std::floor((position.x - cox) / 4096.0f));
					const int dcy = static_cast<int>(std::floor((position.y - coy) / 4096.0f));
					int qv = -1, qs = -1;
					if (dcx >= -3 && dcx <= 3 && dcy >= -3 && dcy <= 3) {
						const int ci = (dcy + 3) * 7 + (dcx + 3);
						for (int qd = 0; qd < 4; qd++) {
							const auto& cg = landBuf[bidx][ci][qd];
							if (cg.verts == 0)
								continue;
							const float gx = cg.worldT[0], gy = cg.worldT[1];
							// v480c：**quad 范围匹配修复**——锚点 = cell 原点+2048：
							// q3 高密覆盖东北 [gx, gx+2048]；q0-2 低密覆盖西南 [gx-2048, gx]
							//（原检测只查东北 → 物品在西南 quadrant 误报 noQuad）
							const float qx0 = (qd == 3) ? gx : gx - 2048.0f;
							const float qy0 = (qd == 3) ? gy : gy - 2048.0f;
							if (position.x >= qx0 && position.x <= qx0 + 2048.0f &&
								position.y >= qy0 && position.y <= qy0 + 2048.0f) {
								qv = static_cast<int>(cg.verts);
								qs = cg.surfaceClass;
								break;
							}
						}
					}
					if (qv == (int)(highResDim * highResDim))
						gObjQHigh.fetch_add(1, std::memory_order_relaxed);
					else if (qv > 0)
						gObjQLow.fetch_add(1, std::memory_order_relaxed);
					else
						gObjNoQuad.fetch_add(1, std::memory_order_relaxed);
					if (qs == 0) gObjS0.fetch_add(1, std::memory_order_relaxed);
					if (qs == 1) gObjS1.fetch_add(1, std::memory_order_relaxed);
					if (qs == 2) gObjS2.fetch_add(1, std::memory_order_relaxed);
					if (qs == 3) gObjS3.fetch_add(1, std::memory_order_relaxed);
					if (qv < 0) {
						// v480b-dbg：**落点查询失败详情**——玩家位置/物品位置/锚点/
						// 缓冲代/ci——一次定位为什么 71% 盖章找不到 quad
						SKSE::log::info("v480b-dbg: NOQUAD ppos=({:.0f},{:.0f}) obj=({:.0f},{:.0f}) dc=({},{}) bidx={} landReady={} anchor=({:.0f},{:.0f})",
							ppos.x, ppos.y, position.x, position.y, dcx, dcy, bidx,
							landReady.load() ? 1 : 0, landAnchorX.load(), landAnchorY.load());
					}
					// 每 4 次盖章打印 1 次落点详情（密度/材质/坐标）
					if ((gObjStamps.load() & 0x3) == 0) {
						SKSE::log::info("v469-dbg: stamp at=({:.0f},{:.0f}) cell=({},{}) qVerts={} sClass={} (0=other 1=snow 2=sand 3=mud)",
							position.x, position.y, dcx, dcy, qv, qs);
					}
				}
				SKSE::log::info("v454: obj stamp shapes={} at=({:.0f},{:.0f}) r={:.1f}",
					shapeCount, position.x, position.y, lastR);
			} else {
				// 无形状提取（MOPP/list）→ fallback：worldBound 单盖
				const float br = objRoot->worldBound.radius;
				if (br >= 4.0f && br <= 128.0f && position.z - br <= groundZ + 40.0f) {
					const float sr = kObjWidth;  // v482：固定宽度（fallback 同）
					const float pvx = (it->second.stampTime != 0) ? it->second.stampPos.x : position.x;
					const float pvy = (it->second.stampTime != 0) ? it->second.stampPos.y : position.y;
					std::lock_guard<std::mutex> lk(footMtx);
					// v477：depth = kObjDepth
						footprints.push_back({ position.x, position.y, kObjDepth, 0.0f, 0.0f, 0.0f, sr, sr, pvx, pvy, 9, GetTickCount() });
					if (footprints.size() > gPlayerFpMax) {  // v573: INI 可调（默认 400，玩家可增——见 LoadConfig）
						// v553：物品坑独立保护（同玩家脚印处）
						const std::size_t objCnt = std::count_if(footprints.begin(), footprints.end(),
							[](const Footprint& f) { return f.shape > 3; });
						constexpr std::size_t kObjFpMax = 60;  // v605：80→60（动物/尸体脚印上限收紧，fp 442 卡顿修复）
						if (objCnt > kObjFpMax) {
							if (auto itE = std::find_if(footprints.begin(), footprints.end(),
								[](const Footprint& f) { return f.shape > 3 && f.dieAt == 0; });  // v574：跳过淡出中
								itE != footprints.end())
								{ itE->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
						} else {
							if (auto itP = std::find_if(footprints.begin(), footprints.end(),
								[](const Footprint& f) { return f.shape <= 3 && f.dieAt == 0; });
								itP != footprints.end())
								{ itP->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
							else
								{ footprints.begin()->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 兜底标记最老淡出）
						}
					}
					landFootDirty.store(true);
					it->second.stampPos = { position.x, position.y, position.z };
					it->second.stampTime = nowMs;
					SKSE::log::info("v456: obj stamp (worldBound fb) r={:.1f}", sr);
				}
			}
			gMoved += 1;
			gNearGround += 1;
			// 缓存上限（防无限增长）
			if (lastObjPos.size() > 200) {
				for (auto it2 = lastObjPos.begin(); it2 != lastObjPos.end();) {
					if (std::fabs(it2->second.pos.x - ppos.x) > 1500.0f ||
						std::fabs(it2->second.pos.y - ppos.y) > 1500.0f)
						it2 = lastObjPos.erase(it2);
					else
						++it2;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		// v451-dbg：物品扫描诊断（3 秒）——scanned=扫到物品数 droppable=可掉落
		// moved=移动中 nearGround=贴地（定位物品盖章不触发的卡点）
		{
			static unsigned long lastObjDbg = 0;
			const unsigned long nowDbg = GetTickCount();
			if (nowDbg - lastObjDbg >= 3000) {
				lastObjDbg = nowDbg;
				// v465-dbg：+rebObjFoot/rebObjWrite（RebuildField 物品脚印采样数/实际
				// 写入数——增长 = 物品坑真实进场；foot 涨但 write 不涨 = 物品分支没写）
				const int crN = gObjCradN.load(std::memory_order_relaxed);
				const int gpN = gObjGapN.load(std::memory_order_relaxed);
				// v467-dbg：**物品盖章全链路检测（CS pr2659 逐条对齐）**——
				// shapeTot=遍历形状总数；airGate=贴地门挡；radGate=半径门挡（<4/>128）；
				// cradMin/Max/Avg=通过门形状的原始碰撞半径（CS 用原半径×StampRadius/20
				// 不压缩，我们 ×0.5——看真实半径判断压缩合理性）；
				// gapAvg=盖章间距实际值（×10 存，CS 每帧盖无节流，我们 8 间距+300ms
				// 冷却）；capsule=距上次盖章 0..256 的次数（CS StampEnds 胶囊可行前提）
				SKSE::log::info("v469-dbg: scanned={} droppable={} moved={} nearGround={} stamps={} shapeTot={} airGate={} radGate={} cradMin={} cradMax={} cradAvg={} gapAvg={} capsule={} rebFoot={} rebWrite={} qHigh={} qLow={} s0={} s1={} s2={} s3={} noQuad={}",
					gScanned.load(std::memory_order_relaxed),
					gDroppable.load(std::memory_order_relaxed),
					gMoved.load(std::memory_order_relaxed),
					gNearGround.load(std::memory_order_relaxed),
					gObjStamps.load(std::memory_order_relaxed),
					gObjShapeTot.load(std::memory_order_relaxed),
					gObjAirGate.load(std::memory_order_relaxed),
					gObjRadGate.load(std::memory_order_relaxed),
					gObjCradMin.load(std::memory_order_relaxed),
					gObjCradMax.load(std::memory_order_relaxed),
					crN > 0 ? gObjCradSum.load(std::memory_order_relaxed) / crN : 0,
					gpN > 0 ? gObjGapSum.load(std::memory_order_relaxed) / 10 / gpN : 0,
					gObjCapsule.load(std::memory_order_relaxed),
					gRebObjFoot.load(std::memory_order_relaxed),
					gRebObjWrite.load(std::memory_order_relaxed),
					gObjQHigh.load(std::memory_order_relaxed),
					gObjQLow.load(std::memory_order_relaxed),
					gObjS0.load(std::memory_order_relaxed),
					gObjS1.load(std::memory_order_relaxed),
					gObjS2.load(std::memory_order_relaxed),
					gObjS3.load(std::memory_order_relaxed),
					gObjNoQuad.load(std::memory_order_relaxed));
			}
		}
	}

	// v506：**武器碰撞雪坑（Precision 兼容版）**——v505 数据实锤：
	// Precision 环境下 `GetAttackState()` 恒为 4（从不进 Swing/Hit/FollowThrough）
	// → 依赖攻击状态的 v504 永远零触发。**绕开攻击状态，改为武器碰撞体贴地检测**：
	// 每帧（50ms 调度）检查武器 3D 碰撞体底部是否距地面 <60 单位 = 武器碰地 →
	// 盖章（shape=9 物品分支形态，渐进 -5/次，同格 5 次上限）。任何武器挥到地面
	// 都出坑（不限斧头——"武器碰撞雪坑"普适），砍到哪坑在哪（跟随挥砍动画）。
	void SnowShellMesh::ScanPlayerMining()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		auto* tes = RE::TES::GetSingleton();
		if (!player || !tes)
			return;
		// 武器 3D 碰撞体贴地检测
		bool hit = false;
		RE::NiPoint3 hitPos{};
		float hitR = 0.0f;
		float hitHalfL = 0.0f, hitHalfW = 0.0f;  // v515：武器长/粗半轴（拖痕用真实尺寸）
		// v523：**武器最低点定位（用户"拖拽武器雪沟壑依旧没看到"——盖章位置 =
		// 质心水平投影（武器中部），不是武器尖端/拖地接触点 → 视觉错位像没有）**。
		// 遍历全部碰撞体，记录 **底部 z（cpos.z - crad）最低** 的形状 = 武器真实
		// 拖地接触点（刀尖/最低端）。拖痕/击打都用它盖章。
		float bestBottomZ = 1.0e30f;
		if (auto* root3d = player->Get3D(false)) {
			if (SnowDeform::RTTIIsA(root3d, "NiNode")) {
				auto* wnode = static_cast<RE::NiNode*>(root3d)->GetObjectByName("Weapon R");
				if (!wnode)
					wnode = static_cast<RE::NiNode*>(root3d)->GetObjectByName("Weapon");
				if (wnode) {
					RE::BSVisit::TraverseScenegraphCollision(wnode,
						[&](RE::bhkNiCollisionObject* colObj) -> RE::BSVisit::BSVisitControl {
							RE::NiPoint3 cpos;
							float crad = 0.0f;
							if (!GetColliderBoundList(colObj, cpos, crad))  // v527：List 版（武器 ListShape 支持，仅武器盖章）
								return RE::BSVisit::BSVisitControl::kContinue;
							// v515：**crad 上限 64→128（v507 实锤：武器圆柱 crad=71.5 被滤掉，
							// 盖章用了小碰撞体 → 拖痕尺寸不对）**
							if (crad < 2.0f || crad > 128.0f)
								return RE::BSVisit::BSVisitControl::kContinue;
							float lh = -30000.0f;
							if (!tes->GetLandHeight(RE::NiPoint3{ cpos.x, cpos.y, 0.0f }, lh))
								return RE::BSVisit::BSVisitControl::kContinue;
							// v513：**贴地阈值 150→300（用户"继续放宽碰撞距离"）**——
							// 武器离地 300 内就算碰地盖章（配合加长武器/敲击动作）。
							if (cpos.z - crad > lh + 300.0f)
								return RE::BSVisit::BSVisitControl::kContinue;
							// v523：**只保留底部 z 最低（最贴地）的形状**——其余跳过
							const float bottomZ = cpos.z - crad;
							if (bottomZ > bestBottomZ)
								return RE::BSVisit::BSVisitControl::kContinue;
							bestBottomZ = bottomZ;
							// v515：保存武器形状半轴（拖痕长度/宽度用真实尺寸）
							{
								float a = crad, b = crad, c = crad;
								if (auto* sp = GetColliderShapePtr(colObj)) {
									const float inv = RE::bhkWorld::GetWorldScaleInverse();
									auto proj = [sp, inv](float x, float y, float z) {
										return sp->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * inv;
									};
									a = proj(1, 0, 0);
									b = proj(0, 1, 0);
									c = proj(0, 0, 1);
								}
								hitHalfL = std::max(a, std::max(b, c));          // 长半轴（武器长度）
								const float mn = std::min(a, std::min(b, c));    // 最短
								hitHalfW = a + b + c - hitHalfL - mn;            // 中间值（武器粗细）
								if (hitHalfW < 2.0f)
									hitHalfW = hitHalfL;  // 单一半轴退化（球）
							}
							hit = true;
							hitPos = cpos;
							hitR = crad;
							return RE::BSVisit::BSVisitControl::kContinue;  // 继续遍历找更低点
						});
				}
			}
		}
		if (!hit)
			return;
		// v513：**敲击检测修复（用户"棍子敲击地面没深坑"）**——
		// ① 位置节流（8 单位）删除——同位置连续敲击距离<8 被挡 → 换成**时间冷却**
		//   （250ms，防同一帧/抖动重复盖，但不挡连续敲击）
		// ② 5 次上限（mineCounts）移除——武器坑是碰撞效果，每次敲击都该出坑
		//   （5 次上限是 v448 采集逻辑，不适用于武器碰撞）
		static unsigned long lastWeaponStrike = 0;
		const unsigned long nowWs = GetTickCount();
		if (lastWeaponStrike != 0 && nowWs - lastWeaponStrike < 250)
			return;
		lastWeaponStrike = nowWs;
		// v519：**深坑固定深度（用户"不要坑越来越大，只要些许变化，不加深度只变
		// 雪堆"）**——shape=10 分支 objD 固定 1.0（-18 玩家同深，不叠加）；雪堆
		// m=6×objD×(1+0.15n) 随同格击打次数 n 稍增（变化但不盖坑）。fp.depth=nW
		// 仍传（供 ridgeMul 用）。拖痕（shape=11）= 物品同款（v520）。
		const std::int64_t gxW = static_cast<std::int64_t>(std::floor(hitPos.x / 40.0f));
		const std::int64_t gyW = static_cast<std::int64_t>(std::floor(hitPos.y / 40.0f));
		// v609：gxW/gyW 可为负（负坐标），有符号负数左移 = 标准 UB → 先截断为 uint32 再拼
		const std::int64_t keyW = static_cast<std::int64_t>(
			(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gxW)) << 32) |
			static_cast<std::uint32_t>(gyW));
		auto itW = mineCounts.find(keyW);
		const int nW = (itW == mineCounts.end()) ? 1 : itW->second + 1;
		mineCounts[keyW] = nW;
		const float depth = static_cast<float>(nW);  // shape=10 objD=depth → -18n 叠加
		// v531：**拖拽全面取消（用户拍板），只保留武器攻击碰撞（敲击深坑 shape=10）**——
		// 走路/拖地（武器慢速触地）不再盖章。敲击 = 玩家静止（站定挥击）+ 武器快速
		// 移动（挥动砸地）；走路/跑步（玩家移动下垂）、站定下垂（武器静止）→ 一律
		// 不盖章。
		static RE::NiPoint3 lastWpnPos{};
		static unsigned long lastWpnMs = 0;
		const unsigned long nowSpd = GetTickCount();
		float wpnSpeed = 0.0f;
		{
			const float dd = (hitPos - lastWpnPos).Length();
			const float dt = (nowSpd - lastWpnMs) / 1000.0f;
			if (lastWpnMs != 0 && dt > 0.001f)
				wpnSpeed = dd / dt;
		}
		lastWpnPos = hitPos;
		lastWpnMs = nowSpd;
		// 玩家移动速度（位置差分，同 250ms 窗口）
		static RE::NiPoint3 lastPcPos{};
		static unsigned long lastPcMs = 0;
		float pcSpeed = 0.0f;
		{
			const auto pcp = player->GetPosition();
			const float dd = (pcp - lastPcPos).Length();
			const float dt = (nowSpd - lastPcMs) / 1000.0f;
			if (lastPcMs != 0 && dt > 0.001f)
				pcSpeed = dd / dt;
			lastPcPos = pcp;
			lastPcMs = nowSpd;
		}
		// v532：**速度门放宽（02:19 数据实锤：gate 317→343 每秒 +4~5 = 攻击全被
		// 挡——v531"站定敲击"太严：移动中攻击 pcSpeed>50、挥击峰值错过 250ms
		// 采样都拦）**。判定 = 武器速度显著高于玩家移动速度（挥击叠加移动）：
		//   wpnSpeed > max(250, pcSpeed×1.5+150)
		// 走路下垂：wpnSpeed≈pcSpeed(100-200) < 250 → 拦截 ✓
		// 跑步下垂：wpnSpeed≈pcSpeed(400) < pcSpeed×1.5+150(750) → 拦截 ✓
		// 站定敲击：dragSpd=324 实锤 > 250 → 通过 ✓
		// 移动挥击：wpnSpeed≈324+pcSpeed > pcSpeed×1.5+150 → 通过 ✓
		if (!(wpnSpeed > std::max(250.0f, pcSpeed * 1.5f + 150.0f))) {
			gDragSpeedGate.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const int wshape = 10;  // v531：只剩深坑（拖痕 11 已取消）
		// v531：**深坑 push 参数（拖拽取消后仅深坑）**——depth=nW（雪堆 1+0.15n
		// 随同格敲击次数稍增）、rL/rS=8（战壕 40 宽，与物品/玩家一致，v527b）、
		// prev=current（单点敲击，无连续拖线）。
		const float pushDepth = depth;
		const float pushRL = 8.0f;
		const float pushRS = 8.0f;
		// v594：**武器坑同位置最多 2 下（用户"前面 2 下有效，后面无论多少下取消
		// 效果，不再挖深"）**——50 单位内已有 ≥2 个武器坑（shape=10）→ 本次跳过
		// 不盖章。列表项 300s 半衰但保留在列表 → 该位置永久限制（后面怎么打都不
		// 再加深）。范围 50 = 一次挥击的落点漂移容差。
		{
			std::lock_guard<std::mutex> lk(footMtx);
			int wCnt = 0;
			for (const auto& f : footprints) {
				if (f.shape == 10) {
					const float ddx = f.x - hitPos.x, ddy = f.y - hitPos.y;
					if (ddx * ddx + ddy * ddy < 50.0f * 50.0f)
						wCnt++;
				}
			}
			if (wCnt >= 2) {
				return;  // v594：该位置已挖满 2 下，后续攻击无效（不盖章不重建）
			}
			footprints.push_back({ hitPos.x, hitPos.y, pushDepth, 0.0f, 0.0f, 0.0f, pushRL, pushRS, hitPos.x, hitPos.y, wshape, GetTickCount() });
			if (footprints.size() > gPlayerFpMax) {  // v573: INI 可调（默认 400，玩家可增——见 LoadConfig）
				// v553：物品坑独立保护（同玩家脚印处）
				const std::size_t objCnt = std::count_if(footprints.begin(), footprints.end(),
					[](const Footprint& f) { return f.shape > 3; });
				constexpr std::size_t kObjFpMax = 128;
				if (objCnt > kObjFpMax) {
					if (auto itE = std::find_if(footprints.begin(), footprints.end(),
						[](const Footprint& f) { return f.shape > 3 && f.dieAt == 0; });
						itE != footprints.end())
						{ itE->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
				} else {
					if (auto itP = std::find_if(footprints.begin(), footprints.end(),
						[](const Footprint& f) { return f.shape <= 3 && f.dieAt == 0; });  // v574：跳过淡出中
						itP != footprints.end())
						{ itP->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 淡出标记）
					else
						{ footprints.begin()->dieAt = GetTickCount(); gFadeMarkN.fetch_add(1, std::memory_order_relaxed); }  // v575：驱逐标记计数（v574 兜底标记最老淡出）
				}
			}
		}
		gDragSpeed.store(wpnSpeed, std::memory_order_relaxed);  // v530：速度门调参
		gPcSpeed.store(pcSpeed, std::memory_order_relaxed);     // v532：速度门调参
		landFootDirty.store(true);
		// 格计数缓存上限（远离玩家的格清除，v516 恢复）
		if (mineCounts.size() > 500) {
			const auto ppos = player->GetPosition();
			for (auto it2 = mineCounts.begin(); it2 != mineCounts.end();) {
				const float dx = (static_cast<float>(it2->first >> 32) * 40.0f) - ppos.x;
				const float dy = (static_cast<float>(static_cast<std::int32_t>(it2->first & 0xFFFFFFFF)) * 40.0f) - ppos.y;
				if (dx * dx + dy * dy > 3000.0f * 3000.0f)
					it2 = mineCounts.erase(it2);
				else
					++it2;
			}
		}
	}

	// v130：渲染线程每帧——对 3×3 cell 全部 landscape geom 顶点做脚印变形
	// （世界坐标连续场 → 跨 cell/quadrant 边界顶点同步下陷 → 无缝）+ 有变形才
	// UpdateSubresource 上传。盖章（每 12 单位一个脚印）移出 geom 循环只盖一次；
	// 脚印按包围盒粗筛（nearFp）降开销；玩家移动 >1024 单位触发重缓存（节流 800ms）。
	void SnowShellMesh::UpdateLandscape(ID3D11DeviceContext* a_ctx)
	{
		if (!landReady.load() || !a_ctx)
			return;
		// v157：移除 v149 实验（4 象限抬高 300）——实验已完成使命：v156 实锤
		// mesh.child 为真正渲染对象（用户实测 4 块全抬高 ✓）。恢复正常变形。
		if (landPaused.load())
			return;
		// v545：**land 内部阶段计时工具（用户"添加检测帧数和数据变化的工具，下次进游戏
		// 就知道具体哪个地方导致帧数下降"）**——细粒度拆分 land 耗时来源：RebuildField
		// （场重建 34 万循环）/ geom 顶点循环（变形+ConeCS+沙丘+法线）/ 上传，每 2s
		// 汇总输出 v545-perf 行（含数据变化：处理 geom 数/变形顶点/上传 KB/脚印数）。
		using clkLd = std::chrono::steady_clock;
		static long long sLdRfUs = 0, sLdVtUs = 0, sLdUpUs = 0;
		static long long sLdRfMaxUs = 0, sLdVtMaxUs = 0, sLdUpMaxUs = 0;
		static int    sLdFrames = 0, sLdGeoms = 0, sLdDeform = 0, sLdUpKB = 0;
		static std::size_t sLdFp = 0;
		static auto   sLdT0 = clkLd::now();
		const auto tLd0 = clkLd::now();
		// v197：性能门控——footprints 不变时变形结果不变，无需每帧重算上传
		// （181²×4=13 万顶点全量遍历+上传 = 帧率减半元凶）。盖章（新脚印）时
		// landFootDirty 置 true → 本帧全量重算+上传；无新脚印时跳过整个顶点
		// 循环（v160 检测仍在下面跑，重缓存后置 dirty 恢复重算）。
		// cell 切换检测：玩家移动 >512 单位（约 1/8 cell）→ 重新缓存 3×3。
		// v135：1024→512——引擎的 65×65 高分辨率 quadrant 跟随玩家（带滞后），
		// 缩小触发距离能更快捕获玩家脚下的高分辨率网格（细腻坑尽早出现）
		const auto now = GetTickCount();
		if (now - landLastRequest > 800) {
			const float dxa = playerPos.x - landAnchorX.load();
			const float dya = playerPos.y - landAnchorY.load();
			if (dxa * dxa + dya * dya > 512.0f * 512.0f) {
				landLastRequest = now;
				SKSE::log::info("v130: moved {:.0f} units → re-caching landscape",
					std::sqrt(dxa * dxa + dya * dya));
				SKSE::GetTaskInterface()->AddTask([this]() { FindLandscape(); });
			}
		}
		// v287：**落地检测盖章**——根因：v163/v285 按"玩家移动 60 单位"盖章，触发
		// 瞬间脚在摆动中（悬空/前摆/后摆），坑盖在摆动轨迹上 → "凹陷怪异不跟脚"
		// （日志实测：脚印相对玩家偏移 20-60 单位方向乱、身前身后都有）。改为检测脚
		// **真实落地**：脚 z 接近地形（gap<45）+ z 静止（|Δz|<2，支撑期）+ 水平
		// 钉地（|Δxy|<3，落地后脚相对世界不动；摆动最低点水平速度快→排除）+ 玩家
		// 在移动（>40 单位）→ 盖章。盖章频率 = 步频（每步 1 个），位置 = 实际落地点。
		if (!footInited) {
			lastFootPos = playerPos;
			footInited = true;
		}
		// v346：**Havok 碰撞体扫描调度**（游戏线程 50ms）——CS Dynamic Snow 同款
		// 安全方案：只读碰撞体（bhkNiCollisionObject），不读网格顶点（v342b 禁用
		// 网格形状扫描：spModelData.vertex 换装时悬空崩溃实锤）。
		// v360: 50->20ms (per-frame @60fps, CS-style continuous trail) + reentrancy guard
		// v437b：**ScanContactShapes 调度禁用（v437 闪退实锤）**——v437 恢复该调度后
		// 04:55:24 崩：SnowDeformationPlugin.dll+002F6DC movss xmm1,[rbx+r9*4]（读 float
		// 数组访问违规）= FindBootMesh 读 spModelData.vertex 悬空（换装/动画中网格释放）。
		// 印证 v342b 铁律：**网格顶点不可读（换装崩溃），只读碰撞体**。鞋形改由
		// v437b GetColliderShapeAxis 提取碰撞体形状半轴实现（碰撞体不随换装重建，安全）。
		// if (now - bootScanLast >= 500) {
		// 	bootScanLast = now;
		// 	SKSE::GetTaskInterface()->AddTask([this]() { ScanContactShapes(); });
		// }
		if (now - colliderScanLast >= 20) {
			colliderScanLast = now;
			SKSE::GetTaskInterface()->AddTask([this]() { ScanColliders(); });
		}
		// v447：**移动物品盖章调度（游戏线程 200ms）**——掉落/滚动物品在雪/沙/泥上
		// 拖出沟壑（头盔掉地滚动等）。ForEachReferenceInRange 开销 ~0.1ms，200ms
		// 节流可接受。
		if (now - movingScanLast >= 200) {
			movingScanLast = now;
			SKSE::GetTaskInterface()->AddTask([this]() { ScanMovingObjects(); });
		}
		// v448：**砍击采集调度（游戏线程 50ms）**——斧砍地面 → 凹陷（5 次上限）+
		// OIF 并行掉资源。攻击状态轮询开销极小。
		if (now - miningScanLast >= 50) {
			miningScanLast = now;
			SKSE::GetTaskInterface()->AddTask([this]() { ScanPlayerMining(); });
		}
		// v560：**动物脚印调度（游戏线程 100ms）**——ForEachHighActor 遍历玩家附近
		// 动物，Hoof/Paw/Foot 脚节点盖章（马匹/狗/牛等）。遍历开销 ~0.1ms，节流可接受。
		if (now - animalScanLast >= 100) {
			animalScanLast = now;
			SKSE::GetTaskInterface()->AddTask([this]() { ScanAnimalFeet(); });
		}
		// v444：**动态视差调度已移除（v563 清理）**——ENB wrap 周期重复无法消除（用户拍板关闭）
		// v439：地形碰撞体诊断调度（2 秒一次，游戏线程）——碰撞跟随下降调研用
		// v440：**禁用（v439g 崩溃实锤）**——broadphase QuerySingleAabb 的 handle
		// 反查读悬空（CommonLib 4.2.0 vtable 槽位不可靠，movsx [rdi] 崩）。三路验证
		// 结论：地形碰撞体不在 cell 3D 树/射线打不到/handle 反查崩 + 社区无先例
		// （Smooth Terrain 官方文档 "Collision is not updated"）→ **地形碰撞不可改**。
		// 碰撞跟随采用"浅坑 + 视觉碰撞分离"妥协（v440 kSnowDepth 30→18，悬空减半
		// 且被坑沿雪堆遮挡，视觉不可见）。
		// static unsigned long lastCellScan = 0;
		// if (now - lastCellScan >= 2000) {
		// 	lastCellScan = now;
		// 	SKSE::GetTaskInterface()->AddTask([this]() { ScanCellCollision(); });
		// }
		// v349：**碰撞体盖章禁用**（18:51 崩溃嫌疑 + stamps=0 未生效）——渲染线程遍历
		// colliders 与游戏线程 move 替换存在迭代器风险；且 v125 stamps=0（盖章从未
		// 触发，5 碰撞体在扫但位移门 24 未过/首见不盖）。恢复 v287 落地盖章（稳定）。
		// 碰撞体扫描诊断保留（v346 日志）。后续安全重做：盖章也在游戏线程。
		// v205：回填——脚印深度每帧向 0 恢复（Josef RefillTime 700s 移植）。
		// 回填改变变形结果但性能门控只在盖章时全量重算 → 每 120 帧（2 秒）
		// 强制一次重算上传；阶梯步长 0.00114×120≈0.14 单位/2 秒，视觉平滑。
		// 深度回 0 的脚印（雪已恢复）从列表移除释放 512 上限。
		// v410：**回填连续化（CS 对齐：每帧 deformation -= dt/RefillTime）**——
		// 原 120 帧（2 秒）阶梯一次减 kRefillPerFrame×120（等效但阶梯跳变）。
		// 改每帧减 kRefillPerFrame（=1/(86400×60)，60fps 下 1 天回完，平滑）。
		// 深度每帧变但 dirty 只每 250ms 置位（回填是缓慢过程，250ms 更新一次
		// 几何足够平滑，防每帧全量重算卡帧）。
		// v438e：**回填取消（用户：脚印永久，不随时间变浅）**——v205-v410 的回填
		// （fp.depth 每帧减 kRefillPerFrame，1 天回完）整块禁用：fp.depth 恒为盖章
		// 值（0.6~1.0 越踩越深），脚印永不消失（仅超 1000 上限滚动清除）。
		// {
		// 	static unsigned long lastRefillDirty = 0;
		// 	std::lock_guard<std::mutex> lkR(footMtx);
		// 	bool anyActive = false;
		// 	for (auto& fp : footprints) {
		// 		if (fp.depth > 0.0f) {
		// 			fp.depth = std::max(0.0f, fp.depth - kRefillPerFrame);
		// 			anyActive = true;
		// 		}
		// 	}
		// 	if (anyActive) {
		// 		footprints.erase(std::remove_if(footprints.begin(), footprints.end(),
		// 							 [](const Footprint& f) { return f.depth <= 0.0f; }),
		// 			footprints.end());
		// 		const unsigned long nowRD = GetTickCount();
		// 		if (nowRD - lastRefillDirty >= 250) {
		// 			lastRefillDirty = nowRD;
		// 			landFootDirty.store(true);
		// 		}
		// 	}
		// }
		// v197：无新脚印（且未重建）→ 跳过 13 万顶点全量遍历（帧率恢复）
		// v551 回退：rf 分离调度"没啥用"（用户实测）——恢复 rf 在 dirty 帧内限频
		// 150ms 执行（v550b 状态）。
		// v576（用户"直接改"，2026-08-27）：**去 dirty/RebuildField 双限频，盖章立即重建**——
		// 原 v564 100ms + v407 150ms 限频：盖章 13/s（v437b 实测）→ 新脚印延迟
		// 150ms 才出现 = "雪堆突然冒出/一闪一闪"（v575 实锤 fadeMark=0 非驱逐，纯延迟）。
		// 改盖章帧下一帧立即重建（延迟 1 帧 ≈16ms）→ 新坑紧跟脚出现。成本：重建
		// 频率 = 盖章频率（≈每 5 帧 1 次），land 平均 ~1-2ms（可接受）。
		// v609：删 fullDue/lastFullT（v567 恒 false 死逻辑——回填已禁用，场不每帧变）
		bool dirtyDue = landFootDirty.exchange(false);  // v603：不限频，dirty 帧立即重建
		if (!dirtyDue && !landRebuildPending.load())
			return;
		// v576：dirtyDue 帧立即重建（30ms 轻限频防盖章高频帧每帧重建）+ 延迟检测
		// v577：v576 全限频去除后 rf 涨到 12ms（fp 451 时）；门 48 盖章降到 ~7/s，
		// 30ms 轻限频不拦（间隔 140ms）但兜底盖章异常高频场景。
		// v603：**去掉 30ms 轻限频（用户"盖章延迟太大，要精确"）**——30ms 限频在
		// 200fps 下 = 6 帧延迟（盖章 → dirty → 被限频跳过 → 下帧再试），感知延迟
		// 明显。v596 后 fp 受控（玩家 200 + obj 80）+ v593 平滑限幅 → rf 3-6ms，
		// 盖章 42/s ÷ 200fps ≈ 每 4.8 帧重建 1 次（摊薄 ~1ms/帧）成本可接受 →
		// dirty 帧立即重建（延迟 = 盖章到下一帧 ≈ 5ms 即时）。
		if (dirtyDue) {
				const unsigned long setT = gDirtySetT.load(std::memory_order_relaxed);
				const unsigned long nowR = GetTickCount();
				if (setT != 0 && nowR >= setT) {
					const unsigned long delay = nowR - setT;
					gDelaySum.fetch_add(delay, std::memory_order_relaxed);
					gDelayN.fetch_add(1, std::memory_order_relaxed);
					unsigned long mx = gDelayMax.load(std::memory_order_relaxed);
					while (delay > mx && !gDelayMax.compare_exchange_weak(mx, delay, std::memory_order_relaxed)) {}
				}
				const auto tRf0 = clkLd::now();
				RebuildField();
				const auto tRf1 = clkLd::now();
				sLdRfUs += std::chrono::duration_cast<std::chrono::microseconds>(tRf1 - tRf0).count();
				sLdRfMaxUs = std::max(sLdRfMaxUs,
					std::chrono::duration_cast<std::chrono::microseconds>(tRf1 - tRf0).count());
			}
		auto& cells = landBuf[landBufIdx.load()];
		// v274：**盖章帧同步补建玩家周围 3×3**——用户建议"走上去一次性就平整"：
		// 裂缝 = 玩家脚下 cell 还是引擎网格（rebuild 分帧 0.2s 未完成 / 移动中引擎
		// 刚加载新 cell 未替换），反复来回走触发多次 rebuild 后才全 129² → 平。
		// 盖章帧（dirty）检查玩家 cell（ci=24）+ 8 邻域是否全 129²，缺 → AddTask
		// 游戏线程**同步 BuildCell 补建**（延迟 1-2 帧 = 人眼无感）→ 玩家脚下永远
		// 即时 129² → 裂缝在脚下立即消失，不需来回走。同步只补缺失 cell（通常
		// 0-4 个，15 万顶点插值 ≈ 几 ms），远处仍走分帧队列（v273 512 同步）。
		// v609：删 v434 同步补建空块（needSync 只更新 lastSyncTick 无实际动作——
		// Smooth Terrain 129² 直接改顶点，零替换）
		std::vector<const Footprint*> nearFp;
		int totalDeformed = 0;
		float totalDeepest = 0.0f;  // v214：最深下陷诊断（数据确认坑真实深度）
		// v141：分帧交错全量上传——每帧轮换上传 1/5 的 geom（100/5=20 个），5 帧轮完。
		// 目的：a) 抗引擎 LOD 刷新吃掉 17×17 象限的抬高（0.08s 内恢复 → 棋盘格消失）；
		// b) 避免 v138 每帧全量 6MB/100 次 UpdateSubresource 导致 GPU 队列积压 → TDR
		// 闪退/扯拉。有脚印的 geom（玩家附近）仍每帧实时上传（坑实时）。
		// v548：缓存后首帧全量上传标志（沙丘基线落 GPU）——重缓存后重置
		static bool firstFullUp = true;
		if (landRebuildPending.exchange(false))  // v567：exchange 一次性取走（消费后清，防永久强制全量）
			firstFullUp = true;
		const auto tVt0 = clkLd::now();  // v545：geom 顶点循环计时起点
		// v579（用户"走起来尖角三角、停下平滑、交替闪"，2026-08-27）：**ConeCS 去降频、
		// 每帧削坡**——原 v567 每 4 个 dirty 帧 ConeCS 1 次：顶点循环每 dirty 帧全量
		// 重算，coneDue 帧顶点被削坡（平滑）、非 coneDue 帧顶点=场原始值（坑/雪堆
		// 边缘陡 → 三角尖角）→ 走路盖章（每帧重建）时 3/4 帧尖角、1/4 帧平滑 = 交替
		// 闪；停下无重建 = 保持最后上传 → 平滑。每帧削坡后顶点恒平滑（不再交替）。
		// 成本：ConeCS 只在 dirty 帧跑（盖章 7/s → 每 ~9 帧 1 次），平均 ~1.6ms 可接受。
		const bool coneDue = true;
		for (int ci = 0; ci < 49; ci++) {
			for (int q = 0; q < 4; q++) {
				auto& lc = cells[ci][q];
				auto* raw = lc.raw;
				const auto vc = lc.verts;
				const auto stride = lc.stride;
				if (!raw || vc == 0 || lc.orig.size() < static_cast<std::size_t>(stride) * vc ||
					lc.work.size() < static_cast<std::size_t>(stride) * vc)
					continue;
				// v206：非雪 quad（泥地/岩石/草地/道路）不挖坑不沙丘——黑神话战壕只在雪地
				// v445：**材质分类扩展（用户"雪地/沙地/泥地有效，其他完全无效"）**——
				// surfaceClass 0=其他（岩石/草地/道路）→ 整个 cell 跳过（不遍历顶点/
				// 不采样场/不上传）；1=雪 2=沙 3=泥 → 正常变形。
				if (lc.surfaceClass == 0)
					continue;
				// v269：**cell 距离跳过（帧数修复）**——7×7 = 326 万顶点每 dirty 帧全量
				// 遍历 + 场采样 = 玩家移动（盖章频繁）时帧数暴跌/不稳定（用户实锤）。
				// 脚印最大影响范围 ≈ 2702 单位（队列 2600 剔除 + 坑/雪堆 ~102 影响半径），
				// cell 中心距玩家 > 4500 → cell 最近端（中心 - 半对角 1448）> 3052 单位
				// → 无脚印能影响该 cell → 变形恒 0 → 跳过（不遍历顶点/不采样场/不上传）。
				// 交界安全：被跳 cell 与处理 cell 的交界处变形都 ≈0 → 无裂缝（脚印到
				// 不了 3052 处）。dirty 帧实际处理 cell 从 49 → ~5-9 个 → 帧数恢复。
				{
					const float cdx = lc.centerX - playerPos.x;
					const float cdy = lc.centerY - playerPos.y;
					if (cdx * cdx + cdy * cdy > 4500.0f * 4500.0f)
						continue;
				}
				sLdGeoms++;  // v545：实际处理 geom 计数（诊断数据变化）
				const float tx = lc.worldT[0];
				const float ty = lc.worldT[1];
				// v382：脚印粗筛包锁（渲染线程 vs 游戏线程写）
				{
				std::lock_guard<std::mutex> lkF2(footMtx);
				nearFp.clear();
				const float hw = lc.halfDiag;
				for (const auto& fp : footprints) {
					if (std::abs(fp.x - lc.centerX) < hw && std::abs(fp.y - lc.centerY) < hw)
						nearFp.push_back(&fp);
				}
				}
				// v267 注释历史：曾删 nearFp 跳过（防"脚印在 A cell → B cell 跳过 → 边界
				// 裂缝"）。v549：**恢复静息 geom 跳过（vt 9-10ms 大头实锤——每盖章帧全量
				// 遍历 13 万顶点）**——nearFp 空 = 包围盒内无脚印 → 场值恒 0（脚印影响
				// 半径 R=56 << halfDiag 粗筛）→ work=orig 无变形 → 跳过变形/ConeCS/沙丘/
				// 法线/上传。v267 裂缝担忧不成立：近交界脚印必被相邻 geom 包围盒捕获
				//（都处理 → 场连续一致）；远处脚印场值 0 本就无变形。首帧（firstFullUp）
				// 不跳过（沙丘基线落 GPU）。玩家附近变形区 nearFp 非空照常处理 →
				// 无"消失又出现"。
				// v609：删 fullDue（恒 false 死逻辑）——条件等价于 !firstFullUp
				if (!firstFullUp && nearFp.empty())
					continue;
				// v132：网格间距感知的有效半径——17×17（289）间距 128、33×33（1089）
				// 间距 64、65×65（4225）间距 32；低分辨率网格的坑半径放大（≥2×2 顶点）
				// 才可见，否则坑落在 1 个顶点上完全看不出凹陷。
				// v190：高密度网格（255²=65025）间距 8.06
				// v147：写 work 副本（不写 raw——v146 5×5 大范围写 raw，边缘 cell geom 被
				// 引擎重建 → raw 悬空 → 闪退）。上传 work 副本到 vb。
				std::uint32_t deformed = 0;
				// v544：**删除 v265/v278 边缘 5 圈不变形（详见下方 v544 注释）**——
				// 历史：v265 曾让 129² 边缘 5 圈保持原高度防相邻 cell 裂缝（v278 恢复）；
				// v544 用户实锤边界深色线后删除，改场方案世界连续保证边界无缝。
				for (std::uint32_t vi = 0; vi < vc; vi++) {
					auto* pos = reinterpret_cast<float*>(
						lc.work.data() + static_cast<std::size_t>(vi) * stride);
					const float wx = pos[0] + tx;
					const float wy = pos[1] + ty;
				const float origZ = *reinterpret_cast<const float*>(
					lc.orig.data() + static_cast<std::size_t>(vi) * stride + 8);
				// v544k：**截断更细更自然（用户"截断是否可以更细一点，更自然一点"）**——
				// 5 圈（80 单位）硬截断（坑在边界前 80 单位突然截止，不自然）→ **3 圈
				// （48 单位）平滑淡出**：ring 0（最外圈）变形 0%（保持 origZ 平 → 相邻块
				// 交界一致无裂缝）；ring 1/2 变形量 smoothstep 渐增（0.26/0.74）；ring 3+
				// 全量变形。坑/雪堆在边界 48 单位内逐渐消失而非硬截止，过渡自然。
				float edgeFade = 1.0f;
				if (vc == highResDim * highResDim) {
					const int nn = static_cast<int>(highResDim);
					const int rr = static_cast<int>(vi) / nn;
					const int cc = static_cast<int>(vi) % nn;
					const int ring = std::min({ rr, cc, nn - 1 - rr, nn - 1 - cc });
					constexpr int kEdgeFade = 2;  // v544l：3→2（32 单位，用户"再细"）
					if (ring < kEdgeFade) {
						const float t = static_cast<float>(ring) / static_cast<float>(kEdgeFade);
						edgeFade = t * t * (3.0f - 2.0f * t);  // smoothstep：ring0=0 ring2=0.74
						if (edgeFade < 0.001f) {  // ring 0 完全平（边界交界无裂缝）
							pos[2] = origZ;
							continue;
						}
					}
				}
				// v581：**场框剔除（帧数优化，用户"继续检测帧数"）**——v580 影响框外
				// 场值恒 0（无坑无雪堆）→ 跳过场采样/obj 采样/合成，直接写基线：
				// origZ + 场景雪堆 + 静态沙丘（UndulationXY 世界锚定，值恒定）→
				// 顶点遍历量 ~76 万 → 框内 1-5 万。首帧（firstFullUp）全量不跳
				//（沙丘基线落 GPU）；无脚印（fieldBoxValid=false）不跳（走原路径）。
				// 沙丘在此写入后，下方沙丘循环跳过框外（每顶点恰一次沙丘，无双写）。
				if (fieldBoxValid && !firstFullUp) {
					const bool inBox = wx >= fieldBoxMinX && wx <= fieldBoxMaxX &&
									   wy >= fieldBoxMinY && wy <= fieldBoxMaxY;
					if (!inBox) {
						float lift2 = 0.0f;
						SampleSceneLift(wx, wy, lift2);
						const float rem2 = 24.0f + lift2;
						const float sc2 = std::clamp(rem2 / 8.0f, 0.0f, 1.0f);
						pos[2] = origZ + lift2 + UndulationXY(wx, wy) * sc2;
						continue;
					}
				}
				// v266：**变形场采样**——所有网格（129² + 引擎 289/65²）从同一张
				// 世界坐标变形场取 deform/ridge（用户方案"边界顶点链接一起变形"）：
				// 相邻 cell 边界顶点（同一世界位置）采样同一场 → 变形量必然一致 →
				// 无交界高低差 → 无裂缝；坑/雪堆可跨 cell 连续。
				float deform = 0.0f;
				float ridge = 0.0f;
				float sceneLift = 0.0f;
				SampleFieldNearest(wx, wy, deform, ridge);  // v564：最近邻（4 读→1 读，顶点对齐零误差）
				// v529：**场通道分离合成（用户"武器盖章覆盖脚下导致脚印变化，互不影响"）**——
				// 玩家脚印场优先：有玩家变形（deform>阈值）显示玩家脚印；无脚印区域
				// 用物体场（物品/拖痕/深坑）→ 脚印不被拖痕覆盖、拖痕独立呈现。
				{
					float deformObj = 0.0f, ridgeObj = 0.0f;
					SampleFieldObjNearest(wx, wy, deformObj, ridgeObj);  // v564
					// v530：**坑玩家优先 + 雪堆叠加（用户"拖拽没看到雪堆"实锤）**——
					// v529 玩家优先把 obj 场（拖痕/物品/深坑）的沟和雪堆整块遮掉：
					// 拖武器走路时刀尖就在玩家脚边 → deform>0.05 脚印区 → 拖痕全
					// 被吃。坑保持玩家场（脚印形状/深度不变 = 互不影响）；雪堆改
					// 加法叠加 → 拖痕/物品雪堆在脚印上也显示（用户当前痛点）。
					if (deform < 0.05f)
						deform = deformObj;
					ridge = ridge + ridgeObj;
				}
				SampleSceneLift(wx, wy, sceneLift);  // v435：场景雪堆（墙边/岩石边，低频刷新）
				// v435：**叠加合成（修复"非此即彼"吞雪堆）**——旧版坑内走 delta 分支、
				// ridge 完全丢失（用户"只有坑没有雪堆"），且 ridge 只有 3-5 高（坑深 42）
				// 几乎不可见。叠加后：坑内 delta 负（下陷）+ 坑沿 ridge 正（隆起）→
				// 连续雪路 = 沟壑 + 两侧雪堆；sceneLift 在墙脚/岩石边额外堆雪。
				// v427（0.5 分支）：恢复地形下陷 delta = -deform×kSnowDepth
				// （kSnowDepth=42，用户"雪堆深度稍微提高"）。v347 冻结已释放。
				// v446c：**沙/泥与雪效果一致（用户拍板）**——撤销 v446 的材质差异
				// factor（沙/泥 pure 雪堆不下陷），沙/泥 = 雪（凹陷+雪堆同幅度）。
				// 保留：材质分类（v445，其他材质 cell 跳过）+ 分类关键字（v446b）。
				// v450：**材质响应算法（用户"泥沙应该和雪不一样，要现实效果"）**——
				// 物理依据：雪松软塌陷（深坑+高堆+陡壁）；沙粒流动（浅坑+低堆+缓坡，
				// 踩过边缘流平）；湿泥粘稠可塑（中坑+中堆+中坡）。
				//   deformF 坑深比例 | ridgeF 雪堆比例 | slopeF 坡面陡度（ConeCS）
				// 雪：1.0（-18）   | 1.0（+12） | 1.0（45°）
				// 沙：0.4（-7）    | 0.25（+3） | 0.5（~27° 缓坡）
				// 泥：0.65（-12）  | 0.65（+8） | 0.75（~37° 中坡）
				float deformF = 1.0f, ridgeF = 1.0f;
				if (lc.surfaceClass == 2) {       // 沙
					deformF = 0.4f;
					ridgeF = 0.25f;
				} else if (lc.surfaceClass == 3) {  // 泥
					deformF = 0.65f;
					ridgeF = 0.65f;
				}
				const float delta = -deform * kSnowDepth * deformF + ridge * ridgeF + sceneLift;
				// v544k：淡出区变形量乘 edgeFade（smoothstep 渐增 → 过渡自然）
				const float deltaF = delta * edgeFade;
				if (std::abs(deltaF) > 0.01f) {
					pos[2] = origZ + kSnowLayer + deltaF;
					deformed++;
					if (deltaF < totalDeepest)
						totalDeepest = std::min(totalDeepest, deltaF);  // v214：跟踪最深下陷
				} else {
					pos[2] = origZ + kSnowLayer;
				}
				}
				// v205：ConeCS 坡度限制（Josef HeightMapProcessCS 移植）——8 邻域多尺度
				// min 变换：任何顶点不得比邻点高出 SlopePerUnit×距离（1.0=45° 休止角）。
				// 只限制凸起（雪堆/沙丘），凹陷（坑）不受影响；多尺度 step 1,2,4 传播
				// 大范围缓坡；最后 max(原地形) 防穿透。只对有脚印的高密 geom 做。
				// v438h：**修复 ConeCS 填坑（SINK=1.7 vs deepest=-93 实锤）**——旧
				// `*p = std::max(h, origZC)` 把**下陷顶点也拉回原地形**（坑内邻域全
				// 下陷 → h<orig → max=orig → 每帧合成写入的坑被填平，只剩沙丘残余
				// 1.7）。正确：**凸起（h>orig）用坡度受限值，凹陷（h≤orig）保留合成
				// 下陷**（max(orig) 的防穿透语义只对凸起雪堆有意义）。
				// v547：回退 v534m——ConeCS 全量（无增量跳过）
				// v579：ConeCS 每帧削坡（原 v564 降频 1/4 导致 3/4 帧未削 = 尖角/平滑交替闪，
				// 用户实锤"走尖角停平滑"）——每 dirty 帧所有高密 geom 全量削坡。
				if (coneDue && !nearFp.empty() && vc == highResDim * highResDim) {
					const auto n = static_cast<int>(highResDim);
					const float spacing = 2048.0f / static_cast<float>(n - 1);
					// v450：**坡面陡度按材质**——雪 1.0（45° 陡壁）；沙 0.5（~27°
					// 缓坡，沙粒流动踩过边缘流平）；泥 0.75（~37° 中坡，粘滞）。
					// v586：**回退 v584/v585 圆滑调整（用户"回退到没有调整圆滑的效果，
					// 我感觉差不多"）**——v584（1.0→0.8）与 v585（0.8→0.4）削坡坡度
					// 调整用户实测观感无差异 → 恢复原始 Josef 参数：雪 1.0（45°）、
					// 沙 0.5、泥 0.75。ConeCS 削坡回到原力度。
					float kSlopePerUnit = 1.0f;  // 1.0 = 45°（Josef SnowMoundSteepness）
					if (lc.surfaceClass == 2)
						kSlopePerUnit = 0.5f;
					else if (lc.surfaceClass == 3)
						kSlopePerUnit = 0.75f;
					const int kConeSteps[3] = { 1, 2, 4 };
					auto coneZAt = [&](int r, int c) -> float {
						return *reinterpret_cast<const float*>(
							lc.work.data() + static_cast<std::size_t>(r * n + c) * stride + 8);
					};
					// v581：**ConeCS 写范围裁剪到场框（帧数优化）**——框外无坑无雪堆
					// → 削坡恒无变化（h==zCur==基线）→ 跳过写。邻域读（coneStep≤4 →
					// ±16 格）自动扩展到框外（coneZAt 越界 clamp 已处理），削坡结果
					// 不受裁剪影响。顶点 (r,c) 世界坐标 = (tx + c*spacing, ty + r*spacing)
					// → r = (wy - ty)/spacing，c = (wx - tx)/spacing。框不相交 → 范围
					// 倒置 → 循环不执行。
					int rStart = 0, rEnd = n - 1, cStart = 0, cEnd = n - 1;
					if (fieldBoxValid) {
						rStart = std::max(0, static_cast<int>(std::ceil((fieldBoxMinY - ty) / spacing)));
						rEnd = std::min(n - 1, static_cast<int>(std::floor((fieldBoxMaxY - ty) / spacing)));
						cStart = std::max(0, static_cast<int>(std::ceil((fieldBoxMinX - tx) / spacing)));
						cEnd = std::min(n - 1, static_cast<int>(std::floor((fieldBoxMaxX - tx) / spacing)));
					}
					for (int coneStep : kConeSteps) {
						for (int r = rStart; r <= rEnd; r++) {
							for (int c = cStart; c <= cEnd; c++) {
								auto* p = reinterpret_cast<float*>(
									lc.work.data() + static_cast<std::size_t>(r * n + c) * stride + 8);
								const float zCur = *p;  // 合成后值（下陷或隆起）
								float h = zCur;
								for (int dr = -1; dr <= 1; dr++) {
									for (int dc = -1; dc <= 1; dc++) {
										if (dr == 0 && dc == 0)
											continue;
										const int rr = r + dr * coneStep;
										const int cc = c + dc * coneStep;
										if (rr < 0 || rr >= n || cc < 0 || cc >= n)
											continue;
										const float dist = std::sqrt(static_cast<float>(dr * dr + dc * dc)) *
											static_cast<float>(coneStep) * spacing;
										h = std::min(h, coneZAt(rr, cc) + kSlopePerUnit * dist);
									}
								}
								const float origZC = *reinterpret_cast<const float*>(
									lc.orig.data() + static_cast<std::size_t>(r * n + c) * stride + 8);
								// v438h：凸起受限、凹陷保持（不再 max 拉回）
								*p = (h > origZC) ? h : zCur;
							}
						}
					}
				}
				// v205：沙丘起伏——所有重算顶点叠加（世界锚定，深度缩放）。雪面剩深
				// ≈ 24 单位（snow01 30 类近似），scale=saturate(剩深/8)：坑底（剩深 0）
				// 无起伏，雪面/浅坑自然起伏。随后法线重算会包含沙丘梯度（Josef 同款）。
				// v547：回退 v534m——沙丘全量（无增量跳过）
				for (std::uint32_t vi2 = 0; vi2 < vc; vi2++) {
					auto* pos2 = reinterpret_cast<float*>(
						lc.work.data() + static_cast<std::size_t>(vi2) * stride);
					const float wx2 = pos2[0] + tx;
					const float wy2 = pos2[1] + ty;
				const float origZ2 = *reinterpret_cast<const float*>(
					lc.orig.data() + static_cast<std::size_t>(vi2) * stride + 8);
					// v544k：**沙丘边缘跳过 5 → 3 圈（与主变形淡出匹配）**——淡出区
					// 几何变形小，沙丘也跳过（边缘 3 圈无沙丘 → 交界平一致）。
					if (vc == highResDim * highResDim) {
						const int nn2 = static_cast<int>(highResDim);
						const int rr2 = static_cast<int>(vi2) / nn2;
						const int cc2 = static_cast<int>(vi2) % nn2;
						constexpr int kEdgeSkip2 = 2;  // v544l：3→2 与淡出匹配
						if (rr2 < kEdgeSkip2 || rr2 >= nn2 - kEdgeSkip2 ||
							cc2 < kEdgeSkip2 || cc2 >= nn2 - kEdgeSkip2)
							continue;
					}
					// v581：**沙丘框剔除（帧数优化）**——框外顶点主循环已写基线
					//（origZ + sceneLift + 沙丘，UndulationXY 世界锚定值恒定）→ 沙丘
					// 循环跳过框外，避免重复计算（框内顶点主循环不含沙丘 → 每顶点
					// 恰一次沙丘，无双写）。首帧（firstFullUp）全量不跳（基线落 GPU）。
					if (fieldBoxValid && !firstFullUp) {
						if (wx2 < fieldBoxMinX || wx2 > fieldBoxMaxX ||
							wy2 < fieldBoxMinY || wy2 > fieldBoxMaxY)
							continue;
					}
					const float remaining = 24.0f + (pos2[2] - origZ2);
					const float scale = std::clamp(remaining / 8.0f, 0.0f, 1.0f);
					pos2[2] = pos2[2] + UndulationXY(wx2, wy2) * scale;
				}
				// v179：法线同步——SmoothTerrain 网格 @20 有压缩法线（引擎读！），我们改 z
				//后法线不更新 → 坑壁光照错乱 → 三角黑色暗部。从变形后邻域高度差中心差分
				//重算法线，编码写入 work @20（3B：b=(n+1)*127 clamp 0..255）。只对有
				//变形的 geom 做（玩家附近）。n 由顶点数开方（181×181=32761 或 129²=16641）。
				// v202：**限幅 0.2 → 0.9**——v183 限幅是为防 Catmull-Rom 尖塔（v201 前
				// SmoothTerrain 细分网格上凸起会过冲）。**v201 后 181² 网格是我们自己生成
				// 的（heights 双线性），不再经过 Catmull-Rom → 尖塔风险消除 → 放宽限幅
				// 让战壕壁有真实立体光照（黑神话战壕好看的核心 = 壁面明暗）**。
				// v206：法线重算条件修复——原 `deformed > 0` 漏掉无脚印轮询 geom（v205 Undulation
				// 沙丘改了高度但法线未更新 → 阴影 pass slope-bias 用旧法线 → "变形后奇怪三角
				// 阴影/条带"（用户反馈）。改为所有重算的高密 geom 一律重算法线（含沙丘梯度）。
				// v279：恢复法线重算（v244 镜像边界版）——v272 二分验证禁用法线后裂缝
				// 仍在（裂缝已由 v265 边缘 5 圈解决）→ 法线不是裂缝源 → 恢复。坑壁立体
				// 光照（沟壑感核心）需要。镜像边界：坡度方向保留，边界法线不水平。
				// v544i：**法线重算跳过边缘 5 圈（用户"是否可以跳过边缘"实锤方案）**——
				// v544g 二分定案：深色线 = 法线重算把块间边界法线替换成**块内差分值**
				// （每块独立差分 → 交界处法线方向不连续 → 光照突变）。边缘 5 圈（与
				// v265 截断一致：几何不变 pos[2]=origZ）**跳过不重算** → 保留引擎原始
				// 法线（引擎烘焙法线在相邻块边界天然连续）→ 交界深色线消失。内部顶点
				// 正常重算（坑壁立体光照保留 = 沟壑感核心）。v544f 去增强 + 无软饱和
				// 保持（真实几何坡度差分）。
				constexpr bool kEnableNormalRecalc = true;
				// v548：**法线只对变形 geom（用户"都改了但不消失又出现"；v545-perf 实锤
				// vt=12ms 大头 = 全量 13 万顶点差分）**——静息 geom（deformed==0）法线
				// 保持首次全量计算值（work 保留，与 GPU 一致），只重算有变形顶点的
				// geom（坑壁立体光照不受影响）。沙丘静态（UndulationXY 世界锚定）→
				// 首次全量已含梯度，无需每帧重算。全量重算保留 → work 永远正确 →
				// 无"消失又出现"。
				if (kEnableNormalRecalc && vc > 1024 && deformed > 0) {
					const auto n = static_cast<int>(std::sqrt(static_cast<double>(vc)) + 0.5);
					if (n * n == static_cast<int>(vc)) {
						const float sp2 = 2.0f * (2048.0f / static_cast<float>(n - 1));
						// v609：**stride 防御门**——法线写 work@20（3B）要求 stride≥23，
						// 否则越界写坏下一顶点。引擎 LANDSCAPE stride=40 正常，防御引擎
						// 变体网格（如 LOD 对象 stride 不同）。
						if (stride < 23) {
						// v562c：**取消 v562b 两遍平滑（用户"不行，取消这次圆润化"）**——
						// 恢复 v562 前单遍差分法线（v544j 增强 ×1.35 + v444 软饱和 +
						// v544c 单侧差分边界 + v544i 边缘 2 圈保留引擎法线）。
						// v583：**回退 v582 法线框裁剪（用户 23:46 要求回退）**——恢复全量
						// n² 法线重算（vt 恢复 2.2-2.9ms，法线全量成本仍在）。v582 裁剪导致
						// 问题（用户实测不满意，原因未明）。若后续再优化法线需换思路
						//（如变形区增量重算，勿用框裁剪）。
						// v611：法线平滑缓冲（差分法线先存 float，高斯平滑后压缩写回 @20）——
						// 用户 2026-08-30"凹坑法线/阴影更柔和细腻"：差分法线逐顶点跳变 →
						// 坑壁阴影生硬；3×3 [1,2,1]² 高斯平均后法线连续过渡 → 阴影柔和细腻。
						std::vector<float> normBuf(static_cast<std::size_t>(n) * n * 3, 0.0f);
						for (int r = 0; r < n; r++) {
							for (int c = 0; c < n; c++) {
								constexpr int kNormEdgeSkip = 2;  // v544l：3→2 与淡出匹配
								if (r < kNormEdgeSkip || r >= n - kNormEdgeSkip ||
									c < kNormEdgeSkip || c >= n - kNormEdgeSkip)
									continue;
								auto* p = reinterpret_cast<float*>(
									lc.work.data() + static_cast<std::size_t>(r * n + c) * stride);
								auto zAt = [&](int rr, int cc) -> float {
									return *reinterpret_cast<const float*>(
										lc.work.data() + static_cast<std::size_t>(rr * n + cc) * stride + 8);
								};
								// v544c：边界单侧差分（右边界 c-1/c-2，左边界 c+1/c+2）——
								// 交界法线与内部连续（深色线修复）
								const float zE = (c + 1 < n) ? zAt(r, c + 1) : zAt(r, c - 2);
								const float zW = (c > 0) ? zAt(r, c - 1) : zAt(r, c + 2);
								const float zN = (r + 1 < n) ? zAt(r + 1, c) : zAt(r - 2, c);
								const float zS = (r > 0) ? zAt(r - 1, c) : zAt(r + 2, c);
								// v544j：动态法线增强（差分×1.35 + 软饱和——视觉深坑）
								// v562d/e：**增强 1.35→1.25→1.15（用户"法线圆润只加一点点"逐次微调）**——
								// 坡度缓 15%，光照柔和，坑壁立体感保留
								// v610：1.15→1.3（用户 2026-08-30"走路沟壑很假"）——1.15 太缓
								// 光照平、沟壑立体感不足；1.3 介于原 1.35 与现 1.15 之间（网格
								// 181² 变细后法线差分更准，增强可适度回提）。用户可再微调。
								const float kNormScale = 1.3f;
								const float rawX = ((zW - zE) / sp2) * kNormScale;
								const float rawY = ((zS - zN) / sp2) * kNormScale;
								float nx = rawX / (1.0f + std::fabs(rawX));
								float ny = rawY / (1.0f + std::fabs(rawY));
								float nz = 1.0f;
								const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
								if (len > 1.0e-6f) {
									nx /= len;
									ny /= len;
									nz /= len;
								}
								// v611：先存 float 法线到 normBuf（平滑 pass 用），不直接压缩写回
								auto* nb = normBuf.data() + (static_cast<std::size_t>(r) * n + c) * 3;
								nb[0] = nx;
								nb[1] = ny;
								nb[2] = nz;
							}
						}
						// v611：法线高斯平滑 + 压缩写回（边缘 2 圈不参与平滑，保留差分结果）
						for (int sr = 1; sr < n - 1; sr++) {
							for (int sc = 1; sc < n - 1; sc++) {
								float sx = 0.0f, sy = 0.0f, sz = 0.0f;
								for (int dr = -1; dr <= 1; dr++) {
									for (int dc = -1; dc <= 1; dc++) {
										const float w = (dr == 0 ? 2.0f : 1.0f) * (dc == 0 ? 2.0f : 1.0f);
										const auto* sn = normBuf.data() +
											(static_cast<std::size_t>(sr + dr) * n + (sc + dc)) * 3;
										sx += sn[0] * w;
										sy += sn[1] * w;
										sz += sn[2] * w;
									}
								}
								const float inv16 = 1.0f / 16.0f;
								float nx = sx * inv16, ny = sy * inv16, nz = sz * inv16;
								const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
								if (len > 1.0e-6f) {
									nx /= len;
									ny /= len;
									nz /= len;
								} else {
									nx = 0.0f;
									ny = 0.0f;
									nz = 1.0f;
								}
								auto* sp = reinterpret_cast<float*>(
									lc.work.data() + static_cast<std::size_t>(sr * n + sc) * stride);
								auto* nbo = reinterpret_cast<std::uint8_t*>(sp) + 20;
								nbo[0] = static_cast<std::uint8_t>(std::clamp((nx + 1.0f) * 127.0f, 0.0f, 255.0f));
								nbo[1] = static_cast<std::uint8_t>(std::clamp((ny + 1.0f) * 127.0f, 0.0f, 255.0f));
								nbo[2] = static_cast<std::uint8_t>(std::clamp((nz + 1.0f) * 127.0f, 0.0f, 255.0f));
							}
						}
						}
					}
				}
				// v164：回滚 v162 法线重算——@16 区域可能不是 NORMAL 而是 TANGENT/
				// 其他属性（v159 dump 显示垃圾值 + 用户实测"完全不对劲"）→ 写入破坏
				// normal map 切线空间光照。法线重算需先确认 vb 真实布局（vertexDesc）
				// 或改 normals[4][289] 数据源（小步验证），再单独上。
				// v154：SEH 保护上传——geom 可能被引擎 LOD 重建（悬空）导致崩。
				// SafeGeomValid 读 vertexCount 对比缓存（不一致=重建→失效跳过）；
				// SafeUpload 保护 UpdateSubresource（崩溃日志实锤 nvwgf2umx rep movsb）
				std::uint32_t curVc = 0;
				if (!lc.geom || !SafeGeomValid(lc.geom, vc, curVc)) {
					lc.geom = nullptr;  // 引擎重建 → 本缓存失效（下轮重缓存补）
					continue;
				}
				// v403：显式 null 检查（不依赖 SEH——/EHsc 下 __try 不生效的兜底）
				auto* rd = lc.geom ? lc.geom->GetGeometryRuntimeData().rendererData : nullptr;
				auto* vb = rd ? *reinterpret_cast<ID3D11Buffer**>(
					reinterpret_cast<std::uintptr_t>(rd) + 0x00) : nullptr;
				// v548：**上传只传曾变形的 geom（用户"都改了"；v545-perf 实锤 upKB=250MB/s）**——
				// 静息 geom（raised==false 且本帧未变形）work=orig 与 GPU 一致 → 不传
				// 省带宽；变形区（raised 历史标记 || 本帧 deformed>0）每帧传 → 引擎
				// LOD 刷新 ≤1 帧恢复 → **无"消失又出现"**（区别于 v534m 的 box 快照）。
				// 缓存后**首帧全量上传**（firstFullUp）：沙丘/静息基线落 GPU（沙丘是
				// 静态的，deformed==0 但 work 含 ±3.5 起伏 → 必须首帧传一次）。
				if (vb && (firstFullUp || lc.raised || deformed > 0)) {
					const auto tUp0 = clkLd::now();  // v545：上传计时
					SafeUpload(a_ctx, vb, lc.work.data(),
						static_cast<std::uint32_t>(stride * vc));
					const auto tUp1 = clkLd::now();
					const long long upUs = std::chrono::duration_cast<std::chrono::microseconds>(tUp1 - tUp0).count();
					sLdUpUs += upUs;
					sLdUpMaxUs = std::max(sLdUpMaxUs, upUs);
					sLdUpKB += static_cast<int>(stride * vc / 1024);  // v545：上传 KB
					lc.raised = true;
				}
				sLdDeform += static_cast<int>(deformed);  // v545：变形顶点累计
				totalDeformed += deformed;  // v133 诊断依赖（保留）
			}
		}
		// v545：land 内部阶段 2s 汇总（定位掉帧来源：rf=场重建 vt=顶点循环 up=上传）
		{
			const auto tVt1 = clkLd::now();
			sLdVtUs += std::chrono::duration_cast<std::chrono::microseconds>(tVt1 - tVt0).count();
			sLdVtMaxUs = std::max(sLdVtMaxUs,
				std::chrono::duration_cast<std::chrono::microseconds>(tVt1 - tVt0).count());
			sLdFrames++;
			sLdFp = footprints.size();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(clkLd::now() - sLdT0).count() >= 2000) {
				const double rfA = static_cast<double>(sLdRfUs) / 1000.0 / sLdFrames;
				const double vtA = static_cast<double>(sLdVtUs) / 1000.0 / sLdFrames;
				const double upA = static_cast<double>(sLdUpUs) / 1000.0 / sLdFrames;
				SKSE::log::info(
					"v545-perf: rf={:.2f}ms(max {:.1f}) vt={:.2f}ms(max {:.1f}) up={:.2f}ms(max {:.1f}) | geoms={} deform={} upKB={} fp={}",
					rfA, static_cast<double>(sLdRfMaxUs) / 1000.0,
					vtA, static_cast<double>(sLdVtMaxUs) / 1000.0,
					upA, static_cast<double>(sLdUpMaxUs) / 1000.0,
					sLdGeoms, sLdDeform, sLdUpKB, sLdFp);
				// v576：盖章→重建延迟（预期 avg≈16ms；>100ms 说明限频残留）
				SKSE::log::info("v576-dbg: rebuild={}/2s delayAvg={:.0f}ms delayMax={}ms",
					gDelayN.load(std::memory_order_relaxed),
					gDelayN.load(std::memory_order_relaxed) > 0 ?
						static_cast<double>(gDelaySum.load(std::memory_order_relaxed)) / gDelayN.load(std::memory_order_relaxed) : 0.0,
					gDelayMax.load(std::memory_order_relaxed));
					gDelaySum.store(0, std::memory_order_relaxed);
					gDelayN.store(0, std::memory_order_relaxed);
					gDelayMax.store(0, std::memory_order_relaxed);
					// v604：**盖章同步精准检测**——按类型盖章数（0 玩家 1 NPC 2 马
					// 3 狼/其他）+ 盖章→场重建延迟（maxTms 路径，v603 去限频后
					// 预期 ≈1 帧 5-16ms；>50ms = 盖章路径有延迟偏差）
					SKSE::log::info("v604-dbg: stmp(p={} n={} h={} w={}) tmsDelayAvg={:.0f}ms tmsMax={}ms",
						gStmpType[0].load(std::memory_order_relaxed),
						gStmpType[1].load(std::memory_order_relaxed),
						gStmpType[2].load(std::memory_order_relaxed),
						gStmpType[3].load(std::memory_order_relaxed),
						gDelay2N.load(std::memory_order_relaxed) > 0 ?
							static_cast<double>(gDelay2Sum.load(std::memory_order_relaxed)) / gDelay2N.load(std::memory_order_relaxed) : 0.0,
						gDelay2Max.load(std::memory_order_relaxed));
					for (auto& gs : gStmpType)
						gs.store(0, std::memory_order_relaxed);
					gDelay2Sum.store(0, std::memory_order_relaxed);
					gDelay2N.store(0, std::memory_order_relaxed);
					gDelay2Max.store(0, std::memory_order_relaxed);
				sLdRfUs = sLdVtUs = sLdUpUs = 0;
				sLdRfMaxUs = sLdVtMaxUs = sLdUpMaxUs = 0;
				sLdFrames = sLdGeoms = sLdDeform = sLdUpKB = 0;
				sLdT0 = clkLd::now();
			}
			if (firstFullUp)
				firstFullUp = false;  // 首帧全量上传完成
		}
		// v133：周期诊断——首帧 + 每 3 秒报告变形量（确认 17×17/65×65 各区域都生效）
		const auto now2 = GetTickCount();
		if (totalDeformed > 0 && (!landUploaded || now2 - landLastDiag > 3000)) {
			landUploaded = true;
			landLastDiag = now2;
			// v404：**变形/盖章检测**——场峰值 + 盖章速率（判断依据：
			// fieldMax ~1.0 = 坑深到位（1.5 depth clamp 1）；fieldMax <0.3 = 坑太浅
			// 或回填过快；stampRate >0 = 盖章活跃；deformVerts 增长 = 几何在动）
			{
				float fMax = 0.0f;
				const auto& df = deformField;
				for (float fv : df)
					if (fv > fMax) fMax = fv;
				for (float fv : deformFieldObj)  // v529：obj 场也统计
					if (fv > fMax) fMax = fv;
				// v435：ridge 场峰值（坑沿雪堆强度）+ 场景雪堆峰值诊断
				float rMax = 0.0f;
				for (float rv : ridgeField)
					if (rv > rMax) rMax = rv;
				for (float rv : ridgeFieldObj)  // v529
					if (rv > rMax) rMax = rv;
				const auto& slb = sceneLiftBuf[sceneLiftIdx.load()];
				float sMax = 0.0f;
				for (float sv : slb.field)
					if (sv > sMax) sMax = sv;
				static std::size_t lastStampCount = 0;
				static unsigned long lastStampTick = 0;
				const unsigned long nowS = GetTickCount();
				// v407：rate 下溢修复（size_t 无符号相减 → 巨大数 bug）
				const long long stampDelta = static_cast<long long>(footprints.size()) -
					static_cast<long long>(lastStampCount);
				const float rate = (nowS > lastStampTick && stampDelta >= 0) ?
					static_cast<float>(stampDelta) * 1000.0f /
						static_cast<float>(nowS - lastStampTick) : 0.0f;
				lastStampCount = footprints.size();
				lastStampTick = nowS;
				// v480g：**测最近盖章（物品滚远后 = 干净）**——firstStamp 是落地瞬间
				// = 玩家脚下，被玩家脚印(40宽)+雪堆环(2.6rC)覆盖 → minHalf=60 假象。
				// gObjLast 是物品当前位置——踢远后远离玩家脚印，环形 min = 真宽度。
				// 附加条件：测点距玩家 >200 单位才有效（远离玩家脚印区）
				{
					const float ox = gObjLastX.load(std::memory_order_relaxed);
					const float oy = gObjLastY.load(std::memory_order_relaxed);
					const auto pChar = RE::PlayerCharacter::GetSingleton();
					const float pdx = pChar ? ox - pChar->GetPosition().x : 0.0f;
					const float pdy = pChar ? oy - pChar->GetPosition().y : 0.0f;
					const float pDist = std::sqrt(pdx * pdx + pdy * pdy);
					if (gObjStamps.load(std::memory_order_relaxed) > 0 && pDist > 200.0f) {
						const auto fvAt = [&](float wx, float wy) -> float {
							const int gx = static_cast<int>(std::floor((wx - fieldOriginX) / kFieldStep + 0.5f));
							const int gy = static_cast<int>(std::floor((wy - fieldOriginY) / kFieldStep + 0.5f));
							if (gx < 0 || gx >= kFieldDim || gy < 0 || gy >= kFieldDim)
								return 0.0f;
							return deformFieldObj[static_cast<std::size_t>(gy) * kFieldDim + gx];  // v529：物品在 obj 场
						};
						const float peak = fvAt(ox, oy);
						// v480h：**场剖面明细**——沿 +x 方向每 8 单位 deform 值
						//（看 56 处到底是不是物品变形：objR=21.57 时 24 后应变 0）
						std::string prof;
						for (float dd = 0.0f; dd <= 56.0f; dd += 8.0f)
							prof += fmt::format(" {:.2f}", fvAt(ox + dd, oy));
						float minHalf = 0.0f;
						for (int a = 0; a < 8; a++) {
							const float ang = static_cast<float>(a) * 3.14159265f / 4.0f;
							const float cdx = std::cos(ang), cdy = std::sin(ang);
							float hw = 0.0f;
							for (float dd = 4.0f; dd <= 64.0f; dd += 4.0f) {
								if (fvAt(ox + cdx * dd, oy + cdy * dd) > 0.3f * peak)
									hw = dd;
								else
									break;
							}
							if (a == 0 || hw < minHalf)
								minHalf = hw;
						}
					SKSE::log::info("v480c-dbg: lastStamp at=({:.0f},{:.0f}) pDist={:.0f} peak={:.2f} minHalf={:.1f} +x:[{}] (expect ~21)",
						ox, oy, pDist, peak, minHalf, prof);
				}
			}
			SKSE::log::info("v404-def: stamps={} rate={:.1f}/s deformVerts={} deepest={:.1f} fieldMax={:.3f} ridgeMax={:.1f} sceneLiftMax={:.1f}",
				footprints.size(), rate, totalDeformed, totalDeepest, fMax, rMax, sMax);
			}
			SKSE::log::info("v133: deform active ({} verts, {} stamps, deepest={:.1f}, player=({:.0f},{:.0f}))",
				totalDeformed, footprints.size(), totalDeepest, playerPos.x, playerPos.y);
					// v433-dbg：**raw/vb 一致性验证（用户"没效果"决定性诊断）**——读引擎
					// 渲染实际用的 raw（rd+0x20）首顶点 z 与我们的 work 对比：raw 变了 =
					// 引擎渲染用 raw → 上传生效（问题在视觉/剔除/材质）；raw 没变 = 我们
					// 改的 work 从未被引擎采用 → 上传目标错（rd+0x00 不是渲染 vb）。
					// v438g：**修复"找最深"逻辑（SINK=0 假象实锤）**——旧版找 work z 最小
					// 顶点：129² 局部 z 值域 ±350（地形起伏），地形本身低点（orig=-31.7）
					// 永远比挖的坑（orig 320→work 232）小 → 恒找到地形低点 → SINK 恒 0。
					// 正确：找 **origZ - workZ（下陷量）最大** 的顶点（真正被挖的坑）。
					{
						// v510b：**遍历玩家 cell 全部 4 quadrant（v433 只查 q0——武器坑在
						// 玩家当前 quadrant，可能是 q3 → SINK=1.7 是 q0 的脚印不是武器坑）**。
						// 每个 quad 打印最深下陷（orig-work）顶点 + work/raw z——确认武器坑
						// 顶点真的变了（SINK≈40）+ raw 是否同步（raw≈work=上传生效，raw=orig=没传）。
						for (int qd = 0; qd < 4; qd++) {
							auto& cg0 = cells[24][qd];
							if (!cg0.geom || !cg0.raw || cg0.work.size() < 40 || cg0.verts <= 10)
								continue;
							// v569：SafeGeomValid——诊断块在主循环可能被跳过（nearFp 空/
							// 材质/距离）→ geom 未被置 null → 引擎 LOD 重建后悬空 → UAF 崩
							std::uint32_t curVc0 = 0;
							if (!SafeGeomValid(cg0.geom, cg0.verts, curVc0))
								continue;
							const auto* rd0 = cg0.geom->GetGeometryRuntimeData().rendererData;
							auto* rawP = rd0 ? *reinterpret_cast<std::uint8_t**>(
								reinterpret_cast<std::uintptr_t>(rd0) + 0x20) : nullptr;
							const auto wStride = cg0.stride ? cg0.stride : 40u;
							float maxSink = -1.0e6f;
							std::size_t sinkIdx = 0;
							const bool origOK = cg0.orig.size() >= static_cast<std::size_t>(wStride) * cg0.verts;
							for (std::uint32_t vi = 0; vi < cg0.verts && vi < 6000; vi++) {
								const float wz = *reinterpret_cast<const float*>(
									cg0.work.data() + static_cast<std::size_t>(vi) * wStride + 8);
								const float oz = origOK ? *reinterpret_cast<const float*>(
									cg0.orig.data() + static_cast<std::size_t>(vi) * wStride + 8) : wz;
								const float sink = oz - wz;
								if (sink > maxSink) { maxSink = sink; sinkIdx = vi; }
							}
							const float rawSinkZ = rawP ? *reinterpret_cast<const float*>(
								rawP + sinkIdx * wStride + 8) : -99999.0f;
							const float origSinkZ = origOK ? *reinterpret_cast<const float*>(
								cg0.orig.data() + sinkIdx * wStride + 8) : -99999.0f;
							const float workSinkZ = *reinterpret_cast<const float*>(
								cg0.work.data() + sinkIdx * wStride + 8);
						SKSE::log::info("v510b-dbg: q{} verts={} SINK={:.1f}@{} orig={:.1f} work={:.1f} raw={:.1f} cell=({:.0f},{:.0f}) center=({:.0f},{:.0f}) player=({:.0f},{:.0f})",
							qd, cg0.verts, maxSink, sinkIdx, origSinkZ, workSinkZ, rawSinkZ,
							cg0.worldT[0], cg0.worldT[1], cg0.centerX, cg0.centerY, playerPos.x, playerPos.y);
						}
					}
			// v233：玩家 cell（ci=24，7×7 中心 (0+3)*7+(0+3)）缓存密度诊断——v140 显示
			// llll（17²）但 v151 显示 mesh.child=16641（129²）：怀疑 FindLandscape 缓存
			// lc.geom = 引擎重建的 17² 对象（≠我们替换的 129² 渲染对象）→ 变形上传到
			// 不在渲染的 vb → 凹陷不可见。v270：索引 12→24（v258 改 7×7 后中心=24）。
			for (int qd = 0; qd < 4; qd++) {
				auto& qc = cells[24][qd];
				SKSE::log::info("v233: player cell q{} cachedVerts={} geom={} raised={}",
					qd, qc.verts, static_cast<void*>(qc.geom), qc.raised);
			}
			// v276：**静息贴地诊断**——v275 边缘淡出后"还是有"裂缝 → 怀疑不是"变形切"
			// 而是"静息高度差"（无坑时 129² 与引擎网格高度基准差）。取 5×5 边缘 cell
			// cell(2,0) 的 129² 东边界顶点：世界高度（work[2]+引擎节点 worldT[2]）vs
			// GetLandHeight（引擎权威地形高度）——差≈0 → 129² 贴地 → 裂缝在渲染层；
			// 差大 → 129² 没贴地（orig 源滞后/LOD 网格偏差）→ 修 BuildCell 高度源。
			{
				auto* tes = RE::TES::GetSingleton();
				const int ciE = (0 + 3) * 7 + (2 + 3);  // cell(2,0) 5×5 东边缘
				auto& lcE = cells[ciE][1];              // q1（东侧 quadrant）
				if (tes && lcE.verts == highResDim * highResDim &&
					lcE.work.size() >= static_cast<std::size_t>(lcE.stride) * lcE.verts) {
					// v565：wx/wy/wz/p 诊断变量已删（C4189——v315 已禁用此诊断）
				}
			}
			// v239：边界高度诊断——玩家 cell（ci=24）东边界（q1/q3 的 c=n-1 列）与东邻
			// cell（ci=25）西边界（q0/q2 的 c=0 列）**同一世界位置**（x=cell 中心+2048）的
			// orig（构建高度）/work（变形后高度）对比。判定裂缝根因：
			//   dOrig 大 → 构建高度不连续（129² 边界顶点高度源不一致）
			//   dOrig≈0 但 dWork 大 → 变形后不连续（变形量/基准问题）
			//   dWork≈0 → 高度连续 → 裂缝在渲染层（UV/法线/剔除）
			// v270：索引 12/13→24/25（v258 改 7×7 后玩家 cell=24、东邻=25——旧索引读到
			// 角落 cell 且 verts=0 → 诊断一直没输出）
			for (int h = 0; h < 2; h++) {
				const int qA = 1 + h * 2;  // 玩家 cell 东侧 q1/q3
				const int qB = 0 + h * 2;  // 东邻 cell 西侧 q0/q2
				auto& lcA = cells[24][qA];
				auto& lcB = cells[25][qB];
				// v437c：**geom NULL 检查（闪退修复）**——v271 潜伏 bug：顶点循环
				// 检测到引擎重建会 `lc.geom = nullptr`（3244 行）但 verts 未清零 →
				// 同帧 v271 诊断读 lcB.geom+0x0A0 的 worldT[2]（@0xA8）→ NULL+偏移
				// = 读 0xA8 → EXCEPTION_ACCESS_VIOLATION（crash 04:59:31 实锤，
				// rbx=0xA8）。worldT 必须 geom 有效才能读。
				if (lcA.verts == 0 || lcB.verts == 0 || lcA.stride < 12 || lcB.stride < 12 ||
					!lcA.geom || !lcB.geom ||
					lcA.orig.size() < static_cast<std::size_t>(lcA.stride) * lcA.verts ||
					lcB.orig.size() < static_cast<std::size_t>(lcB.stride) * lcB.verts)
					continue;
				const int onA = static_cast<int>(std::sqrt(static_cast<double>(lcA.verts)) + 0.5);
				const int onB = static_cast<int>(std::sqrt(static_cast<double>(lcB.verts)) + 0.5);
				if (onA * onA != static_cast<int>(lcA.verts) || onB * onB != static_cast<int>(lcB.verts))
					continue;
				const int rA = onA / 2;
				const int rB = onB / 2;
				const float zoA = *reinterpret_cast<const float*>(
					lcA.orig.data() + static_cast<std::size_t>(rA * onA + (onA - 1)) * lcA.stride + 8);
				const float zoB = *reinterpret_cast<const float*>(
					lcB.orig.data() + static_cast<std::size_t>(rB * onB + 0) * lcB.stride + 8);
				const float zA = *reinterpret_cast<const float*>(
					lcA.work.data() + static_cast<std::size_t>(rA * onA + (onA - 1)) * lcA.stride + 8);
				const float zB = *reinterpret_cast<const float*>(
					lcB.work.data() + static_cast<std::size_t>(rB * onB + 0) * lcB.stride + 8);
				// v240：加顶点世界位置打印——确认 ci=13 的 orig 是否真的东邻 cell 数据
				// （预期 wxA==wxB==玩家 cell 中心+2048；若错位 → 位置不同 → 数据错位实锤）
				const float wxA = *reinterpret_cast<const float*>(
					lcA.orig.data() + static_cast<std::size_t>(rA * onA + (onA - 1)) * lcA.stride + 0) + lcA.worldT[0];
				const float wyA = *reinterpret_cast<const float*>(
					lcA.orig.data() + static_cast<std::size_t>(rA * onA + (onA - 1)) * lcA.stride + 4) + lcA.worldT[1];
				const float wxB = *reinterpret_cast<const float*>(
					lcB.orig.data() + static_cast<std::size_t>(rB * onB + 0) * lcB.stride + 0) + lcB.worldT[0];
				const float wyB = *reinterpret_cast<const float*>(
					lcB.orig.data() + static_cast<std::size_t>(rB * onB + 0) * lcB.stride + 4) + lcB.worldT[1];
				// v271：**世界高度对比**——v270 实锤 orig dZ=-1360（A/B 同世界位置两个
				// 129² 局部 z 差恒定 1360）。129² z 是局部高度，渲染时 + 引擎节点真实
				// worldT[2]（lc.worldT[2] 缓存写死 0 ≠ 引擎真实）→ 必须读引擎节点 worldT：
				//   (zA+wtA)-(zB+wtB)≈0 → 世界连续 → 129² 构建正确 → 裂缝在渲染层
				//   (zA+wtA)-(zB+wtB) 大 → 世界不连续 → BuildCell 高度基准 bug
				const auto* wtA = reinterpret_cast<const float*>(
					reinterpret_cast<std::uintptr_t>(lcA.geom) + 0x07C + 0x24);
				const auto* wtB = reinterpret_cast<const float*>(
					reinterpret_cast<std::uintptr_t>(lcB.geom) + 0x07C + 0x24);
				SKSE::log::info("v271: E-bnd q{}: A@({:.0f},{:.0f}) zA={:.1f} wtA[2]={:.1f} wA={:.1f} | B@({:.0f},{:.0f}) zB={:.1f} wtB[2]={:.1f} wB={:.1f} | dWorld={:.2f} dOrig={:.2f} (vA={} vB={})",
					qA, wxA, wyA, zA, wtA[2], zA + wtA[2], wxB, wyB, zB, wtB[2], zB + wtB[2],
					(zA + wtA[2]) - (zB + wtB[2]), zoA - zoB, lcA.verts, lcB.verts);
				// v241：玩家 z 对比——判断 129² 顶点 z 是"世界高度"还是"局部高度"：
				//   playerZ ≈ zA → 129² z 世界（正确）→ ci=13 的 zB 数据错（+1120）
				//   playerZ ≈ 1000+ → 129² z 局部（渲染层 transform 修正）→ dZ 差是正常基准差
				SKSE::log::info("v241: playerZ={:.1f} player=({:.0f},{:.0f}) | q{} zA={:.1f} zB={:.1f}",
					playerPos.z, playerPos.x, playerPos.y, qA, zA, zB);
			}
		}
		// v160：全 25 cell × 4 象限重建检测 + 自动恢复——v142 只查玩家 cell（ci=12），
		// 其他 24 cell 的 geom 被引擎 LOD 重建（vertexCount 变化/对象替换）后 SafeGeomValid
		// 标记失效（lc.geom=nullptr）但无人触发重缓存 → 地块逐渐全部失效（"不是所有
		// 地块都能渲染"）。每 1 秒全量扫（v199：2s→1s 更快恢复引擎重建后的高密度）：
		// valid/invalid 统计 + invalid>0 节流重缓存。
		// v280：needRecover 检测 1000ms → **300ms**——"凹凸消失一秒又回来" = 引擎降级
		// 129² 后 needRecover 要等 1 秒才检测到 + 0.4s rebuild = 恢复慢。300ms 检测 +
		// v274 同步补建 3×3 → 凹凸消失 < 0.5s（人眼勉强可接受）。
		if (now2 - landLastDiag3 > 300) {
			landLastDiag3 = now2;
			int validCount = 0, invalidCount = 0, nullCount = 0;
			for (int ciA = 0; ciA < 49; ciA++) {
				for (int qd = 0; qd < 4; qd++) {
					auto& cg = cells[ciA][qd];
					if (!cg.geom || cg.verts == 0) {
						nullCount++;
						continue;
					}
					std::uint32_t curVc = 0;
					if (SafeGeomValid(cg.geom, cg.verts, curVc)) {
						validCount++;
					} else {
						invalidCount++;
						if (ciA == 24)
							SKSE::log::info("v160: player q{} REBUILT cachedVc={} curVc={}",
								qd, cg.verts, curVc);
					}
				}
			}
			// v264：**needRecover 独立判断（不在 invalidCount>0 内）**——v263 实锤：玩家
			// 移动后引擎重建把玩家 cell 也降级回 289（v233 cachedVerts=289），而 289 对象
			// vs 289 缓存 SafeGeomValid 自洽通过 → invalidCount=0 → 旧结构（needRecover 在
			// invalidCount>0 块内）根本不执行检查 → 不恢复 → 玩家脚下 289（用户实锤"脚下
			// 地块有问题"）。**中心 5×5 必须全 129²（16641）**，任何非 129² = 引擎重建覆盖
			// = 需要恢复。玩家停下后引擎 LOD 稳定 → 129² 保持 → 不触发（防循环）。
			bool needRecover = false;
			// v284：**检测范围 5×5 → 3×3**——v283 实测（日志）：v151/v233 玩家 cell 全
			// 16641 但 v268 每秒触发"center 5x5 not 129²"→ **Smooth Terrain 只覆盖中心
			// 区域，5×5 外圈（±2）给 65²/17²（密度混合仍在）** → 检测 5×5 有外圈非
			// 129² → 反复 rebuild → 闪。玩家脚下 3×3 必须 129²（v274 盖章同步补建保证），
			// 外圈 ±2 允许引擎密度（玩家视野边缘，坑不需要那么密）→ 只检测 3×3 →
			// 循环停止 → 不闪。变形场（v266）让外圈引擎网格也参与变形（边界连续）。
			for (int dy2 = -1; dy2 <= 1 && !needRecover; dy2++) {
				for (int dx2 = -1; dx2 <= 1 && !needRecover; dx2++) {
					const int ci2 = (dy2 + 3) * 7 + (dx2 + 3);
					for (int qd = 0; qd < 4 && !needRecover; qd++) {
						std::uint32_t curVc = 0;
						auto& cg = cells[ci2][qd];
						if (cg.geom && cg.verts > 0) {
							if (cg.verts != highResDim * highResDim ||
								!SafeGeomValid(cg.geom, cg.verts, curVc)) {
								needRecover = true;
							}
						}
					}
				}
			}
			if (needRecover) {
				if (now2 - landLastRequest > 800) {  // v565b：highResBuildQueued 已删（BuildHighResMesh 死链，恒 false）
					SKSE::log::info("v434: center 3x3 not 129² (valid={}/196 invalid={} null={}) — zero-replace skip",
						validCount, invalidCount, nullCount);
					// v284b：**needRecover 不再重建**（零替换架构——Smooth Terrain 129²
					// 直接改顶点，引擎重建后 FindLandscape 重缓存自动重新变形）。
					// v434：撤销 v432 替换恢复（v433-dbg 实锤替换致 rd 分裂 + 闪）。
					landLastRequest = now2;
				}
			} else if (invalidCount > 0) {
				SKSE::log::info("v264: valid={}/196 invalid={} null={} (center 5x5 all 129²) — no rebuild",
					validCount, invalidCount, nullCount);
			} else if (now2 - landLastDiag > 3000) {
				// 无失效时也周期报告全量状态（确认所有地块保持有效）
				landLastDiag = now2;
				SKSE::log::info("v160: all valid={}/100 (null={} unstamped areas)", validCount, nullCount);
			}
		}
		if (now2 - landLastDiag2 > 2000) {
			landLastDiag2 = now2;
			std::string line;
			for (int qd = 0; qd < 4; qd++) {
				auto& cg = cells[24][qd];
				if (cg.geom && cg.verts > 0 && cg.raised)
					line += cg.verts == 4225 ? "Q" : (cg.verts == 1089 ? "m" : "l");
				else
					line += "-";
			}
			SKSE::log::info("v140: player-cell quads=[{}] (Q=65x65 m=33x33 l=17x17 -=miss)", line);
		}
	}

	void SnowShellMesh::ResetForLoadGame()
	{
		// v449b：读档/新游戏——引擎卸载全部地形/REFR/场景，所有缓存指向已释放
		// 逐项清空（vector 用默认构造安全复位），下一轮 FindLandscape 重建。
		// v569：**landReady 先置 false（停渲染线程）再清空**——原顺序 clear 场后
		// 才 store(false)，渲染线程在窗口内仍写场 → 竞争。footprints/colliders
		// 清空加锁（渲染线程 RebuildField/ScanColliders 并发）。
		landReady.store(false);
		landFootDirty.store(false);
		for (auto& gen : landBuf)
			for (auto& row : gen)
				for (auto& q : row)
					q = LandCellGeom{};
		{
			std::lock_guard<std::mutex> lkF9(footMtx);
			footprints.clear();
		}
		{
			std::lock_guard<std::mutex> lkC9(colliderMtx);
			colliders.clear();
		}
		lastObjPos.clear();
		mineCounts.clear();
		lastPos.clear();    // v609：读档清空（formID 复用防误盖）
		g_corpseQ.clear();  // v609：读档清空（防旧尸体 2s 后在新档位置盖坑）
		deformField.clear();
		ridgeField.clear();
		deformFieldObj.clear();  // v529：物体场清空
		ridgeFieldObj.clear();
		sceneLiftBuf = {};
		terrainH.clear();
		root = nullptr;
		dynMesh = nullptr;
		rendererData = nullptr;
		SKSE::log::info("v449b: reset all cache for load game");
	}
}
