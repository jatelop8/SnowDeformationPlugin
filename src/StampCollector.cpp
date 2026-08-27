// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

// StampCollector.cpp —— 采集角色 Havok 碰撞形状生成印章
// v185：全量对齐 CS SnowDeformation Stamping.cpp——
//   TraverseScenegraphCollision 遍历角色 3D 碰撞对象 → GetShapeBound 提取
//   每个形状（脚/腿/手/躯干）中心+半径 → 每个形状独立印章（胶囊线段
//   = 上一帧→本帧位置，高速移动轨迹连续）——真实鞋印（黑神话效果关键）。
// 过滤：离地 40 内（空中肢体不刻）、半径 4-128、空中角色不刻、传送断线。
// Phase 1：玩家 + 附近 NPC（尸体/道具后续）。

#include "StampCollector.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cmath>
#include <unordered_map>

namespace SnowDeform
{
	namespace
	{
		// 形状底部离地超过此值不刻（脚踩地才留印）
		constexpr float kStampSurfaceBand = 40.0f;
		// 形状半径合理范围
		constexpr float kMinStampShapeRadius = 4.0f;
		constexpr float kMaxStampShapeRadius = 128.0f;
		// 传送/读图位移超过此值打断胶囊轨迹
		constexpr float kTrailBreakDistance = 256.0f;
		// 低于此位移算静止（不产生新印章）
		constexpr float kStampMovementGate = 3.0f;

		// 上一帧形状位置（key = formID<<16 | shapeIndex），胶囊轨迹用
		struct PrevPos2
		{
			float x, y;
		};
		std::unordered_map<std::uint64_t, PrevPos2> g_stampPrevPositions;

		// 按形状类型提取半径（CS ActorUtils::ExtractShapeBound 移植）
		bool ExtractShapeBound(const RE::hkpShape* a_shape, float& a_radius)
		{
			using ShapeType = RE::hkpShapeType;
			if (!a_shape)
				return false;
			auto project = [a_shape](float x, float y, float z) {
				return a_shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			};
			auto symmetricHalfExtents = [&project](float& hx, float& hy, float& hz) {
				float xp = project(1.0f, 0.0f, 0.0f);
				float xn = project(-1.0f, 0.0f, 0.0f);
				float yp = project(0.0f, 1.0f, 0.0f);
				float yn = project(0.0f, -1.0f, 0.0f);
				float zp = project(0.0f, 0.0f, 1.0f);
				float zn = project(0.0f, 0.0f, -1.0f);
				hx = 0.5f * (xp - xn);
				hy = 0.5f * (yp - yn);
				hz = 0.5f * (zp - zn);
			};
			auto halfDiagonal = [](float hx, float hy, float hz) {
				return std::sqrt(hx * hx + hy * hy + hz * hz);
			};
			switch (a_shape->type) {
			case ShapeType::kCapsule: {
				float hx, hy, hz;
				symmetricHalfExtents(hx, hy, hz);
				a_radius = std::max(hx, std::max(hy, hz));
				return true;
			}
			case ShapeType::kSphere: {
				float hx, hy, hz;
				symmetricHalfExtents(hx, hy, hz);
				a_radius = hx;
				return true;
			}
			case ShapeType::kBox: {
				float hx, hy, hz;
				symmetricHalfExtents(hx, hy, hz);
				a_radius = halfDiagonal(hx, hy, hz);
				return true;
			}
			case ShapeType::kCylinder: {
				float hx, hy, hz;
				symmetricHalfExtents(hx, hy, hz);
				float hr = std::max(hx, hy);
				a_radius = std::sqrt(hr * hr + hz * hz);
				return true;
			}
			case ShapeType::kConvexVertices:
			case ShapeType::kTriangle: {
				float hx, hy, hz;
				symmetricHalfExtents(hx, hy, hz);
				a_radius = std::max(hx, std::max(hy, hz));
				return true;
			}
			default:
				return false;
			}
		}

		// 从碰撞对象提取形状包围球（CS ActorUtils::GetShapeBound 移植）
		bool GetShapeBound(RE::bhkNiCollisionObject* a_obj, RE::NiPoint3& a_center, float& a_radius)
		{
			if (!a_obj)
				return false;
			auto* bhkRigid = a_obj->body.get() ? a_obj->body.get()->AsBhkRigidBody() : nullptr;
			auto* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
			if (!bhkRigid || !hkpRigid || skyrim_cast<RE::hkpListShape*>(hkpRigid))
				return false;  // hkpListShape 不支持，跳过
			RE::hkVector4 massCenter;
			bhkRigid->GetCenterOfMassWorld(massCenter);
			float massTrans[4];
			_mm_storeu_ps(massTrans, massCenter.quad);
			a_center = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();
			return ExtractShapeBound(hkpRigid->collidable.GetShape(), a_radius);
		}
	}

	uint32_t StampCollector::Collect(
		float cameraX, float cameraY, float deformWorldSize,
		StampData* outStamps, StampData* outStampEnds, uint32_t maxStamps)
	{
		uint32_t stampCount = 0;
		const float radiusSq = 0.25f * deformWorldSize * deformWorldSize;
		std::unordered_map<std::uint64_t, PrevPos2> currentPositions;

		auto addActor = [&](RE::Actor* actor) {
			if (!actor || stampCount >= maxStamps)
				return;
			if (!actor->Is3DLoaded())
				return;
			const auto pos = actor->GetPosition();
			const float dx = pos.x - cameraX;
			const float dy = pos.y - cameraY;
			if (dx * dx + dy * dy > radiusSq)
				return;
			auto* root = actor->Get3D(false);
			if (!root)
				return;

			// 空中角色不刻（跳跃/下落）；尸体后续单独处理
			if (!actor->IsDead()) {
				if (auto* ctrl = actor->GetCharController()) {
					if (ctrl->context.currentState == RE::hkpCharacterStateType::kInAir)
						return;
				}
			}

			const uint32_t formID = actor->formID;
			const float groundZ = pos.z;
			uint32_t shapeIndex = 0;

			// 遍历角色 3D 全部碰撞对象（脚/腿/手/躯干各自独立印章）
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 centerPos;
				float radius;
				if (GetShapeBound(a_object, centerPos, radius)) {
					const uint32_t thisIndex = shapeIndex++;
					if (stampCount >= maxStamps)
						return RE::BSVisit::BSVisitControl::kStop;
					// 形状底部离地太远（空中肢体）不刻
					if (centerPos.z - radius > groundZ + kStampSurfaceBand)
						return RE::BSVisit::BSVisitControl::kContinue;
					if (radius < kMinStampShapeRadius || radius > kMaxStampShapeRadius)
						return RE::BSVisit::BSVisitControl::kContinue;

					// 胶囊印章：上一帧位置 → 本帧位置（高速移动轨迹连续）
					const float cx = centerPos.x;
					const float cy = centerPos.y;
					const std::uint64_t key = (std::uint64_t(formID) << 16) | (thisIndex & 0xFFFF);
					float px = cx;
					float py = cy;
					auto it = g_stampPrevPositions.find(key);
					bool moved = true;
					if (it != g_stampPrevPositions.end()) {
						const float ddx = cx - it->second.x;
						const float ddy = cy - it->second.y;
						const float sq = ddx * ddx + ddy * ddy;
						if (sq < kTrailBreakDistance * kTrailBreakDistance) {
							px = it->second.x;
							py = it->second.y;
						}
						moved = sq > kStampMovementGate * kStampMovementGate;
					}
					currentPositions[key] = { cx, cy };
					if (!moved)
						return RE::BSVisit::BSVisitControl::kContinue;  // 静止不盖章

					outStamps[stampCount] = { cx, cy, 1.0f, radius };
					outStampEnds[stampCount] = { px, py, 0.0f, 0.0f };
					stampCount++;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};

		// 玩家
		if (const auto player = RE::PlayerCharacter::GetSingleton())
			addActor(player);

		// 附近 NPC
		if (const auto processLists = RE::ProcessLists::GetSingleton()) {
			for (const auto& handle : processLists->highActorHandles) {
				if (const auto actor = handle.get())
					addActor(actor.get());
			}
		}

		// 更新上一帧位置
		for (const auto& [k, v] : currentPositions)
			g_stampPrevPositions[k] = v;

		lastStampCount = stampCount;
		return stampCount;
	}
}
