// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string_view>

namespace SnowDeform
{
	// 变形图核心常量（与 CS SnowDeformation.h 对齐）
	constexpr uint32_t kTextureDim = 2048;    // 变形图分辨率
	constexpr float kWorldSize = 14000.0f;    // 变形窗口世界尺寸（与 CS 默认一致）
	constexpr float kTexelSize = kWorldSize / static_cast<float>(kTextureDim);
	constexpr uint32_t kMaxStamps = 128;      // 每帧最大印章数

	// 单个印章：world XY 位置 + 深度 + 半径；StampEnds 提供胶囊线段起点
	struct Stamp
	{
		float x, y, depth, radius;
	};

	// 每帧 GPU 常量缓冲（布局需与 DeformationUpdateCS.hlsl 的 cbuffer 完全一致）
	struct PerFrameCB
	{
		float WindowOriginX;
		float WindowOriginY;
		int32_t ScrollDeltaX;
		int32_t ScrollDeltaY;

		float TexelSize;
		uint32_t StampCount;
		float RefillAmount;
		uint32_t ClearMap;

		Stamp Stamps[kMaxStamps];
		Stamp StampEnds[kMaxStamps];  // 只使用 x/y
	};
	static_assert(sizeof(PerFrameCB) % 16 == 0, "PerFrameCB must be 16-byte aligned");

	// 每帧 CPU 端输入（由 StampCollector 填充）
	struct FrameInput
	{
		float windowOriginX = 0.0f;
		float windowOriginY = 0.0f;
		int32_t scrollDeltaX = 0;
		int32_t scrollDeltaY = 0;
		float refillAmount = 0.0f;
		bool clearMap = true;  // 首帧需要清空
		uint32_t stampCount = 0;
		Stamp stamps[kMaxStamps]{};
		Stamp stampEnds[kMaxStamps]{};
	};

	// 变形图：滚动窗口 + 双缓冲 ping-pong 纹理 + 计算着色器更新
	class DeformationMap
	{
	public:
		DeformationMap() = default;
		~DeformationMap() = default;
		DeformationMap(const DeformationMap&) = delete;
		DeformationMap& operator=(const DeformationMap&) = delete;

		// 初始化 D3D11 资源（延迟到首个渲染帧调用，此时 device/context 可用）
		bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
		void Reset();

		// 每帧更新：滚动 + 回填 + 印章混合，返回当前最新 SRV（供后续 shader 采样）
		ID3D11ShaderResourceView* Update(const FrameInput& input);

		// 最新变形图的 SRV（绑定到需要采样的着色器阶段）
		ID3D11ShaderResourceView* GetCurrentSRV() const { return textures[currentTexture].srv.Get(); }

		// 调试：把当前变形图导出为 DDS 文件
		bool DebugExport(const std::string_view& path) const;

	private:
		bool CreateResources();
		bool CreateComputeShader();
		void ScrollAndRefill(const FrameInput& input);

		// D3D11 资源
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> updateCS;

		struct TexturePair
		{
			Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
			Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
		};
		TexturePair textures[2];
		uint32_t currentTexture = 0;

		Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB;

		// 窗口状态
		float windowOriginX = 0.0f;
		float windowOriginY = 0.0f;
		int32_t pendingScrollX = 0;
		int32_t pendingScrollY = 0;
		bool initialized = false;
	};

	// 全局单例访问（插件主入口持有）
	DeformationMap& GetDeformationMap();
}
