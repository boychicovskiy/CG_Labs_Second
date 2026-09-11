// Windows.h min/max macros collide with std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RenderingSystem.hpp"

#include <DirectXColors.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "d3dx12.h"            // UpdateSubresources
#include "DDSTextureLoader.h"

using namespace DirectX;

namespace {
	const std::wstring kObjPathW  = L"assets\\sponza.obj";
	const std::wstring kAssetDirW = L"assets\\";

	// diffuse, alpha, normal, displacement — по четыре SRV на материал
	const UINT kTexturesPerMaterial = 4;

	std::string WideToUtf8(const std::wstring& w)
	{
		if (w.empty()) return {};
		int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string s((n > 0) ? (n - 1) : 0, '\0');
		if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
		return s;
	}

	std::wstring Utf8ToWide(const std::string& s)
	{
		if (s.empty()) return {};
		int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
		std::wstring w((n > 0) ? (n - 1) : 0, L'\0');
		if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
		return w;
	}
}

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::Init(ID3D12Device* device,
                           ID3D12CommandQueue* cmdQueue,
                           ID3D12CommandAllocator* cmdAlloc,
                           ID3D12GraphicsCommandList* cmdList,
                           DXGI_FORMAT backBufferFormat,
                           DXGI_FORMAT depthStencilFormat)
{
	m_backBufferFormat   = backBufferFormat;
	m_depthStencilFormat = depthStencilFormat;

	m_srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_uploadFence)));

	BuildShaders();
	BuildGeometryRootSignature(device);
	BuildLightRootSignature(device);
	BuildGeometryPSO(device);
	BuildTessellationPSO(device);
	BuildLightPSO(device);
	BuildShadowRootSignature(device);
	BuildShadowPSO(device);

	m_gbuffer.Init(device);
	m_post.Init(device, m_backBufferFormat);   // формат RTV бэк-буфера (_SRGB)

	// ── ДЗ №5: каскадные карты теней ────────────────────────────────────────
	m_shadowMap.Init(device, /*size*/ 2048, /*cascades*/ ShadowMap::MaxCascades);
	// SRV массива каскадов кладём в кучу G-Buffer — слот t4
	m_gbuffer.SetShadowSrv(device, m_shadowMap.Resource(), m_shadowMap.CascadeCount());

	// ── Одноразовая заливка текстур и вершинного буфера ─────────────────────
	// uploadKeepAlive держит промежуточные UPLOAD-ресурсы живыми до тех пор,
	// пока GPU не выполнит команды копирования.
	std::vector<ComPtr<ID3D12Resource>> uploadKeepAlive;

	ThrowIfFailed(cmdAlloc->Reset());
	ThrowIfFailed(cmdList->Reset(cmdAlloc, nullptr));

	CreateWhiteTexture(device, cmdList, uploadKeepAlive);
	LoadScene(device, cmdList, uploadKeepAlive);

	FlushUploads(cmdQueue, cmdList);
	uploadKeepAlive.clear();   // GPU закончил — можно освобождать

	BuildMaterialSrvHeap(device);
	BuildLights();
	BuildConstantBuffers(device);

	// ── ДЗ №4: поле объектов вокруг сцены ───────────────────────────────────
	// Область заметно больше самой Sponza, чтобы за пределами пирамиды
	// видимости всегда оставалась заметная доля объектов — иначе выигрыш
	// от отсечения не на чем показывать.
	{
		const float halfX = 0.5f * (m_boundsMax.x - m_boundsMin.x) * m_modelScale;
		const float halfY = 0.5f * (m_boundsMax.y - m_boundsMin.y) * m_modelScale;
		const float halfZ = 0.5f * (m_boundsMax.z - m_boundsMin.z) * m_modelScale;

		AABB region;
		region.Min = { -2.5f * std::max(halfX, 0.5f), -1.0f * std::max(halfY, 0.5f), -2.5f * std::max(halfZ, 0.5f) };
		region.Max = { +2.5f * std::max(halfX, 0.5f), +2.5f * std::max(halfY, 0.5f), +2.5f * std::max(halfZ, 0.5f) };

		m_objectField.Init(device, region, /*objectCount*/ 4096, m_depthStencilFormat);

		// ── ДЗ №6: фонтан частиц у пола атриума ─────────────────────────────
		const DirectX::XMFLOAT3 emitter = { 0.0f, -halfY + 0.02f, 0.0f };
		const float sceneScale = 2.0f * std::max(halfY, 0.25f);

		m_particles.Init(device, emitter, sceneScale, m_depthStencilFormat);
	}
}

void RenderingSystem::FlushUploads(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd)
{
	ThrowIfFailed(cmd->Close());
	ID3D12CommandList* lists[] = { cmd };
	queue->ExecuteCommandLists(1, lists);

	++m_uploadFenceValue;
	ThrowIfFailed(queue->Signal(m_uploadFence.Get(), m_uploadFenceValue));

	if (m_uploadFence->GetCompletedValue() < m_uploadFenceValue)
	{
		HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		ThrowIfFailed(m_uploadFence->SetEventOnCompletion(m_uploadFenceValue, evt));
		WaitForSingleObject(evt, INFINITE);
		CloseHandle(evt);
	}
}

// ═════════════════════════════════════════════════════════════════════════════
// Шейдеры
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildShaders()
{
	const std::wstring geoFile   = L"shader\\GeometryPass.hlsl";
	const std::wstring lightFile = L"shader\\LightPass.hlsl";

	m_geoVS = CompileShader(geoFile, nullptr, "VS", "vs_5_1");
	m_geoPS = CompileShader(geoFile, nullptr, "PS", "ps_5_1");

	// Конвейер с тесселяцией: тот же файл, другие точки входа.
	// Пиксельный шейдер общий — G-Buffer у обоих путей одинаковый.
	m_geoVSTess = CompileShader(geoFile, nullptr, "VS_Tess", "vs_5_1");
	m_geoHS     = CompileShader(geoFile, nullptr, "HS",      "hs_5_1");
	m_geoDS     = CompileShader(geoFile, nullptr, "DS",      "ds_5_1");

	m_lightVS = CompileShader(lightFile, nullptr, "VS", "vs_5_1");
	m_lightPS = CompileShader(lightFile, nullptr, "PS", "ps_5_1");
	m_debugPS = CompileShader(lightFile, nullptr, "PS_Debug", "ps_5_1");

	// Проход карты теней — только вершинные шейдеры, пиксельного нет вовсе
	const std::wstring shadowFile = L"shader\\ShadowPass.hlsl";
	m_shadowVS          = CompileShader(shadowFile, nullptr, "VS",           "vs_5_1");
	m_shadowVSInstanced = CompileShader(shadowFile, nullptr, "VS_Instanced", "vs_5_1");
}

// ═════════════════════════════════════════════════════════════════════════════
// Root signature геометрического прохода
//   b0 — ObjectCB   (root CBV, VS)
//   b1 — GeoPassCB  (root CBV, VS)
//   b2 — MaterialCB (root CBV, ALL: VS берёт UV, PS — Kd/Ks/Ns)
//   t0..t1 — descriptor table: diffuse + alpha mask (PS)
//   s0 — статический сэмплер (лекция 02, слайд 41)
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildGeometryRootSignature(ID3D12Device* device)
{
	// t0 = diffuse, t1 = alpha, t2 = normal, t3 = displacement.
	// t3 читает domain shader, t0..t2 — пиксельный, поэтому видимость ALL.
	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors                    = kTexturesPerMaterial;
	srvRange.BaseShaderRegister                = 0;
	srvRange.RegisterSpace                     = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// Все константные буферы видимы всем стадиям: hull shader читает параметры
	// тесселяции из b1, domain shader — ViewProj из b1 и силу смещения из b2.
	D3D12_ROOT_PARAMETER params[4] = {};

	params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

	params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1;
	params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

	params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor.ShaderRegister = 2;
	params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

	params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges   = &srvRange;
	params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter           = D3D12_FILTER_ANISOTROPIC;
	sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias       = 0.0f;
	sampler.MaxAnisotropy    = 8;
	sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	sampler.MinLOD           = 0.0f;
	sampler.MaxLOD           = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister   = 0;
	sampler.RegisterSpace    = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // DS тоже сэмплирует

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters     = _countof(params);
	desc.pParameters       = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers   = &sampler;
	desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> blob, errors;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                         blob.GetAddressOf(), errors.GetAddressOf());
	if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
	ThrowIfFailed(hr);

	ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                          IID_PPV_ARGS(&m_geoRootSig)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Root signature светового прохода
//   b0 — LightPassCB (root CBV, ALL)
//   b1 — LightCB     (root CBV, PS) — меняется на каждый источник
//   t0..t3 — descriptor table: albedo, normal, material, depth (PS)
//   Сэмплер не нужен: читаем через Texture2D.Load (лекция 03, слайд 20)
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildLightRootSignature(ID3D12Device* device)
{
	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors                    = GBuffer::SrvCount;   // 4
	srvRange.BaseShaderRegister                = 0;
	srvRange.RegisterSpace                     = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER params[3] = {};

	params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

	params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1;
	params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

	params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges   = &srvRange;
	params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

	// Сэмплер сравнения для карты теней (слайд 40): фильтрация LINEAR даёт
	// аппаратный PCF 2×2 внутри одной выборки SampleCmp.
	D3D12_STATIC_SAMPLER_DESC shadowSampler = {};
	shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	shadowSampler.MipLODBias       = 0.0f;
	shadowSampler.MaxAnisotropy    = 1;
	// LESS_EQUAL: глубина пикселя не больше сохранённой → он освещён (1.0)
	shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	// Белая граница: всё за пределами карты считается освещённым
	shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	shadowSampler.MinLOD           = 0.0f;
	shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;
	shadowSampler.ShaderRegister   = 0;
	shadowSampler.RegisterSpace    = 0;
	shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters     = _countof(params);
	desc.pParameters       = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers   = &shadowSampler;
	// Input layout не нужен — вершины генерирует VS из SV_VertexID.
	desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> blob, errors;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                         blob.GetAddressOf(), errors.GetAddressOf());
	if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
	ThrowIfFailed(hr);

	ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                          IID_PPV_ARGS(&m_lightRootSig)));
}

// ═════════════════════════════════════════════════════════════════════════════
// PSO геометрического прохода: 3 render target'а (лекция 03, слайд 19)
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildGeometryPSO(ID3D12Device* device)
{
	D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode              = D3D12_FILL_MODE_SOLID;
	// NONE, а не BACK: в Sponza листва и ткани — односторонние полигоны,
	// при back-face culling половина из них исчезает.
	raster.CullMode              = D3D12_CULL_MODE_NONE;
	raster.FrontCounterClockwise = FALSE;
	raster.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
	raster.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	raster.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	raster.DepthClipEnable       = TRUE;
	raster.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_BLEND_DESC blend = {};
	blend.AlphaToCoverageEnable  = FALSE;
	blend.IndependentBlendEnable = FALSE;
	for (UINT i = 0; i < GBuffer::RT_Count; ++i)
	{
		blend.RenderTarget[i].BlendEnable           = FALSE;
		blend.RenderTarget[i].LogicOpEnable         = FALSE;
		blend.RenderTarget[i].SrcBlend              = D3D12_BLEND_ONE;
		blend.RenderTarget[i].DestBlend             = D3D12_BLEND_ZERO;
		blend.RenderTarget[i].BlendOp               = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[i].SrcBlendAlpha         = D3D12_BLEND_ONE;
		blend.RenderTarget[i].DestBlendAlpha        = D3D12_BLEND_ZERO;
		blend.RenderTarget[i].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[i].LogicOp               = D3D12_LOGIC_OP_NOOP;
		blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}

	D3D12_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable      = TRUE;
	ds.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
	ds.StencilEnable    = FALSE;
	ds.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
	ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	ds.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
	ds.BackFace = ds.FrontFace;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.InputLayout           = { inputLayout, _countof(inputLayout) };
	pso.pRootSignature        = m_geoRootSig.Get();
	pso.VS                    = { m_geoVS->GetBufferPointer(), m_geoVS->GetBufferSize() };
	pso.PS                    = { m_geoPS->GetBufferPointer(), m_geoPS->GetBufferSize() };
	pso.RasterizerState       = raster;
	pso.BlendState            = blend;
	pso.DepthStencilState     = ds;
	pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets      = GBuffer::RT_Count;
	for (UINT i = 0; i < GBuffer::RT_Count; ++i)
		pso.RTVFormats[i] = GBuffer::Format(i);
	pso.DSVFormat             = m_depthStencilFormat;
	pso.SampleDesc.Count      = 1;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_geoPSO)));

	// Каркасный вариант — нужен, чтобы видеть плотность сетки (слайд 67)
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_geoPSOWire)));
}

// ═════════════════════════════════════════════════════════════════════════════
// PSO с тесселяцией (лекция 04, слайды 24–31)
//
// Отличий от обычного PSO ровно три:
//   1. заполнены поля HS и DS
//   2. VS другой — выдаёт мировые координаты, а не clip space
//   3. PrimitiveTopologyType = PATCH, а не TRIANGLE
// Пиксельный шейдер, форматы render target'ов и depth-состояние те же.
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildTessellationPSO(ID3D12Device* device)
{
	D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode              = D3D12_FILL_MODE_SOLID;
	raster.CullMode              = D3D12_CULL_MODE_NONE;
	raster.FrontCounterClockwise = FALSE;
	raster.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
	raster.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	raster.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	raster.DepthClipEnable       = TRUE;
	raster.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_BLEND_DESC blend = {};
	for (UINT i = 0; i < GBuffer::RT_Count; ++i)
	{
		blend.RenderTarget[i].BlendEnable           = FALSE;
		blend.RenderTarget[i].SrcBlend              = D3D12_BLEND_ONE;
		blend.RenderTarget[i].DestBlend             = D3D12_BLEND_ZERO;
		blend.RenderTarget[i].BlendOp               = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[i].SrcBlendAlpha         = D3D12_BLEND_ONE;
		blend.RenderTarget[i].DestBlendAlpha        = D3D12_BLEND_ZERO;
		blend.RenderTarget[i].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[i].LogicOp               = D3D12_LOGIC_OP_NOOP;
		blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}

	D3D12_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable      = TRUE;
	ds.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
	ds.StencilEnable    = FALSE;
	ds.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
	ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	ds.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
	ds.BackFace = ds.FrontFace;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.InputLayout           = { inputLayout, _countof(inputLayout) };
	pso.pRootSignature        = m_geoRootSig.Get();
	pso.VS                    = { m_geoVSTess->GetBufferPointer(), m_geoVSTess->GetBufferSize() };
	pso.HS                    = { m_geoHS->GetBufferPointer(),     m_geoHS->GetBufferSize() };
	pso.DS                    = { m_geoDS->GetBufferPointer(),     m_geoDS->GetBufferSize() };
	pso.PS                    = { m_geoPS->GetBufferPointer(),     m_geoPS->GetBufferSize() };
	pso.RasterizerState       = raster;
	pso.BlendState            = blend;
	pso.DepthStencilState     = ds;
	pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
	// PATCH, а не TRIANGLE: на вход тесселятора приходят патчи из 3 контрольных
	// точек, поэтому и топология в IASetPrimitiveTopology будет PATCHLIST.
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
	pso.NumRenderTargets      = GBuffer::RT_Count;
	for (UINT i = 0; i < GBuffer::RT_Count; ++i)
		pso.RTVFormats[i] = GBuffer::Format(i);
	pso.DSVFormat             = m_depthStencilFormat;
	pso.SampleDesc.Count      = 1;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_tessPSO)));

	pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_tessPSOWire)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Root signature прохода карты теней: один root CBV, всё остальное запрещено
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildShadowRootSignature(ID3D12Device* device)
{
	D3D12_ROOT_PARAMETER param = {};
	param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	param.Descriptor.ShaderRegister = 0;
	param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 1;
	desc.pParameters   = &param;
	desc.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

	ComPtr<ID3DBlob> blob, errors;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                         blob.GetAddressOf(), errors.GetAddressOf());
	if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
	ThrowIfFailed(hr);

	ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                          IID_PPV_ARGS(&m_shadowRootSig)));
}

// ═════════════════════════════════════════════════════════════════════════════
// PSO прохода карты теней (лекция 06, слайды 14, 18–20)
//
// Ни одного render target'а и пустой пиксельный шейдер — пишется только
// глубина. Смещение задаётся состоянием растеризатора, а не в шейдере:
// SlopeScaledDepthBias учитывает наклон полигона, что и нужно против
// «shadow acne» на поверхностях под углом к свету.
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildShadowPSO(ID3D12Device* device)
{
	// Из всей вершины нужна только позиция; остальные поля просто пропускаем
	D3D12_INPUT_ELEMENT_DESC staticLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_INPUT_ELEMENT_DESC instancedLayout[] =
	{
		{ "POSITION",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
		{ "INSTCENTER", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0,
		  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTEXTENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
	};

	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode              = D3D12_FILL_MODE_SOLID;
	raster.CullMode              = D3D12_CULL_MODE_NONE;
	raster.FrontCounterClockwise = FALSE;
	// Постоянное смещение в единицах младшего бита буфера глубины
	raster.DepthBias             = 1200;
	// Ограничитель: на полигонах под острым углом формула наклона даёт
	// огромное смещение и тень «отрывается» от объекта (слайд 20)
	raster.DepthBiasClamp        = 0.01f;
	// Смещение, пропорциональное наклону — основное средство против acne
	raster.SlopeScaledDepthBias  = 1.8f;
	raster.DepthClipEnable       = TRUE;
	raster.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable      = TRUE;
	ds.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
	ds.StencilEnable    = FALSE;
	ds.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
	ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	ds.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
	ds.BackFace = ds.FrontFace;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.InputLayout           = { staticLayout, _countof(staticLayout) };
	pso.pRootSignature        = m_shadowRootSig.Get();
	pso.VS                    = { m_shadowVS->GetBufferPointer(), m_shadowVS->GetBufferSize() };
	pso.PS                    = { nullptr, 0 };          // пиксельный шейдер не нужен
	pso.RasterizerState       = raster;
	pso.BlendState            = D3D12_BLEND_DESC{};
	pso.DepthStencilState     = ds;
	pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets      = 0;                        // только глубина
	pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc.Count      = 1;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowPSO)));

	pso.InputLayout = { instancedLayout, _countof(instancedLayout) };
	pso.VS          = { m_shadowVSInstanced->GetBufferPointer(), m_shadowVSInstanced->GetBufferSize() };

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowPSOInstanced)));
}

// ═════════════════════════════════════════════════════════════════════════════
// PSO светового прохода: аддитивный блендинг, глубина выключена
// (лекция 03, слайды 11 и 13)
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildLightPSO(ID3D12Device* device)
{
	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode             = D3D12_FILL_MODE_SOLID;
	raster.CullMode             = D3D12_CULL_MODE_NONE;
	raster.DepthClipEnable      = TRUE;
	raster.ConservativeRaster   = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	// Накопление яркости: Final = Src * 1 + Dest * 1  (слайд 36)
	D3D12_BLEND_DESC additive = {};
	additive.RenderTarget[0].BlendEnable           = TRUE;
	additive.RenderTarget[0].LogicOpEnable         = FALSE;
	additive.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
	additive.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
	additive.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
	additive.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
	additive.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
	additive.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
	additive.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
	additive.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Depth-буфер сейчас привязан как SRV (t3), поэтому DSV не ставим вовсе.
	D3D12_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable   = FALSE;
	ds.StencilEnable = FALSE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.InputLayout           = { nullptr, 0 };
	pso.pRootSignature        = m_lightRootSig.Get();
	pso.VS                    = { m_lightVS->GetBufferPointer(), m_lightVS->GetBufferSize() };
	pso.PS                    = { m_lightPS->GetBufferPointer(), m_lightPS->GetBufferSize() };
	pso.RasterizerState       = raster;
	pso.BlendState            = additive;
	pso.DepthStencilState     = ds;
	pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets      = 1;
	pso.RTVFormats[0]         = PostProcess::HdrFormat;
	pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
	pso.SampleDesc.Count      = 1;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_lightPSO)));

	// Отладочный PSO: тот же VS, другой PS, блендинг выключен —
	// показывает содержимое одного таргета G-Buffer на весь экран.
	D3D12_BLEND_DESC opaque = {};
	opaque.RenderTarget[0].BlendEnable           = FALSE;
	opaque.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
	opaque.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
	opaque.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
	opaque.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
	opaque.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
	opaque.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
	opaque.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
	opaque.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	pso.PS         = { m_debugPS->GetBufferPointer(), m_debugPS->GetBufferSize() };
	pso.BlendState = opaque;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_debugPSO)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Константные буферы
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildConstantBuffers(ID3D12Device* device)
{
	m_objectCB    = std::make_unique<UploadBuffer<ObjectConstants>>(device, 1, true);
	m_geoPassCB   = std::make_unique<UploadBuffer<GeoPassConstants>>(device, 1, true);
	m_lightPassCB = std::make_unique<UploadBuffer<LightPassConstants>>(device, 1, true);

	const UINT matCount   = std::max<UINT>(1, static_cast<UINT>(m_materials.size()));
	const UINT lightCount = std::max<UINT>(1, static_cast<UINT>(m_lights.size()));

	m_materialCB = std::make_unique<UploadBuffer<MaterialConstants>>(device, matCount, true);
	m_lightCB    = std::make_unique<UploadBuffer<LightConstants>>(device, lightCount, true);
	m_shadowCB   = std::make_unique<UploadBuffer<ShadowPassConstants>>(device, ShadowMap::MaxCascades, true);
	m_shadowCBStride = CalcConstantBufferByteSize(sizeof(ShadowPassConstants));

	// Root CBV требует выравнивания адреса на 256 байт, поэтому шаг между
	// элементами — тот же, что использует UploadBuffer при isConstantBuffer.
	m_materialCBStride = CalcConstantBufferByteSize(sizeof(MaterialConstants));
	m_lightCBStride    = CalcConstantBufferByteSize(sizeof(LightConstants));

	// Свет статичен — заливаем один раз здесь, а в Update() только
	// перемножаем на m_lightIntensityScale.
	for (size_t i = 0; i < m_lights.size(); ++i)
		m_lightCB->CopyData(static_cast<int>(i), m_lights[i]);
}

// ═════════════════════════════════════════════════════════════════════════════
// Белая заглушка 1×1 — используется там, где у материала нет карты
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::CreateWhiteTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                         std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive)
{
	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width            = 1;
	texDesc.Height           = 1;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels        = 1;
	texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	ThrowIfFailed(device->CreateCommittedResource(
		&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_whiteTex)));

	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width            = uploadSize;
	bufDesc.Height           = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels        = 1;
	bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	ComPtr<ID3D12Resource> upload;
	ThrowIfFailed(device->CreateCommittedResource(
		&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

	static const uint8_t whitePixel[4] = { 255, 255, 255, 255 };

	D3D12_SUBRESOURCE_DATA sub = {};
	sub.pData      = whitePixel;
	sub.RowPitch   = 4;
	sub.SlicePitch = 4;

	UpdateSubresources(cmd, m_whiteTex.Get(), upload.Get(), 0, 0, 1, &sub);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = m_whiteTex.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &barrier);

	uploadKeepAlive.push_back(upload);
}

// ═════════════════════════════════════════════════════════════════════════════
// Загрузка DDS с кэшем: одна и та же текстура в .mtl встречается у нескольких
// материалов, ресурс создаём один раз (SRV — по одному на материал, они дёшевы)
// ═════════════════════════════════════════════════════════════════════════════
ComPtr<ID3D12Resource> RenderingSystem::LoadTextureCached(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmd,
	const std::wstring& path,
	std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive)
{
	auto it = m_textureCache.find(path);
	if (it != m_textureCache.end())
		return it->second;

	ComPtr<ID3D12Resource> texture;
	ComPtr<ID3D12Resource> uploadHeap;

	HRESULT hr = DirectX::CreateDDSTextureFromFile12(
		device, cmd, path.c_str(), texture, uploadHeap);

	if (FAILED(hr) || !texture)
	{
		OutputDebugStringW((L"[TEX] FAILED (fallback white): " + path + L"\n").c_str());
		m_textureCache[path] = nullptr;
		return nullptr;
	}

	uploadKeepAlive.push_back(uploadHeap);
	m_textureCache[path] = texture;

#if defined(_DEBUG)
	OutputDebugStringW((L"[TEX] Loaded: " + path + L"\n").c_str());
#endif

	return texture;
}

// ═════════════════════════════════════════════════════════════════════════════
// Загрузка сцены: OBJ + материалы + текстуры + вершинный буфер
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::LoadScene(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive)
{
	// Материал по умолчанию всегда есть — слот 0
	{
		Material def;
		def.name = "<default>";
		m_materials.push_back(def);
	}

	const std::string objPath = WideToUtf8(kObjPathW);
	const std::string baseDir = WideToUtf8(kAssetDirW);

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t>    shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn, err;

	bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
	                           objPath.c_str(), baseDir.c_str(), /*triangulate*/ true);

	if (!warn.empty()) OutputDebugStringA(("[tinyobj warn] " + warn + "\n").c_str());
	if (!err.empty())  OutputDebugStringA(("[tinyobj err ] " + err  + "\n").c_str());

	if (!ok)
	{
		wchar_t absPath[MAX_PATH] = {};
		GetFullPathNameW(kObjPathW.c_str(), MAX_PATH, absPath, nullptr);

		std::wstring msg =
			L"Не удалось загрузить OBJ-модель.\n\nОжидаемый путь:\n  " + std::wstring(absPath) +
			L"\n\nПроверьте, что рабочая папка отладчика = папка проекта\n"
			L"и рядом лежит assets\\ (sponza.obj, sponza.mtl, textures\\*.dds).";

		MessageBoxW(nullptr, msg.c_str(), L"Ресурсы не найдены", MB_OK | MB_ICONWARNING);
		return;
	}

	// ── Материалы: константы из .mtl + текстуры ─────────────────────────────
	m_materials.reserve(materials.size() + 1);

	for (const auto& m : materials)
	{
		Material mat;
		mat.name = m.name;

		// ── Лаба 8: перевод .mtl в metallic workflow ────────────────────────
		// В формате Wavefront параметров PBR нет вообще: есть Kd (альбедо),
		// Ks (цвет блика) и Ns (экспонента Фонга). Лекция 09 (слайд 33)
		// требует пару metallic/roughness, поэтому их надо вывести.
		//
		// Kd переходит в базовый цвет без изменений.
		mat.constants.DiffuseAlbedo = { m.diffuse[0],  m.diffuse[1],  m.diffuse[2], 1.0f };

		// Ns -> roughness. Обратная к «Phong-аппроксимации» Бринна:
		// экспонента Ns соответствует alpha = sqrt(2 / (Ns + 2)),
		// а alpha в GGX и есть roughness^2, поэтому
		//     roughness = (2 / (Ns + 2))^(1/4).
		// Нижняя граница 0.05 нужна, чтобы знаменатель NDF не взрывался:
		// при roughness == 0 распределение микрофасетов вырождается в дельту
		// (слайды 4-5 — чем глаже поверхность, тем уже пучок микрофасетов).
		{
			const float ns    = (m.shininess > 1.0f) ? m.shininess : 1.0f;
			const float alpha = std::sqrt(2.0f / (ns + 2.0f));
			float rough = std::sqrt(alpha);
			rough = std::min(1.0f, std::max(0.05f, rough));

			// Ks здесь не цвет отражения, а лишь признак: у металлов в Sponza
			// он высокий и нейтральный. Настоящего канала metallic в .mtl нет,
			// поэтому металлы опознаём по имени материала — их всего несколько.
			const float ks = (m.specular[0] + m.specular[1] + m.specular[2]) / 3.0f;

			auto nameHas = [&](const char* what) {
				return m.name.find(what) != std::string::npos;
			};

			const bool isMetal = nameHas("chain") || nameHas("Chain") ||
			                     nameHas("flagpole") || nameHas("Flagpole") ||
			                     nameHas("metal") || nameHas("Metal");

			mat.constants.Metallic  = isMetal ? 1.0f : 0.0f;
			mat.constants.Roughness = isMetal ? std::min(rough, 0.35f) : rough;

			// У диэлектриков с почти нулевым Ks блика в оригинале не было —
			// делаем их заведомо шероховатыми, иначе штукатурка начнёт бликовать.
			if (!isMetal && ks < 0.02f)
				mat.constants.Roughness = std::max(mat.constants.Roughness, 0.85f);

			// Карты AO в этой сборке Sponza нет, затенение считает световой проход.
			mat.constants.AmbientOcclusion = 1.0f;
		}

		if (!m.diffuse_texname.empty())
			mat.diffuseTex = LoadTextureCached(device, cmd,
				Utf8ToWide(baseDir + m.diffuse_texname), uploadKeepAlive);

		// map_d — маска прозрачности (листва, цепи, растения в Sponza)
		if (!m.alpha_texname.empty())
		{
			mat.alphaTex = LoadTextureCached(device, cmd,
				Utf8ToWide(baseDir + m.alpha_texname), uploadKeepAlive);
			mat.constants.AlphaTest = mat.alphaTex ? 1u : 0u;
		}

		// ── ДЗ №3: карта смещения и карта нормалей ──────────────────────────
		// В этой сборке Sponza файлы *_bump.dds — полутоновые карты высот
		// (R == G == B), а не карты нормалей. Значит их можно использовать
		// как displacement напрямую, а карту нормалей взять из парного файла
		// *_nrm.dds, сгенерированного из той же карты высот. Слайд 38 лекции
		// именно это и советует: карта нормалей должна быть согласована
		// с картой смещения.
		if (!m.bump_texname.empty())
		{
			const std::string bumpName = m.bump_texname;

			mat.dispTex = LoadTextureCached(device, cmd,
				Utf8ToWide(baseDir + bumpName), uploadKeepAlive);
			mat.constants.HasDisplacement = mat.dispTex ? 1u : 0u;

			// textures\foo_bump.dds → textures\foo_nrm.dds
			const std::string suffix = "_bump.dds";
			if (bumpName.size() > suffix.size() &&
			    bumpName.compare(bumpName.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				std::string nrmName = bumpName.substr(0, bumpName.size() - suffix.size()) + "_nrm.dds";

				mat.normalTex = LoadTextureCached(device, cmd,
					Utf8ToWide(baseDir + nrmName), uploadKeepAlive);
				mat.constants.HasNormalMap = mat.normalTex ? 1u : 0u;
			}
		}

		m_materials.push_back(std::move(mat));
	}

	// ── Вершины, сгруппированные по material_id ─────────────────────────────
	const bool hasNormals  = !attrib.normals.empty();
	const bool hasTexcoord = !attrib.texcoords.empty();

	std::unordered_map<int, std::vector<Vertex>> groups;

	XMFLOAT3 minP = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
	XMFLOAT3 maxP = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

	auto ExpandBounds = [&](const XMFLOAT3& p) {
		minP.x = std::min(minP.x, p.x); maxP.x = std::max(maxP.x, p.x);
		minP.y = std::min(minP.y, p.y); maxP.y = std::max(maxP.y, p.y);
		minP.z = std::min(minP.z, p.z); maxP.z = std::max(maxP.z, p.z);
	};

	auto ReadPos = [&](int vi) -> XMFLOAT3 {
		if (vi < 0) return { 0, 0, 0 };
		return { attrib.vertices[3 * vi + 0], attrib.vertices[3 * vi + 1], attrib.vertices[3 * vi + 2] };
	};
	auto ReadNrm = [&](int ni) -> XMFLOAT3 {
		if (!hasNormals || ni < 0) return { 0, 1, 0 };
		return { attrib.normals[3 * ni + 0], attrib.normals[3 * ni + 1], attrib.normals[3 * ni + 2] };
	};
	auto ReadUV = [&](int ti) -> XMFLOAT2 {
		if (!hasTexcoord || ti < 0) return { 0.0f, 0.0f };
		// V переворачиваем: в OBJ ось V идёт снизу вверх, в DirectX — сверху вниз
		return { attrib.texcoords[2 * ti + 0], 1.0f - attrib.texcoords[2 * ti + 1] };
	};

	for (const auto& sh : shapes)
	{
		size_t indexOffset = 0;

		for (size_t f = 0; f < sh.mesh.num_face_vertices.size(); ++f)
		{
			int fv = sh.mesh.num_face_vertices[f];
			if (fv != 3) { indexOffset += static_cast<size_t>(fv); continue; }

			int matId = (f < sh.mesh.material_ids.size()) ? sh.mesh.material_ids[f] : -1;

			tinyobj::index_t i0 = sh.mesh.indices[indexOffset + 0];
			tinyobj::index_t i1 = sh.mesh.indices[indexOffset + 1];
			tinyobj::index_t i2 = sh.mesh.indices[indexOffset + 2];

			XMFLOAT3 p0 = ReadPos(i0.vertex_index);
			XMFLOAT3 p1 = ReadPos(i1.vertex_index);
			XMFLOAT3 p2 = ReadPos(i2.vertex_index);

			XMFLOAT3 n0 = ReadNrm(i0.normal_index);
			XMFLOAT3 n1 = ReadNrm(i1.normal_index);
			XMFLOAT3 n2 = ReadNrm(i2.normal_index);

			if (!hasNormals || i0.normal_index < 0 || i1.normal_index < 0 || i2.normal_index < 0)
			{
				XMVECTOR A = XMLoadFloat3(&p0), B = XMLoadFloat3(&p1), C = XMLoadFloat3(&p2);
				XMVECTOR fn = XMVector3Normalize(XMVector3Cross(B - A, C - A));
				XMStoreFloat3(&n0, fn); n1 = n0; n2 = n0;
			}

			XMFLOAT2 uv0 = ReadUV(i0.texcoord_index);
			XMFLOAT2 uv1 = ReadUV(i1.texcoord_index);
			XMFLOAT2 uv2 = ReadUV(i2.texcoord_index);

			// ── Касательная по треугольнику (лекция 04, слайд 9) ────────────
			// В OBJ тангентов нет, считаем их сами из рёбер и разностей UV.
			// Буфер невершинно-индексированный (triangle soup), поэтому одна
			// касательная присваивается всем трём вершинам грани. Плоское
			// касательное пространство не страшно: в пиксельном шейдере оно
			// ортогонализуется по Граму-Шмидту относительно гладкой нормали.
			XMFLOAT4 tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
			{
				XMFLOAT3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
				XMFLOAT3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };

				float du1 = uv1.x - uv0.x, dv1 = uv1.y - uv0.y;
				float du2 = uv2.x - uv0.x, dv2 = uv2.y - uv0.y;

				float det = du1 * dv2 - du2 * dv1;

				if (fabsf(det) > 1e-12f)
				{
					float f = 1.0f / det;

					XMFLOAT3 T = {
						f * (dv2 * e1.x - dv1 * e2.x),
						f * (dv2 * e1.y - dv1 * e2.y),
						f * (dv2 * e1.z - dv1 * e2.z)
					};
					XMFLOAT3 B = {
						f * (-du2 * e1.x + du1 * e2.x),
						f * (-du2 * e1.y + du1 * e2.y),
						f * (-du2 * e1.z + du1 * e2.z)
					};

					XMVECTOR Tv = XMLoadFloat3(&T);
					XMVECTOR Bv = XMLoadFloat3(&B);
					XMVECTOR Nv = XMLoadFloat3(&n0);

					if (XMVectorGetX(XMVector3LengthSq(Tv)) > 1e-20f)
					{
						Tv = XMVector3Normalize(Tv);

						// Хиральность: если N×T смотрит против B, базис зеркальный
						float handedness =
							(XMVectorGetX(XMVector3Dot(XMVector3Cross(Nv, Tv), Bv)) < 0.0f) ? -1.0f : 1.0f;

						XMFLOAT3 Tn;
						XMStoreFloat3(&Tn, Tv);
						tangent = { Tn.x, Tn.y, Tn.z, handedness };
					}
				}
			}

			auto& g = groups[matId];
			g.push_back(Vertex{ p0, n0, tangent, uv0 });
			g.push_back(Vertex{ p1, n1, tangent, uv1 });
			g.push_back(Vertex{ p2, n2, tangent, uv2 });

			ExpandBounds(p0); ExpandBounds(p1); ExpandBounds(p2);
			indexOffset += 3;
		}
	}

	// ── Один общий VB + список подмешей ─────────────────────────────────────
	std::vector<Vertex> vertices;
	vertices.reserve(400000);
	m_subMeshes.clear();

	for (auto& kv : groups)
	{
		SubMesh sm;
		sm.vertexOffset = static_cast<UINT>(vertices.size());
		sm.vertexCount  = static_cast<UINT>(kv.second.size());
		// material_id из tinyobj → слот в m_materials (0 занят дефолтным)
		sm.materialSlot = (kv.first >= 0 && kv.first < static_cast<int>(materials.size()))
			? static_cast<UINT>(kv.first + 1) : 0u;

		// Тесселируем только каменную геометрию: материал с картой высот
		// и без альфа-маски. Листва и цепи (leaf, chain) имеют map_bump,
		// но это плоские билборды с вырезанной альфой — смещать их бессмысленно.
		{
			const MaterialConstants& mc = m_materials[sm.materialSlot].constants;
			sm.tessellated = (mc.HasDisplacement != 0) && (mc.AlphaTest == 0);
		}

		m_subMeshes.push_back(sm);
		vertices.insert(vertices.end(), kv.second.begin(), kv.second.end());
	}

	if (vertices.empty())
	{
		OutputDebugStringA("[SCENE] OBJ loaded but produced 0 vertices.\n");
		return;
	}

	m_boundsMin = minP;
	m_boundsMax = maxP;

	m_modelCenter = { 0.5f * (minP.x + maxP.x), 0.5f * (minP.y + maxP.y), 0.5f * (minP.z + maxP.z) };
	{
		float dx = maxP.x - minP.x, dy = maxP.y - minP.y, dz = maxP.z - minP.z;
		float maxDim = std::max(dx, std::max(dy, dz));
		m_modelScale = (maxDim > 1e-6f) ? (2.0f / maxDim) : 1.0f;
	}

	// Вершинный буфер в UPLOAD-куче: модель статична, upload-heap проще
	// и не требует второго копирования (для лабораторной этого достаточно).
	m_modelVertexCount = static_cast<UINT>(vertices.size());
	const UINT vbByteSize = m_modelVertexCount * sizeof(Vertex);

	D3D12_RESOURCE_DESC vbDesc = {};
	vbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	vbDesc.Width            = vbByteSize;
	vbDesc.Height           = 1;
	vbDesc.DepthOrArraySize = 1;
	vbDesc.MipLevels        = 1;
	vbDesc.Format           = DXGI_FORMAT_UNKNOWN;
	vbDesc.SampleDesc.Count = 1;
	vbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES uploadProps = {};
	uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

	ThrowIfFailed(device->CreateCommittedResource(
		&uploadProps, D3D12_HEAP_FLAG_NONE, &vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_modelVB)));

	void* mapped = nullptr;
	ThrowIfFailed(m_modelVB->Map(0, nullptr, &mapped));
	memcpy(mapped, vertices.data(), vbByteSize);
	m_modelVB->Unmap(0, nullptr);

	m_modelVBV.BufferLocation = m_modelVB->GetGPUVirtualAddress();
	m_modelVBV.StrideInBytes  = sizeof(Vertex);
	m_modelVBV.SizeInBytes    = vbByteSize;
}

// ═════════════════════════════════════════════════════════════════════════════
// SRV-куча материалов: по 2 дескриптора (t0 diffuse, t1 alpha) на материал
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildMaterialSrvHeap(ID3D12Device* device)
{
	const UINT slots = static_cast<UINT>(m_materials.size());
	if (slots == 0) return;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = slots * kTexturesPerMaterial;
	heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_matSrvHeap)));

	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_matSrvHeap->GetCPUDescriptorHandleForHeapStart();

	// Диффузные карты хранят ЦВЕТ и уже гамма-закодированы, поэтому читать их
	// надо через формат _SRGB — аппаратура переведёт выборку в линейное
	// пространство. Карты нормалей, высот и альфа-маски цветом не являются,
	// к ним гамма-преобразование применять нельзя (лекция 08.1, слайд 42).
	auto ToSrgb = [](DXGI_FORMAT f) -> DXGI_FORMAT
	{
		switch (f)
		{
		case DXGI_FORMAT_BC1_UNORM:      return DXGI_FORMAT_BC1_UNORM_SRGB;
		case DXGI_FORMAT_BC2_UNORM:      return DXGI_FORMAT_BC2_UNORM_SRGB;
		case DXGI_FORMAT_BC3_UNORM:      return DXGI_FORMAT_BC3_UNORM_SRGB;
		case DXGI_FORMAT_BC7_UNORM:      return DXGI_FORMAT_BC7_UNORM_SRGB;
		case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		default:                         return f;   // формат без sRGB-варианта
		}
	};

	auto MakeSrv = [&](ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE h, bool srgb = false)
	{
		ID3D12Resource* target = res ? res : m_whiteTex.Get();
		D3D12_RESOURCE_DESC desc = target->GetDesc();

		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Format                        = srgb ? ToSrgb(desc.Format) : desc.Format;
		srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MostDetailedMip     = 0;
		srv.Texture2D.MipLevels           = desc.MipLevels;
		srv.Texture2D.PlaneSlice          = 0;
		srv.Texture2D.ResourceMinLODClamp = 0.0f;

		device->CreateShaderResourceView(target, &srv, h);
	};

	// Порядок внутри четвёрки совпадает с регистрами t0..t3 в шейдере
	for (const auto& mat : m_materials)
	{
		MakeSrv(mat.diffuseTex.Get(), handle, /*srgb*/ true);  handle.ptr += m_srvDescSize;  // t0
		MakeSrv(mat.alphaTex.Get(),   handle);  handle.ptr += m_srvDescSize;  // t1
		MakeSrv(mat.normalTex.Get(),  handle);  handle.ptr += m_srvDescSize;  // t2
		MakeSrv(mat.dispTex.Get(),    handle);  handle.ptr += m_srvDescSize;  // t3
	}
}

// ═════════════════════════════════════════════════════════════════════════════
// Источники света: 1 ambient + 1 directional + 5 point + 2 spot
//
// Позиции задаются относительно габаритов модели ПОСЛЕ нормализации
// (world = Translate(-center) * Scale(2/maxDim)), поэтому не зависят от того,
// в каких единицах экспортирован obj.
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::BuildLights()
{
	m_lights.clear();

	const float halfX = 0.5f * (m_boundsMax.x - m_boundsMin.x) * m_modelScale;
	const float halfY = 0.5f * (m_boundsMax.y - m_boundsMin.y) * m_modelScale;
	const float halfZ = 0.5f * (m_boundsMax.z - m_boundsMin.z) * m_modelScale;

	// 1) Ambient — отдельный «источник» (лекция 03, слайд 13):
	//    ambient нельзя прибавлять на каждый свет, иначе сцена пересветится.
	{
		LightConstants l;
		l.Type      = static_cast<uint32_t>(LightType::Ambient);
		l.Color     = { 1.0f, 1.0f, 1.0f };
		l.Intensity = 1.0f;
		m_lights.push_back(l);
	}

	// 2) Directional — «солнце» сверху-сбоку
	{
		LightConstants l;
		l.Type      = static_cast<uint32_t>(LightType::Directional);
		l.Color     = { 1.0f, 0.96f, 0.88f };
		l.Intensity = 0.55f;

		XMVECTOR d = XMVector3Normalize(XMVectorSet(0.45f, -1.0f, 0.30f, 0.0f));
		XMStoreFloat3(&l.DirectionW, d);

		m_lights.push_back(l);
	}

	// 3) Точечные — цепочка вдоль длинной оси атриума, чуть выше пола
	{
		const XMFLOAT3 colors[5] = {
			{ 1.00f, 0.45f, 0.25f },   // тёплый оранжевый
			{ 0.30f, 0.65f, 1.00f },   // холодный синий
			{ 1.00f, 0.90f, 0.55f },   // жёлтый
			{ 0.40f, 1.00f, 0.55f },   // зелёный
			{ 1.00f, 0.35f, 0.70f },   // розовый
		};

		for (int i = 0; i < 5; ++i)
		{
			float t = (i / 4.0f) * 2.0f - 1.0f;    // -1 .. +1

			LightConstants l;
			l.Type      = static_cast<uint32_t>(LightType::Point);
			l.Color     = colors[i];
			l.Intensity = 2.2f;
			l.PositionW = { t * 0.75f * halfX,
			                -halfY + 0.30f * (2.0f * halfY),
			                0.0f };
			l.Range     = 0.55f * halfY + 0.25f * halfZ;

			m_lights.push_back(l);
		}
	}

	// 4) Прожекторы — светят вертикально вниз с уровня галереи
	{
		const float outerDeg = 32.0f;
		const float innerDeg = 18.0f;

		for (int i = 0; i < 2; ++i)
		{
			float sign = (i == 0) ? -1.0f : 1.0f;

			LightConstants l;
			l.Type         = static_cast<uint32_t>(LightType::Spot);
			l.Color        = { 1.0f, 1.0f, 1.0f };
			l.Intensity    = 4.0f;
			l.PositionW    = { sign * 0.35f * halfX, 0.75f * halfY, 0.0f };
			l.DirectionW   = { 0.0f, -1.0f, 0.0f };
			l.Range        = 2.0f * halfY;
			l.SpotCosOuter = cosf(XMConvertToRadians(outerDeg));
			l.SpotCosInner = cosf(XMConvertToRadians(innerDeg));

			m_lights.push_back(l);
		}
	}

#if defined(_DEBUG)
	char buf[128];
	sprintf_s(buf, "[LIGHTS] total = %zu (bounds half = %.3f %.3f %.3f)\n",
	          m_lights.size(), halfX, halfY, halfZ);
	OutputDebugStringA(buf);
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// OnResize
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::OnResize(ID3D12Device* device, UINT width, UINT height,
                               ID3D12Resource* depthBuffer)
{
	m_gbuffer.Resize(device, width, height, depthBuffer);
	m_post.Resize(device, width, height);
}

// ═════════════════════════════════════════════════════════════════════════════
// Update — заполнение константных буферов
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::Update(double dt,
                             const XMMATRIX& view,
                             const XMMATRIX& proj,
                             const XMFLOAT3& eyePos,
                             UINT width, UINT height,
                             float fovY, float aspect, float nearZ, float farZ)
{
	if (!m_objectCB) return;

	// ── UV-анимация (перенос из ДЗ №1) ──────────────────────────────────────
	if (m_uvAnimEnabled)
	{
		m_uvOffset.x += m_uvAnimSpeed * static_cast<float>(dt);
		if (m_uvOffset.x > 1.0f) m_uvOffset.x -= 1.0f;
	}

	// ── ObjectCB ────────────────────────────────────────────────────────────
	XMMATRIX world =
		XMMatrixTranslation(-m_modelCenter.x, -m_modelCenter.y, -m_modelCenter.z) *
		XMMatrixScaling(m_modelScale, m_modelScale, m_modelScale);

	ObjectConstants obj;
	XMStoreFloat4x4(&obj.World, XMMatrixTranspose(world));

	// Нормали преобразуются матрицей (W^-1)^T. HLSL при упаковке cbuffer
	// читает матрицу column-major, поэтому в буфер кладём её транспонированной:
	// transpose((W^-1)^T) == W^-1. То есть достаточно просто inverse(world).
	XMStoreFloat4x4(&obj.WorldInvTranspose, XMMatrixInverse(nullptr, world));

	m_objectCB->CopyData(0, obj);

	// ── GeoPassCB ───────────────────────────────────────────────────────────
	XMMATRIX viewProj = view * proj;

	GeoPassConstants geoPass;
	XMStoreFloat4x4(&geoPass.ViewProj, XMMatrixTranspose(viewProj));

	// Параметры тесселяции: hull shader считает по ним коэффициенты рёбер,
	// domain shader — величину смещения.
	geoPass.EyePosW           = eyePos;
	geoPass.TessFactorMax     = m_tessFactorMax;
	geoPass.TessFactorMin     = 1.0f;
	geoPass.TessDistNear      = m_tessDistNear;
	geoPass.TessDistFar       = m_tessDistFar;
	geoPass.DisplacementScale = m_displacementScale;
	geoPass.NormalMapEnabled  = m_normalMapEnabled ? 1u : 0u;
	geoPass.FlipGreenChannel  = m_flipGreenChannel ? 1u : 0u;
	geoPass.BackfaceCullHS    = m_backfaceCullHS   ? 1u : 0u;

	// Лаба 8: глобальный множитель шероховатости — клавиши O / L.
	// Позволяет прямо в кадре увидеть ряд «зеркало → матовое», о котором
	// говорит слайд 41 лекции 09.
	geoPass.RoughnessScale    = m_roughnessScale;

	m_geoPassCB->CopyData(0, geoPass);

	// ── MaterialCB: тайлинг/смещение общие, остальное — из .mtl ─────────────
	for (size_t i = 0; i < m_materials.size(); ++i)
	{
		MaterialConstants mc = m_materials[i].constants;
		mc.UvScale  = m_uvTile;
		mc.UvOffset = m_uvOffset;
		m_materialCB->CopyData(static_cast<int>(i), mc);
	}

	// ── Каскады теней ───────────────────────────────────────────────────────
	// Направленный источник — единственный, который отбрасывает тени.
	XMFLOAT3 sunDir = { 0.45f, -1.0f, 0.30f };
	for (const LightConstants& l : m_lights)
	{
		if (l.Type == static_cast<uint32_t>(LightType::Directional))
		{
			sunDir = l.DirectionW;
			break;
		}
	}

	// Каскады строим не на всю дальнюю плоскость: сцена занимает считанные
	// единицы, и растягивать карты теней на 100 единиц — терять разрешение.
	const float shadowFar = std::min(farZ, 15.0f);
	m_shadowMap.UpdateCascades(view, fovY, aspect, nearZ, shadowFar, sunDir);

	// Константы прохода карты теней: по элементу на каскад
	{
		XMMATRIX world =
			XMMatrixTranslation(-m_modelCenter.x, -m_modelCenter.y, -m_modelCenter.z) *
			XMMatrixScaling(m_modelScale, m_modelScale, m_modelScale);

		for (UINT c = 0; c < m_shadowMap.CascadeCount(); ++c)
		{
			XMMATRIX lvp = XMLoadFloat4x4(&m_shadowMap.LightViewProj(c));

			ShadowPassConstants sc;
			XMStoreFloat4x4(&sc.LightViewProj, XMMatrixTranspose(lvp));
			XMStoreFloat4x4(&sc.World,         XMMatrixTranspose(world));
			m_shadowCB->CopyData(static_cast<int>(c), sc);
		}
	}

	// ── LightPassCB ─────────────────────────────────────────────────────────
	LightPassConstants lp;
	XMStoreFloat4x4(&lp.InvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));
	lp.EyePosW       = eyePos;
	lp.InvScreenSize = { 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height) };
	lp.DebugMode     = m_debugMode;
	lp.IblEnabled    = m_iblEnabled ? 1u : 0u;

	// Лаба 8: ambient больше не «плоская добавка» вида float3(0.03)*albedo
	// (слайд 53), а цвет неба, от которого световой проход строит
	// аналитическое окружение: облучённость + отражение (слайды 53-56, 80).
	lp.AmbientColor  = { 0.32f, 0.42f, 0.62f, 1.0f };

	XMStoreFloat4x4(&lp.View, XMMatrixTranspose(view));
	for (UINT c = 0; c < ShadowMap::MaxCascades; ++c)
	{
		// ShadowMap хранит матрицы в «математическом» виде, для HLSL их надо
		// транспонировать — как и все остальные матрицы в константных буферах.
		XMMATRIX m = XMLoadFloat4x4(&m_shadowMap.LightViewProj(c));
		XMStoreFloat4x4(&lp.CascadeViewProj[c], XMMatrixTranspose(m));
	}
	lp.CascadeSplits   = m_shadowMap.SplitDistances();
	lp.ShadowsEnabled  = m_shadowsEnabled ? 1u : 0u;
	lp.ShowCascades    = m_showCascades   ? 1u : 0u;
	lp.ShadowBias      = m_shadowBias;
	lp.ShadowTexelSize = m_shadowMap.TexelSize();

	m_lightPassCB->CopyData(0, lp);

	// ── LightCB: перезаливаем только если менялась общая яркость ────────────
	for (size_t i = 0; i < m_lights.size(); ++i)
	{
		LightConstants lc = m_lights[i];
		lc.Intensity *= m_lightIntensityScale;
		m_lightCB->CopyData(static_cast<int>(i), lc);
	}

	// ── ДЗ №6: константы системы частиц ─────────────────────────────────────
	m_particles.Update(dt, view, viewProj);

	// ── ДЗ №4: отсечение поля объектов ──────────────────────────────────────
	// Запоминаем последнюю матрицу, чтобы было что «заморозить».
	XMStoreFloat4x4(&m_lastViewProj, viewProj);

	const XMMATRIX cullViewProj = m_freezeFrustum
		? XMLoadFloat4x4(&m_frozenViewProj)
		: viewProj;

	m_objectField.Update(viewProj, cullViewProj, m_cullMode);
}

// ═════════════════════════════════════════════════════════════════════════════
// RenderShadowPass — по проходу на каскад (лекция 06, слайд 30)
//
// Сцена рисуется столько раз, сколько каскадов, каждый раз со своей матрицей
// ViewProj света и в свой срез массива. Материалы и текстуры не нужны — важна
// только глубина.
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::RenderShadowPass(ID3D12GraphicsCommandList* cmd)
{
	if (!m_shadowsEnabled || !SceneLoaded() || !m_shadowCB)
	{
		// Даже если тени выключены, ресурс должен быть в состоянии для чтения:
		// световой проход всё равно привяжет его как SRV.
		m_shadowMap.TransitionToRead(cmd);
		return;
	}

	m_shadowMap.TransitionToWrite(cmd);

	// Viewport размером с КАРТУ, а не с окно (слайд 32)
	const D3D12_VIEWPORT vp = m_shadowMap.Viewport();
	const D3D12_RECT     sc = m_shadowMap.Scissor();
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &sc);

	cmd->SetGraphicsRootSignature(m_shadowRootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const D3D12_GPU_VIRTUAL_ADDRESS cbBase = m_shadowCB->Resource()->GetGPUVirtualAddress();

	for (UINT c = 0; c < m_shadowMap.CascadeCount(); ++c)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_shadowMap.Dsv(c);

		// Ни одного render target'а — пишем только глубину
		cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		cmd->SetGraphicsRootConstantBufferView(
			0, cbBase + static_cast<UINT64>(c) * m_shadowCBStride);

		// ── Sponza ──────────────────────────────────────────────────────────
		// Подмеши не разделяем: материалы для глубины не нужны, поэтому весь
		// вершинный буфер рисуется одним вызовом.
		cmd->SetPipelineState(m_shadowPSO.Get());
		cmd->IASetVertexBuffers(0, 1, &m_modelVBV);
		cmd->DrawInstanced(m_modelVertexCount, 1, 0, 0);

		// ── Поле коробок ────────────────────────────────────────────────────
		// Рисуем ВСЕ объекты, а не отобранные frustum culling'ом камеры:
		// объект вне пирамиды камеры может отбрасывать тень внутрь неё.
		if (m_objectField.TotalCount() > 0)
		{
			cmd->SetPipelineState(m_shadowPSOInstanced.Get());

			D3D12_VERTEX_BUFFER_VIEW views[2] = {
				m_objectField.CubeVBV(),
				m_objectField.AllInstancesVBV()
			};
			cmd->IASetVertexBuffers(0, 2, views);
			cmd->IASetIndexBuffer(&m_objectField.CubeIBV());

			cmd->DrawIndexedInstanced(m_objectField.CubeIndexCount(),
			                          m_objectField.TotalCount(), 0, 0, 0);
		}
	}

	m_shadowMap.TransitionToRead(cmd);
}

// ═════════════════════════════════════════════════════════════════════════════
// Render — три прохода
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::Render(ID3D12GraphicsCommandList* cmd,
                             D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
                             D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                             const D3D12_VIEWPORT& viewport,
                             const D3D12_RECT& scissor)
{
	// ═══════════════════════════════════════════════════════════════════════
	// ПРОХОД 0a: Compute → эмиссия и интегрирование частиц (ДЗ №6)
	// Обязательно до графических проходов: они читают результат.
	// ═══════════════════════════════════════════════════════════════════════
	m_particles.Simulate(cmd);

	// ═══════════════════════════════════════════════════════════════════════
	// ПРОХОД 0b: Shadow Pass → заполняем каскады карты теней
	// ═══════════════════════════════════════════════════════════════════════
	RenderShadowPass(cmd);

	cmd->RSSetViewports(1, &viewport);
	cmd->RSSetScissorRects(1, &scissor);

	// ═══════════════════════════════════════════════════════════════════════
	// ПРОХОД 1: Opaque Geometry Stage → заполняем G-Buffer
	// ═══════════════════════════════════════════════════════════════════════
	m_gbuffer.TransitionToWrite(cmd);

	D3D12_CPU_DESCRIPTOR_HANDLE gbufRtv = m_gbuffer.RtvStart();
	// TRUE = дескрипторы лежат подряд, достаточно передать первый handle
	cmd->OMSetRenderTargets(GBuffer::RT_Count, &gbufRtv, TRUE, &dsv);
	m_gbuffer.Clear(cmd, dsv);

	if (SceneLoaded())
	{
		cmd->SetGraphicsRootSignature(m_geoRootSig.Get());

		ID3D12DescriptorHeap* matHeaps[] = { m_matSrvHeap.Get() };
		cmd->SetDescriptorHeaps(1, matHeaps);

		cmd->SetGraphicsRootConstantBufferView(0, m_objectCB->Resource()->GetGPUVirtualAddress());
		cmd->SetGraphicsRootConstantBufferView(1, m_geoPassCB->Resource()->GetGPUVirtualAddress());

		cmd->IASetVertexBuffers(0, 1, &m_modelVBV);

		const D3D12_GPU_VIRTUAL_ADDRESS   matCbBase = m_materialCB->Resource()->GetGPUVirtualAddress();
		const D3D12_GPU_DESCRIPTOR_HANDLE srvBase   = m_matSrvHeap->GetGPUDescriptorHandleForHeapStart();

		// Один и тот же вершинный буфер рисуется двумя конвейерами, поэтому
		// подмеши идут двумя группами: смена топологии и PSO делается один раз
		// на группу, а не на каждый подмеш.
		auto DrawGroup = [&](bool tessellated)
		{
			if (tessellated)
			{
				cmd->SetPipelineState(m_wireframe ? m_tessPSOWire.Get() : m_tessPSO.Get());
				// 3 контрольные точки на патч — соответствует [domain("tri")]
				cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
			}
			else
			{
				cmd->SetPipelineState(m_wireframe ? m_geoPSOWire.Get() : m_geoPSO.Get());
				cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			}

			for (const auto& sm : m_subMeshes)
			{
				const bool useTess = sm.tessellated && m_tessEnabled;
				if (useTess != tessellated)
					continue;

				cmd->SetGraphicsRootConstantBufferView(
					2, matCbBase + static_cast<UINT64>(sm.materialSlot) * m_materialCBStride);

				D3D12_GPU_DESCRIPTOR_HANDLE srv = srvBase;
				srv.ptr += static_cast<UINT64>(sm.materialSlot) * kTexturesPerMaterial * m_srvDescSize;
				cmd->SetGraphicsRootDescriptorTable(3, srv);

				cmd->DrawInstanced(sm.vertexCount, 1, sm.vertexOffset, 0);
			}
		};

		DrawGroup(false);
		DrawGroup(true);
	}

	// Поле объектов пишется в тот же G-Buffer, поэтому освещается общими
	// источниками света. Оно меняет root signature и PSO, но световой проход
	// ниже всё равно ставит свои.
	m_objectField.Render(cmd);

	// Частицы непрозрачные и пишутся в тот же G-Buffer, поэтому их освещает
	// общий световой проход. Рисуем последними в проходе геометрии.
	m_particles.Render(cmd);

	// ═══════════════════════════════════════════════════════════════════════
	// ПРОХОД 2: Light Stage → аккумулируем освещение в back buffer
	// ═══════════════════════════════════════════════════════════════════════
	m_gbuffer.TransitionToRead(cmd);

	// Свет накапливается в HDR-буфере с плавающей точкой, а не в 8-битном
	// бэк-буфере: иначе яркие места обрежутся на единице (лекция 08.2, слайд 5).
	m_post.BeginScene(cmd);

	// DSV не привязываем: depth сейчас читается как SRV (t3)
	D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = m_post.HdrRtv();
	cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);

	cmd->SetGraphicsRootSignature(m_lightRootSig.Get());

	ID3D12DescriptorHeap* gbufHeaps[] = { m_gbuffer.SrvHeap() };
	cmd->SetDescriptorHeaps(1, gbufHeaps);

	cmd->SetGraphicsRootConstantBufferView(0, m_lightPassCB->Resource()->GetGPUVirtualAddress());
	cmd->SetGraphicsRootDescriptorTable(2, m_gbuffer.SrvGpuStart());

	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 1, nullptr);   // отвязываем VB прошлого прохода
	cmd->IASetIndexBuffer(nullptr);

	const D3D12_GPU_VIRTUAL_ADDRESS lightCbBase = m_lightCB->Resource()->GetGPUVirtualAddress();

	if (m_debugMode != 0)
	{
		// Отладочный режим: один fullscreen-треугольник, показываем таргет
		cmd->SetPipelineState(m_debugPSO.Get());
		cmd->SetGraphicsRootConstantBufferView(1, lightCbBase);
		cmd->DrawInstanced(3, 1, 0, 0);
	}
	else
	{
		cmd->SetPipelineState(m_lightPSO.Get());

		for (size_t i = 0; i < m_lights.size(); ++i)
		{
			cmd->SetGraphicsRootConstantBufferView(
				1, lightCbBase + static_cast<UINT64>(i) * m_lightCBStride);

			// Один треугольник на весь экран (лекция 03, слайд 23)
			cmd->DrawInstanced(3, 1, 0, 0);
		}
	}

	// ═══════════════════════════════════════════════════════════════════════
	// ПРОХОД 3: Постобработка (лаба 7) → HDR в бэк-буфер
	// ═══════════════════════════════════════════════════════════════════════
	// Отладочные виды G-Buffer показываем как есть, без тонального
	// отображения и свечения — иначе нормали и глубина исказятся.
	m_post.SetPassthrough(m_debugMode != 0);
	m_post.Execute(cmd, backBufferRtv, viewport, scissor);
}

// ═════════════════════════════════════════════════════════════════════════════
// Управление
// ═════════════════════════════════════════════════════════════════════════════
void RenderingSystem::ToggleUvAnimation()
{
	m_uvAnimEnabled = !m_uvAnimEnabled;
	OutputDebugStringA(m_uvAnimEnabled ? "[UV] Animation ON\n" : "[UV] Animation OFF\n");
}

void RenderingSystem::ScaleTiling(float factor)
{
	m_uvTile.x *= factor;
	m_uvTile.y *= factor;
}

void RenderingSystem::ResetUv()
{
	m_uvTile   = { 1.0f, 1.0f };
	m_uvOffset = { 0.0f, 0.0f };
	OutputDebugStringA("[UV] Reset tile=1 offset=0\n");
}

void RenderingSystem::SetDebugMode(uint32_t mode)
{
	m_debugMode = (m_debugMode == mode) ? 0u : mode;   // повторное нажатие выключает
}

void RenderingSystem::ScaleLightIntensity(float factor)
{
	m_lightIntensityScale *= factor;
	m_lightIntensityScale = std::max(0.05f, std::min(m_lightIntensityScale, 20.0f));
}

// ── ДЗ №3 ────────────────────────────────────────────────────────────────────
void RenderingSystem::ToggleTessellation()
{
	m_tessEnabled = !m_tessEnabled;
	OutputDebugStringA(m_tessEnabled ? "[TESS] ON\n" : "[TESS] OFF\n");
}

void RenderingSystem::ToggleWireframe()
{
	m_wireframe = !m_wireframe;
	OutputDebugStringA(m_wireframe ? "[TESS] Wireframe ON\n" : "[TESS] Wireframe OFF\n");
}

void RenderingSystem::ToggleNormalMapping()
{
	m_normalMapEnabled = !m_normalMapEnabled;
	OutputDebugStringA(m_normalMapEnabled ? "[NRM] Normal mapping ON\n" : "[NRM] Normal mapping OFF\n");
}

void RenderingSystem::ToggleGreenChannelFlip()
{
	m_flipGreenChannel = !m_flipGreenChannel;
	OutputDebugStringA(m_flipGreenChannel ? "[NRM] Green channel FLIPPED\n" : "[NRM] Green channel normal\n");
}

void RenderingSystem::ToggleHullBackfaceCulling()
{
	m_backfaceCullHS = !m_backfaceCullHS;
	OutputDebugStringA(m_backfaceCullHS ? "[TESS] HS backface culling ON\n" : "[TESS] HS backface culling OFF\n");
}

// ── Лаба 8: PBR ─────────────────────────────────────────────────────────────

void RenderingSystem::ScaleRoughness(float factor)
{
	m_roughnessScale *= factor;
	m_roughnessScale = std::max(0.05f, std::min(m_roughnessScale, 4.0f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[PBR] Roughness scale = %.2f\n", m_roughnessScale);
	OutputDebugStringA(buf);
#endif
}

void RenderingSystem::ToggleIbl()
{
	m_iblEnabled = !m_iblEnabled;
	OutputDebugStringA(m_iblEnabled ? "[PBR] IBL ON\n" : "[PBR] IBL OFF (flat ambient)\n");
}

void RenderingSystem::ScaleDisplacement(float factor)
{
	m_displacementScale *= factor;
	m_displacementScale = std::max(0.0f, std::min(m_displacementScale, 0.5f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[TESS] Displacement scale = %.4f\n", m_displacementScale);
	OutputDebugStringA(buf);
#endif
}

void RenderingSystem::SetCullMode(CullMode mode)
{
	m_cullMode = mode;

	const char* name =
		(mode == CullMode::Disabled)   ? "[CULL] OFF (draw everything)\n" :
		(mode == CullMode::BruteForce) ? "[CULL] Brute force frustum culling\n" :
		                                 "[CULL] Frustum culling + octree\n";
	OutputDebugStringA(name);
}

void RenderingSystem::ToggleShadows()
{
	m_shadowsEnabled = !m_shadowsEnabled;
	OutputDebugStringA(m_shadowsEnabled ? "[SHADOW] ON\n" : "[SHADOW] OFF\n");
}

void RenderingSystem::ToggleCascadeView()
{
	m_showCascades = !m_showCascades;
	OutputDebugStringA(m_showCascades ? "[SHADOW] Cascade tint ON\n" : "[SHADOW] Cascade tint OFF\n");
}

void RenderingSystem::ScaleShadowBias(float factor)
{
	m_shadowBias *= factor;
	m_shadowBias = std::max(0.0f, std::min(m_shadowBias, 0.05f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[SHADOW] Bias = %.5f\n", m_shadowBias);
	OutputDebugStringA(buf);
#endif
}

void RenderingSystem::ScaleCascadeLambda(float delta)
{
	m_shadowMap.SetLambda(m_shadowMap.Lambda() + delta);

#if defined(_DEBUG)
	char buf[80];
	sprintf_s(buf, "[SHADOW] Cascade lambda = %.2f (0 = uniform, 1 = logarithmic)\n",
	          m_shadowMap.Lambda());
	OutputDebugStringA(buf);
#endif
}

void RenderingSystem::ToggleFrustumFreeze()
{
	m_freezeFrustum = !m_freezeFrustum;

	if (m_freezeFrustum)
	{
		// Фиксируем пирамиду в текущем положении камеры
		m_frozenViewProj = m_lastViewProj;
		OutputDebugStringA("[CULL] Frustum FROZEN — fly away to see what was culled\n");
	}
	else
	{
		OutputDebugStringA("[CULL] Frustum follows camera again\n");
	}
}

void RenderingSystem::ScaleMaxTessFactor(float delta)
{
	// Верхняя граница совпадает с [maxtessfactor(8.0)] в hull shader'е
	m_tessFactorMax = std::max(1.0f, std::min(m_tessFactorMax + delta, 8.0f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[TESS] Max tess factor = %.1f\n", m_tessFactorMax);
	OutputDebugStringA(buf);
#endif
}
