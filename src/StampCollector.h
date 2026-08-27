// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

#pragma once

#include <cstdint>
#include <vector>

namespace SnowDeform
{
	// 一个印章：世界 XY 位置、深度(1.0)、半径（世界单位）
	struct StampData
	{
		float x, y, depth, radius;
	};

	// 收集本帧所有应写入变形图的印章（玩家、NPC、尸体、可移动道具）
	class StampCollector
	{
	public:
		StampCollector() = default;

		// 每帧调用：扫描玩家 + 附近 actor 的 Havok 碰撞形状，填充印章列表
		// outStamps / outStampEnds 与 FrameInput 中的数组对应
		uint32_t Collect(
			float cameraX, float cameraY, float deformWorldSize,
			StampData* outStamps, StampData* outStampEnds, uint32_t maxStamps);

		// v569：读档清理胶囊轨迹缓存（static——不碰实例状态）
		static void ClearPrevPositions();
	};
}
