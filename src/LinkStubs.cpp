// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

// LinkStubs.cpp —— CommonLibSSE-NG v4.2.0 未实现的 Ni/BSTriShape 虚函数链接桩
// 背景：new/delete RE::BSDynamicTriShape 需要 BSTriShape/BSGeometry/NiAVObject/NiObjectNET
//       的析构与虚函数符号，而 CommonLibSSE-NG 4.2.0 未提供这些实现（仅声明）。
// 原理：运行时对象 vtable 由引擎 ctor() 设置为引擎真实 vtable，虚调用走引擎实现；
//       这些桩只满足链接器，正常情况下不会被调用。
#include <RE/Skyrim.h>

namespace RE
{
	// ---- NiObjectNET ----
	const NiRTTI* NiObjectNET::GetRTTI() const { return nullptr; }
	NiObjectNET::~NiObjectNET() = default;
	bool         NiObjectNET::IsEqual(NiObject*) { return false; }
	bool         NiObjectNET::RegisterStreamables(NiStream&) { return false; }
	void         NiObjectNET::LinkObject(NiStream&) {}
	void         NiObjectNET::LoadBinary(NiStream&) {}
	void         NiObjectNET::SaveBinary(NiStream&) {}
	void         NiObjectNET::PostLinkObject(NiStream&) {}
	void         NiObjectNET::ProcessClone(NiCloningProcess&) {}

	// ---- NiAVObject ----
	const NiRTTI* NiAVObject::GetRTTI() const { return nullptr; }
	NiAVObject::~NiAVObject() = default;
	bool         NiAVObject::IsEqual(NiObject*) { return false; }
	bool         NiAVObject::RegisterStreamables(NiStream&) { return false; }
	void         NiAVObject::LinkObject(NiStream&) {}
	void         NiAVObject::LoadBinary(NiStream&) {}
	void         NiAVObject::SaveBinary(NiStream&) {}
	void         NiAVObject::ProcessClone(NiCloningProcess&) {}
	void         NiAVObject::UpdateControllers(NiUpdateData&) {}

	// ---- BSGeometry ----
	const NiRTTI* BSGeometry::GetRTTI() const { return nullptr; }
	BSGeometry::~BSGeometry() = default;
	bool         BSGeometry::IsEqual(NiObject*) { return false; }
	bool         BSGeometry::RegisterStreamables(NiStream&) { return false; }
	void         BSGeometry::LinkObject(NiStream&) {}
	void         BSGeometry::LoadBinary(NiStream&) {}
	void         BSGeometry::SaveBinary(NiStream&) {}
	void         BSGeometry::PostLinkObject(NiStream&) {}
	void         BSGeometry::ProcessClone(NiCloningProcess&) {}
	BSGeometry*  BSGeometry::AsGeometry() { return nullptr; }

	// ---- BSTriShape ----
	const NiRTTI* BSTriShape::GetRTTI() const { return nullptr; }
	BSTriShape::~BSTriShape() = default;
	bool         BSTriShape::IsEqual(NiObject*) { return false; }
	bool         BSTriShape::RegisterStreamables(NiStream&) { return false; }
	void         BSTriShape::LinkObject(NiStream&) {}
	void         BSTriShape::LoadBinary(NiStream&) {}
	void         BSTriShape::SaveBinary(NiStream&) {}
	BSTriShape*  BSTriShape::AsTriShape() { return nullptr; }
	NiObject*    BSTriShape::CreateClone(NiCloningProcess&) { return nullptr; }

	// ---- BSDynamicTriShape ----
	const NiRTTI* BSDynamicTriShape::GetRTTI() const { return nullptr; }
	bool          BSDynamicTriShape::IsEqual(NiObject*) { return false; }
	bool          BSDynamicTriShape::RegisterStreamables(NiStream&) { return false; }
	void          BSDynamicTriShape::LinkObject(NiStream&) {}
	void          BSDynamicTriShape::LoadBinary(NiStream&) {}
	void          BSDynamicTriShape::SaveBinary(NiStream&) {}
	BSDynamicTriShape* BSDynamicTriShape::AsDynamicTriShape() { return nullptr; }
	NiObject*     BSDynamicTriShape::CreateClone(NiCloningProcess&) { return nullptr; }

	// ---- NiStream（CommonLibSSE-NG 4.2.0 仅声明无实现）----
	// 运行时 vtable 被替换为引擎 vtable（SnowShellMesh::LoadMesh），桩只满足链接器。
	NiStream::~NiStream() = default;
	bool          NiStream::Load1(NiBinaryStream*) { return false; }
	bool          NiStream::Load2(char*, std::uint64_t) { return false; }
	bool          NiStream::Load3(const char* a_path)
	{
		::OutputDebugStringA("[SnowDeform] STUB NiStream::Load3 CALLED (vtable replace FAILED)\n");
		return false;
	}
	bool          NiStream::Save1(NiBinaryStream*) { return false; }
	bool          NiStream::Save2(char*&, std::uint64_t&) { return false; }
	bool          NiStream::Save3(const char*) { return false; }
	void          NiStream::Unk_07() {}
	bool          NiStream::RegisterFixedString(const BSFixedString&) { return false; }
	bool          NiStream::RegisterSaveObject(NiObject*) { return false; }
	bool          NiStream::ChangeObject(NiObject*) { return false; }
	std::uint32_t NiStream::GetLinkIDFromObject(const NiObject*) { return 0; }
	void          NiStream::SaveLinkID(const NiObject*) {}
	bool          NiStream::LoadHeader() { return false; }
	void          NiStream::SaveHeader() {}
	bool          NiStream::LoadStream() { return false; }
	void          NiStream::SaveStream() {}
	void          NiStream::RegisterObjects() {}
	void          NiStream::LoadTopLevelObjects() {}
	void          NiStream::SaveTopLevelObjects() {}
	bool          NiStream::LoadObject() { return false; }
	std::uint64_t NiStream::PreSaveObjectSizeTable() { return 0; }
	bool          NiStream::SaveObjectSizeTable(std::uint64_t) { return false; }
	bool          NiStream::LoadObjectSizeTable() { return false; }

	// ---- NiTMapBase 实例化（NiStream::registerMap 成员）----
	using NiStreamMapBase = NiTMapBase<NiTPointerAllocator<std::uint64_t>, const NiObject*, std::uint32_t>;
	std::uint32_t NiStreamMapBase::hash_function(key_type) const { return 0; }
	bool          NiStreamMapBase::key_eq(key_type, key_type) const { return false; }
	void          NiStreamMapBase::assign_value(value_type*, key_type, mapped_type) {}
	void          NiStreamMapBase::clear_value(value_type*) {}

	// ---- v172：BSShaderProperty::ForEachVisitor 析构桩（CommonLib 只声明）----
	RE::BSShaderProperty::ForEachVisitor::~ForEachVisitor() {}

	// ---- v442：NiTexture/NiSourceTexture 桩（SafeInstallDynamicTex placement new
	// 构造引用其虚函数，CommonLib 4.2.0 未实现）——运行时对象由引擎管理，桩只满足链接。
	const NiRTTI* NiTexture::GetRTTI() const { return nullptr; }
	NiTexture::~NiTexture() = default;
	void         NiTexture::Unk_25() {}
	void         NiTexture::Unk_26() {}
	void         NiTexture::Unk_27() {}
	void         NiTexture::Unk_28() {}
	void         NiTexture::Unk_29() {}
	void         NiTexture::Unk_2A() {}

	const NiRTTI* NiSourceTexture::GetRTTI() const { return nullptr; }
	NiSourceTexture::~NiSourceTexture() = default;
	void         NiSourceTexture::Unk_25() {}
	void         NiSourceTexture::Unk_26() {}
	void         NiSourceTexture::Unk_27() {}
	void         NiSourceTexture::Unk_28() {}
	void         NiSourceTexture::Unk_29() {}
	void         NiSourceTexture::Unk_2A() {}
	// v562：BSTempEffectSimpleDecal/BSTempEffect 桩已移除（v561 脚印贴花全删）
}
