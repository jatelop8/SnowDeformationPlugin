// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

// MeshScanner.cpp —— 雪网格扫描器（Phase 2A 第一步）
// 遍历玩家周围 REF 的 3D 节点树，用 kSnow shader flag 过滤，
// 汇总"哪些网格是雪、多少实例、多少顶点、运行时类层次如何"。
// 输出：SKSE 日志（详细）+ 游戏内控制台（精简，SnowDeformScan <radius> 触发）

#include "MeshScanner.h"
#include "NiUtils.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <map>
#include <sstream>

namespace SnowDeform
{
	namespace
	{
		// 取运行时类名（验证 Skyrim SE 场景树节点的真实类层次）
		[[nodiscard]] std::string RTTIName(RE::NiAVObject* a_node)
		{
			if (!a_node)
				return "null";
			const auto rtti = a_node->GetRTTI();
			if (rtti && rtti->GetName())
				return rtti->GetName();
			return "?";
		}

		// 从任意节点取 shader property（兼容 BSGeometry / NiGeometry 两种布局）
		// v67：恢复 GetRuntimeData()（v62 实测安全）——裸偏移 0x118 对树网格
		// （L2_Branches04 等特殊网格）读到 float 当指针 → RTTI 解引用崩（v66 实锤）
		[[nodiscard]] RE::BSShaderProperty* GetShaderProperty(RE::NiAVObject* a_node)
		{
			if (auto geom = As<RE::BSGeometry>(a_node, "BSGeometry")) {
				const auto& rt = geom->GetGeometryRuntimeData();
				auto* prop = rt.properties[RE::BSGeometry::States::kEffect].get();
				if (prop && RTTIIsA(prop, "BSShaderProperty"))
					return static_cast<RE::BSShaderProperty*>(prop);
			}
			if (auto niGeom = As<RE::NiGeometry>(a_node, "NiGeometry")) {
				const auto& rt = niGeom->GetRuntimeData();
				auto* prop = rt.spEffectState.get();
				if (prop && RTTIIsA(prop, "BSShaderProperty"))
					return static_cast<RE::BSShaderProperty*>(prop);
				auto* prop2 = rt.spPropertyState.get();
				if (prop2 && RTTIIsA(prop2, "BSShaderProperty"))
					return static_cast<RE::BSShaderProperty*>(prop2);
			}
			return nullptr;
		}

		// 判断是否为"雪"着色器（材质类为主 + shader flag 为辅）
		// 雪物体（SnowDrift 等）的材质是 BSLightingShaderMaterialSnow（FEATURE=kMultiIndexTriShapeSnow）；
		// 部分雪材质只设 EShaderPropertyFlag::kSnow（bit 60），故双保险。
		[[nodiscard]] bool IsSnowShader(RE::BSShaderProperty* a_sp)
		{
			if (!a_sp)
				return false;

			// 1) 材质 Feature 检测（BSLightingShaderMaterialSnow 的 FEATURE 就是 kMultiIndexTriShapeSnow）
			if (auto* mat = a_sp->GetBaseMaterial()) {
				if (mat->GetFeature() == RE::BSShaderMaterial::Feature::kMultiIndexTriShapeSnow) {
					return true;
				}
			}

			// 2) shader flags 检测
			if (a_sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSnow)) {
				return true;
			}

			return false;
		}

		// 递归遍历节点树，收集 kSnow 几何节点信息
		void Collect(RE::NiAVObject* a_node, SnowMeshEntry& a_entry)
		{
			if (!a_node)
				return;

			// 记录运行时类名（去重）
			const auto cls = RTTIName(a_node);
			if (a_entry.rttiClasses.find(cls) == std::string::npos) {
				if (!a_entry.rttiClasses.empty())
					a_entry.rttiClasses += ",";
				a_entry.rttiClasses += cls;
			}

			// 几何节点？查雪着色器（材质类 + shader flag 双检测）
			if (const auto sp = GetShaderProperty(a_node)) {
				if (IsSnowShader(sp)) {
					a_entry.geomCount++;

					// CPU 顶点路径验证（NiGeometryData——v67 恢复成员访问）
					if (auto niGeom = As<RE::NiGeometry>(a_node, "NiGeometry")) {
						if (auto data = niGeom->GetRuntimeData().spModelData.get()) {
							a_entry.vertexCount += data->vertices;
							a_entry.hasCpuVertices = true;
						}
					}
				}
			}

			// 递归子节点
			if (auto niNode = As<RE::NiNode>(a_node, "NiNode")) {
				for (auto& child : niNode->GetChildren()) {
					if (child) {
						Collect(child.get(), a_entry);
					}
				}
			}
		}
	}

	std::vector<SnowMeshEntry> MeshScanner::Scan(float a_radius)
	{
		std::vector<SnowMeshEntry> result;

		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return result;

		// 按根节点名聚合（同 NIF 多实例 → 一个条目）
		std::map<std::string, SnowMeshEntry> agg;
		std::uint32_t refCount = 0;

		auto* tes = RE::TES::GetSingleton();
		if (!tes) {
			SKSE::log::warn("MeshScanner: TES singleton is null");
			return result;
		}

		tes->ForEachReferenceInRange(
			player, a_radius,
			[&](RE::TESObjectREFR* a_ref) {
				if (!a_ref)
					return RE::BSContainer::ForEachResult::kContinue;

				refCount++;

				auto root = a_ref->Get3D();
				if (!root)
					return RE::BSContainer::ForEachResult::kContinue;

				const char* name = root->name.c_str();
				const std::string key = (name && *name) ? name : "<unnamed>";

				auto& entry = agg[key];
				entry.meshName = key;
				entry.instanceCount++;

				// 最近距离（根节点世界位置——v67 恢复成员访问，裸偏移对特殊网格不可靠）
				const float dist = (root->world.translate - player->GetPosition()).Length();
				entry.nearestDist = std::min(entry.nearestDist, dist);

				Collect(root, entry);

				return RE::BSContainer::ForEachResult::kContinue;
			});

		result.reserve(agg.size());
		for (auto& [k, v] : agg) {
			if (v.instanceCount > 1)
				v.hasSharedMesh = true;  // 同 NIF 名被多个 REF 引用
			result.push_back(std::move(v));
		}

		// 记录 REF 总数（判断是否雪地：室内/地下城 REF 少且无雪）
		SKSE::log::info("MeshScanner: scanned {} references within radius {}", refCount, a_radius);

		// 按距离排序（近的在前）
		std::sort(result.begin(), result.end(),
			[](const SnowMeshEntry& a, const SnowMeshEntry& b) { return a.nearestDist < b.nearestDist; });

		return result;
	}

	void MeshScanner::LogSummary(const std::vector<SnowMeshEntry>& a_entries)
	{
		// 详细行 → SKSE 日志（spdlog / fmt 格式）
		SKSE::log::info("=== SnowDeform scan: {} kSnow mesh(es) found ===", a_entries.size());
		for (const auto& e : a_entries) {
			SKSE::log::info("[{}] dist={:.0f} inst={} geom={} verts={} cpuVerts={} shared={} classes={}",
				e.meshName, e.nearestDist, e.instanceCount, e.geomCount,
				e.vertexCount, e.hasCpuVertices ? "yes" : "no",
				e.hasSharedMesh ? "yes" : "no", e.rttiClasses);
		}

		// 精简行 → 游戏内控制台（printf 格式，手动拼字符串）
		std::ostringstream oss;
		oss << "SnowDeform: " << a_entries.size() << " snow mesh(es)\n";
		for (const auto& e : a_entries) {
			oss << "  " << e.meshName
				<< " dist=" << static_cast<int>(e.nearestDist)
				<< " inst=" << e.instanceCount
				<< " verts=" << e.vertexCount
				<< (e.hasCpuVertices ? " [CPU]" : " [GPU?]")
				<< (e.hasSharedMesh ? " [shared]" : "")
				<< "\n";
		}
		if (auto* console = RE::ConsoleLog::GetSingleton()) {
			console->Print("%s", oss.str().c_str());
		}
	}
}
