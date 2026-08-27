// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

// DeformationMap.cpp —— 变形图核心实现（滚动窗口 + 双缓冲 + 计算着色器更新）
// 逻辑从 Community Shaders SnowDeformation feature 剥离，不依赖 CS 渲染管线

// 注意：CommonLib（SKSE.h）必须最先——DeformationMap.h 带 d3d11.h（Windows API），
// REX::W32 强制 Windows API 头在 CommonLib 之后
#include <SKSE/SKSE.h>
#include "DeformationMap.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace SnowDeform
{
	namespace
	{
		DeformationMap g_deformationMap;

		// 计算着色器源码（与 CS DeformationUpdateCS.hlsl 逻辑等价，独立版）
		// 简化：Phase 1 直接内嵌 HLSL，避免文件加载路径问题；Phase 2 改为从 Data\Shaders 加载
		constexpr const char* kUpdateCS = R"HLSL(
#define MAX_STAMPS 128

cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	int2 ScrollDelta;

	float TexelSize;
	uint StampCount;
	float RefillAmount;
	uint ClearMap;

	float4 Stamps[MAX_STAMPS];     // xy: world pos, z: depth, w: radius
	float4 StampEnds[MAX_STAMPS];  // xy: previous world pos (capsule segment start)
};

Texture2D<float> PreviousDeformation : register(t0);
RWTexture2D<float> CurrentDeformation : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 pixel = DTid.xy;

	float deformation = 0.0;

	if (!ClearMap) {
		int2 sourcePixel = int2(pixel) + ScrollDelta;

		uint2 dims;
		PreviousDeformation.GetDimensions(dims.x, dims.y);

		[branch] if (all(sourcePixel >= 0) && all(sourcePixel < int2(dims)))
		{
			deformation = PreviousDeformation[uint2(sourcePixel)];
		}

		deformation = max(deformation - RefillAmount, 0.0);
	}

	float2 worldPos = WindowOrigin + (float2(pixel) + 0.5) * TexelSize;

	for (uint i = 0; i < StampCount; i++) {
		float2 p0 = StampEnds[i].xy;
		float2 p1 = Stamps[i].xy;
		float2 seg = p1 - p0;
		float segLenSq = dot(seg, seg);
		float t = segLenSq > 1e-4 ? saturate(dot(worldPos - p0, seg) / segLenSq) : 0.0;
		float2 delta = worldPos - (p0 + seg * t);
		float distSq = dot(delta, delta);
		float radius = Stamps[i].w;

		[branch] if (distSq < radius * radius)
		{
			float falloff = 1.0 - smoothstep(0.2, 1.0, sqrt(distSq) / radius);
			deformation = max(deformation, Stamps[i].z * falloff);
		}
	}

	CurrentDeformation[pixel] = deformation;
}
)HLSL";
	}

	DeformationMap& GetDeformationMap()
	{
		return g_deformationMap;
	}

	bool DeformationMap::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (initialized)
			return true;

		// v64：防御——device/context 必须有效
		if (!a_device || !a_context) {
			SKSE::log::error("DeformationMap: null device or context");
			return false;
		}

		device = a_device;
		context = a_context;

		if (!CreateResources())
			return false;
		if (!CreateComputeShader())
			return false;

		initialized = true;
		return true;
	}


	bool DeformationMap::CreateResources()
	{
		// 双缓冲变形图：R32_FLOAT（存归一化深度 0=未踩，1=压实）
		for (int i = 0; i < 2; i++) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTextureDim;
			desc.Height = kTextureDim;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			HRESULT hr = device->CreateTexture2D(&desc, nullptr, textures[i].tex.ReleaseAndGetAddressOf());
			if (FAILED(hr))
				return false;

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = device->CreateShaderResourceView(textures[i].tex.Get(), &srvDesc, textures[i].srv.ReleaseAndGetAddressOf());
			if (FAILED(hr))
				return false;

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			hr = device->CreateUnorderedAccessView(textures[i].tex.Get(), &uavDesc, textures[i].uav.ReleaseAndGetAddressOf());
			if (FAILED(hr))
				return false;
		}

		// 常量缓冲
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(PerFrameCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, perFrameCB.ReleaseAndGetAddressOf());
		return SUCCEEDED(hr);
	}

	bool DeformationMap::CreateComputeShader()
	{
		Microsoft::WRL::ComPtr<ID3DBlob> blob;
		Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

		HRESULT hr = D3DCompile(
			kUpdateCS, strlen(kUpdateCS), "DeformationUpdateCS.hlsl", nullptr, nullptr,
			"main", "cs_5_0", 0, 0, blob.ReleaseAndGetAddressOf(), errorBlob.ReleaseAndGetAddressOf());

		if (FAILED(hr)) {
			if (errorBlob)
				fprintf(stderr, "[SnowDeform] CS compile error: %s\n", static_cast<const char*>(errorBlob->GetBufferPointer()));
			return false;
		}

		hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, updateCS.ReleaseAndGetAddressOf());
		return SUCCEEDED(hr);
	}

	ID3D11ShaderResourceView* DeformationMap::Update(const FrameInput& input)
	{
		if (!initialized)
			return nullptr;

		PerFrameCB cb{};
		cb.WindowOriginX = input.windowOriginX;
		cb.WindowOriginY = input.windowOriginY;
		cb.ScrollDeltaX = input.scrollDeltaX;
		cb.ScrollDeltaY = input.scrollDeltaY;
		cb.TexelSize = kTexelSize;
		cb.StampCount = input.stampCount;
		cb.RefillAmount = input.refillAmount;
		cb.ClearMap = input.clearMap ? 1u : 0u;

		for (uint32_t i = 0; i < input.stampCount && i < kMaxStamps; i++) {
			cb.Stamps[i] = { input.stamps[i].x, input.stamps[i].y, input.stamps[i].depth, input.stamps[i].radius };
			cb.StampEnds[i] = { input.stampEnds[i].x, input.stampEnds[i].y, 0.0f, 0.0f };
		}

		// 上传常量
		D3D11_MAPPED_SUBRESOURCE mapped{};
		// v569：Map HRESULT 检查——设备丢失等场景 Map 失败 → mapped.pData 空 → memcpy 空指针崩
		if (FAILED(context->Map(perFrameCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return nullptr;
		std::memcpy(mapped.pData, &cb, sizeof(cb));
		context->Unmap(perFrameCB.Get(), 0);

		// ping-pong：读旧、写新
		uint32_t prev = currentTexture;
		uint32_t next = 1 - currentTexture;

		ID3D11Buffer* cbs[] = { perFrameCB.Get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		ID3D11ShaderResourceView* srvs[] = { textures[prev].srv.Get() };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11UnorderedAccessView* uavs[] = { textures[next].uav.Get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->CSSetShader(updateCS.Get(), nullptr, 0);
		context->Dispatch(kTextureDim / 8, kTextureDim / 8, 1);

		// 解绑
		ID3D11ShaderResourceView* nullSRV[] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		ID3D11Buffer* nullCB[] = { nullptr };
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);

		currentTexture = next;
		return textures[currentTexture].srv.Get();
	}

}
