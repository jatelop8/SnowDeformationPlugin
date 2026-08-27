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

#include <cstring>

namespace SnowDeform
{
	// CommonLibSSE-NG 的 dynamic_cast 陷阱：RE:: 命名空间的 typeid 与引擎对象的
	// RTTI（全局命名空间）名字不匹配，dynamic_cast<RE::Xxx*> 在引擎对象上会失败。
	//
	// 也不能用 T::RTTI.address() / T::Ni_RTTI.address()（Address Library ID 比较）：
	// RTTI 的 REL ID 不在 Address Library 数据库里，id2offset 找不到 ID 会直接崩
	// （stl::report_and_fail，实测 v15 闪退）。
	//
	// 正确做法：遍历引擎 RTTI 链（GetRTTI()/GetBaseRTTI()），比较**名字**。
	// 引擎 RTTI 名是全局命名空间的类名（"NiNode"、"BSGeometry"...），子类的 RTTI 链
	// 上必然包含基类名 → 用名字链判断 is-a 关系，不依赖 Address Library。

	// NiAVObject* 的 RTTI 名字链判断
	[[nodiscard]] inline bool RTTIIsA(RE::NiAVObject* a_obj, const char* a_className)
	{
		if (!a_obj || !a_className)
			return false;
		for (auto rtti = a_obj->GetRTTI(); rtti; rtti = rtti->GetBaseRTTI()) {
			const char* name = rtti->GetName();
			if (name && std::strcmp(name, a_className) == 0)
				return true;
		}
		return false;
	}

	// NiObject*（材质等非 AVObject）的 RTTI 名字链判断
	[[nodiscard]] inline bool RTTIIsA(RE::NiObject* a_obj, const char* a_className)
	{
		if (!a_obj || !a_className)
			return false;
		for (auto rtti = a_obj->GetRTTI(); rtti; rtti = rtti->GetBaseRTTI()) {
			const char* name = rtti->GetName();
			if (name && std::strcmp(name, a_className) == 0)
				return true;
		}
		return false;
	}

	// 便捷转换：NiAVObject → 目标类指针（替代 dynamic_cast）
	template <class T>
	[[nodiscard]] inline T* As(RE::NiAVObject* a_obj, const char* a_className)
	{
		return RTTIIsA(a_obj, a_className) ? static_cast<T*>(a_obj) : nullptr;
	}
}
