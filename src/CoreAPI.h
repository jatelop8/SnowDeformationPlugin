// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

#pragma once
// DynamicShader Core 公共 API（功能插件引用此头）
// 前置 DLL 模式（类似 Community Shaders）：Core 提供渲染 hook / 游戏线程调度 /
// 天气状态等公共设施，功能插件（动态雪/水洼/皮肤/灯）通过 DynamicShader_GetAPI
// 获取函数指针表，独立编译、独立部署、玩家可选安装。

#include <cstdint>

struct ID3D11DeviceContext;

namespace DynamicShader
{
	// 渲染线程每帧回调（Core Present hook 内调用，渲染线程）
	using RenderCallback = void (*)(ID3D11DeviceContext* a_ctx, void* a_user);
	// 游戏线程任务（Core 封装 SKSE TaskInterface）
	using TaskCallback = void (*)(void* a_user);

	// 天气类型（Core 每 1 秒轮询 Sky 当前天气）
	enum WeatherType : std::int32_t
	{
		kWeatherOther = 0,
		kWeatherSnow = 1,
		kWeatherRain = 2,
	};

	// Core API 函数指针表（v1 骨架——后续按需扩展：地形缓存/变形场/材质工具）
	struct DS_API
	{
		std::uint32_t version = 1;

		// 注册渲染线程每帧回调（Present hook 内按注册序调用）。返回 false = 已满/失败。
		bool (*RegisterRenderCallback)(RenderCallback a_cb, void* a_user);

		// 取消渲染回调
		bool (*UnregisterRenderCallback)(RenderCallback a_cb, void* a_user);

		// 调度游戏线程任务（SKSE::GetTaskInterface()->AddTask 封装）
		void (*AddGameTask)(TaskCallback a_fn, void* a_user);

		// 当前天气（1s 轮询缓存，线程安全读）
		WeatherType (*GetWeather)();

		// 是否正在下雨（GetWeather()==kWeatherRain 的便捷封装）
		bool (*IsRaining)();
	};

	// 插件加载时调用：a_version = 插件编译时用的 DS_API_VERSION。
	// 返回 Core 的 API 表（版本不匹配返回 nullptr——插件应跳过并提示）。
	constexpr std::uint32_t DS_API_VERSION = 1;
}

// Core DLL 导出函数（插件侧通过 LoadLibrary + GetProcAddress 获取；
// Core 自身编译时定义 DS_CORE_EXPORTS → dllexport）
#ifdef DS_CORE_EXPORTS
#define DS_API_EXPORT __declspec(dllexport)
#else
#define DS_API_EXPORT __declspec(dllimport)
#endif

extern "C" DS_API_EXPORT DynamicShader::DS_API* DynamicShader_GetAPI(std::uint32_t a_version);
