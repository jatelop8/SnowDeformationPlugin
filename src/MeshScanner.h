// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>
#include <vector>

namespace SnowDeform
{
	// 单个雪网格（按 NIF 根节点名聚合）的扫描结果
	struct SnowMeshEntry
	{
		std::string    meshName;        // 3D 根节点名（通常 = NIF 文件名）
		std::uint32_t  instanceCount = 0;  // 世界内同网格实例数（REF 数）
		std::uint32_t  geomCount = 0;      // 带 kSnow 的几何节点数
		std::uint32_t  vertexCount = 0;    // 顶点总数（全部实例）
		bool           hasCpuVertices = false;  // 是否走通 NiGeometryData CPU 顶点路径
		bool           hasSharedMesh = false;   // 同 NIF 被多个 REF 引用（共享网格）
		std::string    rttiClasses;             // 观测到的运行时类名（验证类层次）
		float          nearestDist = 1e30f;     // 最近实例到玩家距离
	};

	// 雪网格扫描器：遍历玩家周围 REF，递归 NiAVObject 树，
	// 用 BSLightingShaderProperty 的 kSnow flag 过滤，汇总统计。
	// 目的：摸清游戏内"雪网格"清单与结构，确定 2A 目标范围。
	class MeshScanner
	{
	public:
		// 扫描玩家周围 radius（世界单位）内的所有 kSnow 网格
		static std::vector<SnowMeshEntry> Scan(float radius);

		// 打印汇总：SKSE 日志 + 游戏内控制台（ConsoleLog）
		static void LogSummary(const std::vector<SnowMeshEntry>& entries);
	};
}
