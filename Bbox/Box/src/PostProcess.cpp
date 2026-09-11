// Windows.h min/max macros collide with std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "PostProcess.hpp"

#include <algorithm>
#include <cstdio>

using namespace DirectX;

namespace {

	void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
	                D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after)
	{
		if (!res || state == after) return;

		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = res;
		b.Transition.StateBefore = state;
		b.Transition.StateAfter  = after;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd->ResourceBarrier(1, &b);

		state = after;
	}

	const float kClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::Init(ID3D12Device* device, DXGI_FORMAT backBufferRtvFormat)
{
	m_backBufferRtvFormat = backBufferRtvFormat;

	m_rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	CreateDescriptorHeaps(device);

	m_cb = std::make_unique<UploadBuffer<PostConstants>>(device, Cb_Count, true);
	m_cbStride = CalcConstantBufferByteSize(sizeof(PostConstants));

	const std::wstring file = L"shader\\PostProcess.hlsl";

	m_vs          = CompileShader(file, nullptr, "VS",             "vs_5_1");
	m_brightPS    = CompileShader(file, nullptr, "PS_BrightPass",  "ps_5_1");
	m_blurPS      = CompileShader(file, nullptr, "PS_Blur",        "ps_5_1");
	m_compositePS = CompileShader(file, nullptr, "PS_Composite",   "ps_5_1");

	BuildRootSignature(device);
	BuildPipelines(device);
}

void PostProcess::CreateDescriptorHeaps(ID3D12Device* device)
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
	rtvDesc.NumDescriptors = Rtv_Count;
	rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
	srvDesc.NumDescriptors = Srv_Count;
	srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)));
}

D3D12_CPU_DESCRIPTOR_HANDLE PostProcess::Rtv(UINT index) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(index) * m_rtvSize;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE PostProcess::SrvTable(UINT index) const
{
	D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<UINT64>(index) * m_srvSize;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE PostProcess::HdrRtv() const
{
	return Rtv(Rtv_Hdr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Resize — пересоздаём цели рендеринга под новый размер окна
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::Resize(ID3D12Device* device, UINT width, UINT height)
{
	if (width == 0 || height == 0) return;

	m_width  = width;
	m_height = height;

	m_bloomWidth  = std::max<UINT>(1, width  / BloomDivisor);
	m_bloomHeight = std::max<UINT>(1, height / BloomDivisor);

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	auto MakeTarget = [&](UINT w, UINT h) -> ComPtr<ID3D12Resource>
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = w;
		desc.Height           = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = HdrFormat;
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_CLEAR_VALUE clear = {};
		clear.Format = HdrFormat;
		memcpy(clear.Color, kClearColor, sizeof(kClearColor));

		ComPtr<ID3D12Resource> res;
		ThrowIfFailed(device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&res)));

		return res;
	};

	m_hdr      = MakeTarget(m_width, m_height);
	m_bloom[0] = MakeTarget(m_bloomWidth, m_bloomHeight);
	m_bloom[1] = MakeTarget(m_bloomWidth, m_bloomHeight);

	m_hdrState      = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	m_bloomState[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	m_bloomState[1] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	CreateViews(device);
}

void PostProcess::CreateViews(ID3D12Device* device)
{
	D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
	rtv.Format        = HdrFormat;
	rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	device->CreateRenderTargetView(m_hdr.Get(),      &rtv, Rtv(Rtv_Hdr));
	device->CreateRenderTargetView(m_bloom[0].Get(), &rtv, Rtv(Rtv_Bloom0));
	device->CreateRenderTargetView(m_bloom[1].Get(), &rtv, Rtv(Rtv_Bloom1));

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format                        = HdrFormat;
	srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MostDetailedMip     = 0;
	srv.Texture2D.MipLevels           = 1;
	srv.Texture2D.ResourceMinLODClamp = 0.0f;

	D3D12_CPU_DESCRIPTOR_HANDLE base = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

	auto At = [&](UINT i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = base;
		h.ptr += static_cast<SIZE_T>(i) * m_srvSize;
		return h;
	};

	// Порядок важен: каждому проходу нужна пара подряд идущих дескрипторов
	device->CreateShaderResourceView(m_hdr.Get(),      &srv, At(Srv_Hdr));
	device->CreateShaderResourceView(m_bloom[0].Get(), &srv, At(Srv_Bloom0));
	device->CreateShaderResourceView(m_bloom[1].Get(), &srv, At(Srv_Bloom1));
	device->CreateShaderResourceView(m_bloom[0].Get(), &srv, At(Srv_Bloom0Dup));
}

// ═════════════════════════════════════════════════════════════════════════════
// Root signature: b0 + таблица t0..t1 + линейный сэмплер с зажимом
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::BuildRootSignature(ID3D12Device* device)
{
	D3D12_DESCRIPTOR_RANGE range = {};
	range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors                    = 2;   // t0 источник, t1 свечение
	range.BaseShaderRegister                = 0;
	range.RegisterSpace                     = 0;
	range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER params[2] = {};

	params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

	params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges   = &range;
	params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

	// CLAMP обязателен: при размытии выборки уходят за край текстуры,
	// а с WRAP на противоположной стороне экрана появится «протекание» света.
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxAnisotropy    = 1;
	sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	sampler.MinLOD           = 0.0f;
	sampler.MaxLOD           = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister   = 0;
	sampler.RegisterSpace    = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters     = _countof(params);
	desc.pParameters       = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers   = &sampler;
	desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> blob, errors;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                         blob.GetAddressOf(), errors.GetAddressOf());
	if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
	ThrowIfFailed(hr);

	ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                          IID_PPV_ARGS(&m_rootSig)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Три PSO. Отличаются только пиксельным шейдером и форматом цели.
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::BuildPipelines(ID3D12Device* device)
{
	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode           = D3D12_FILL_MODE_SOLID;
	raster.CullMode           = D3D12_CULL_MODE_NONE;
	raster.DepthClipEnable    = TRUE;
	raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_BLEND_DESC blend = {};
	blend.RenderTarget[0].BlendEnable           = FALSE;
	blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
	blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
	blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Постобработка работает с полноэкранным треугольником, глубина не нужна
	D3D12_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable   = FALSE;
	ds.StencilEnable = FALSE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.InputLayout           = { nullptr, 0 };
	pso.pRootSignature        = m_rootSig.Get();
	pso.VS                    = { m_vs->GetBufferPointer(), m_vs->GetBufferSize() };
	pso.RasterizerState       = raster;
	pso.BlendState            = blend;
	pso.DepthStencilState     = ds;
	pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets      = 1;
	pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
	pso.SampleDesc.Count      = 1;

	// Bright pass и размытие пишут в буферы свечения — формат HDR
	pso.RTVFormats[0] = HdrFormat;

	pso.PS = { m_brightPS->GetBufferPointer(), m_brightPS->GetBufferSize() };
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_bloomPSO)));

	pso.PS = { m_blurPS->GetBufferPointer(), m_blurPS->GetBufferSize() };
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_blurPSO)));

	// Композит пишет в бэк-буфер: формат _SRGB, аппаратное гамма-кодирование
	pso.RTVFormats[0] = m_backBufferRtvFormat;

	pso.PS = { m_compositePS->GetBufferPointer(), m_compositePS->GetBufferSize() };
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_compositePSO)));
}

// ═════════════════════════════════════════════════════════════════════════════
// BeginScene — HDR-буфер становится целью светового прохода
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::BeginScene(ID3D12GraphicsCommandList* cmd)
{
	if (!m_hdr) return;

	Transition(cmd, m_hdr.Get(), m_hdrState, D3D12_RESOURCE_STATE_RENDER_TARGET);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(Rtv_Hdr);
	cmd->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Execute — вся цепочка постобработки
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::Execute(ID3D12GraphicsCommandList* cmd,
                          D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
                          const D3D12_VIEWPORT& viewport,
                          const D3D12_RECT& scissor)
{
	if (!m_hdr || !m_compositePSO) return;

	// ── Константы всех четырёх проходов ─────────────────────────────────────
	const float invW = 1.0f / static_cast<float>(std::max<UINT>(m_width, 1));
	const float invH = 1.0f / static_cast<float>(std::max<UINT>(m_height, 1));
	const float invBW = 1.0f / static_cast<float>(std::max<UINT>(m_bloomWidth, 1));
	const float invBH = 1.0f / static_cast<float>(std::max<UINT>(m_bloomHeight, 1));

	PostConstants pc;
	pc.Exposure            = m_exposure;
	pc.BloomThreshold      = m_bloomThreshold;
	pc.BloomIntensity      = m_bloomEnabled ? m_bloomIntensity : 0.0f;
	pc.VignetteStrength    = m_vignetteEnabled ? 0.35f : 0.0f;
	pc.ChromaticAberration = m_vignetteEnabled ? 0.0015f : 0.0f;
	pc.ToneMapMode         = m_toneMapMode;
	pc.DitherEnabled       = m_ditherEnabled ? 1u : 0u;
	pc.Passthrough         = m_passthrough ? 1u : 0u;
	pc.ScreenSize          = { static_cast<float>(m_width), static_cast<float>(m_height) };

	// Bright pass читает HDR полного разрешения
	pc.TexelSize     = { invW, invH };
	pc.BlurDirection = { 0.0f, 0.0f };
	m_cb->CopyData(Cb_Bright, pc);

	// Размытие читает буферы свечения в четверть разрешения
	pc.TexelSize     = { invBW, invBH };
	pc.BlurDirection = { 1.0f, 0.0f };
	m_cb->CopyData(Cb_BlurH, pc);

	pc.BlurDirection = { 0.0f, 1.0f };
	m_cb->CopyData(Cb_BlurV, pc);

	// Композит читает HDR полного разрешения
	pc.TexelSize     = { invW, invH };
	pc.BlurDirection = { 0.0f, 0.0f };
	m_cb->CopyData(Cb_Composite, pc);

	const D3D12_GPU_VIRTUAL_ADDRESS cbBase = m_cb->Resource()->GetGPUVirtualAddress();

	ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetGraphicsRootSignature(m_rootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 1, nullptr);
	cmd->IASetIndexBuffer(nullptr);

	// Сцена дорисована — HDR переходит в чтение
	Transition(cmd, m_hdr.Get(), m_hdrState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// ═══════════════════════════════════════════════════════════════════════
	// Свечение считаем только если оно включено
	// ═══════════════════════════════════════════════════════════════════════
	if (m_bloomEnabled && !m_passthrough)
	{
		D3D12_VIEWPORT bloomVp = { 0.0f, 0.0f,
		                           static_cast<float>(m_bloomWidth),
		                           static_cast<float>(m_bloomHeight), 0.0f, 1.0f };
		D3D12_RECT bloomRect = { 0, 0,
		                         static_cast<LONG>(m_bloomWidth),
		                         static_cast<LONG>(m_bloomHeight) };

		cmd->RSSetViewports(1, &bloomVp);
		cmd->RSSetScissorRects(1, &bloomRect);

		// ── 1. Bright pass: HDR -> Bloom0 ───────────────────────────────────
		Transition(cmd, m_bloom[0].Get(), m_bloomState[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(Rtv_Bloom0);
			cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			cmd->SetPipelineState(m_bloomPSO.Get());
			cmd->SetGraphicsRootConstantBufferView(0, cbBase + Cb_Bright * m_cbStride);
			cmd->SetGraphicsRootDescriptorTable(1, SrvTable(Srv_Hdr));
			cmd->DrawInstanced(3, 1, 0, 0);
		}

		// ── 2. Горизонтальное размытие: Bloom0 -> Bloom1 ────────────────────
		Transition(cmd, m_bloom[0].Get(), m_bloomState[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Transition(cmd, m_bloom[1].Get(), m_bloomState[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(Rtv_Bloom1);
			cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			cmd->SetPipelineState(m_blurPSO.Get());
			cmd->SetGraphicsRootConstantBufferView(0, cbBase + Cb_BlurH * m_cbStride);
			cmd->SetGraphicsRootDescriptorTable(1, SrvTable(Srv_Bloom0));
			cmd->DrawInstanced(3, 1, 0, 0);
		}

		// ── 3. Вертикальное размытие: Bloom1 -> Bloom0 ──────────────────────
		Transition(cmd, m_bloom[1].Get(), m_bloomState[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Transition(cmd, m_bloom[0].Get(), m_bloomState[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(Rtv_Bloom0);
			cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

			cmd->SetGraphicsRootConstantBufferView(0, cbBase + Cb_BlurV * m_cbStride);
			cmd->SetGraphicsRootDescriptorTable(1, SrvTable(Srv_Bloom1));
			cmd->DrawInstanced(3, 1, 0, 0);
		}
	}

	// Свечение должно быть читаемым в композите в любом случае:
	// при выключенном bloom там просто остаётся чёрный буфер.
	Transition(cmd, m_bloom[0].Get(), m_bloomState[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// ═══════════════════════════════════════════════════════════════════════
	// 4. Композит в бэк-буфер
	// ═══════════════════════════════════════════════════════════════════════
	cmd->RSSetViewports(1, &viewport);
	cmd->RSSetScissorRects(1, &scissor);

	cmd->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);

	cmd->SetPipelineState(m_compositePSO.Get());
	cmd->SetGraphicsRootConstantBufferView(0, cbBase + Cb_Composite * m_cbStride);
	cmd->SetGraphicsRootDescriptorTable(1, SrvTable(Srv_Hdr));   // t0 = HDR, t1 = Bloom0
	cmd->DrawInstanced(3, 1, 0, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Переключатели
// ═════════════════════════════════════════════════════════════════════════════
void PostProcess::ToggleBloom()
{
	m_bloomEnabled = !m_bloomEnabled;
	OutputDebugStringA(m_bloomEnabled ? "[POST] Bloom ON\n" : "[POST] Bloom OFF\n");
}

void PostProcess::ToggleDither()
{
	m_ditherEnabled = !m_ditherEnabled;
	OutputDebugStringA(m_ditherEnabled ? "[POST] Dither ON\n" : "[POST] Dither OFF\n");
}

void PostProcess::ToggleVignette()
{
	m_vignetteEnabled = !m_vignetteEnabled;
	OutputDebugStringA(m_vignetteEnabled ? "[POST] Vignette + aberration ON\n"
	                                     : "[POST] Vignette + aberration OFF\n");
}

void PostProcess::CycleToneMap()
{
	m_toneMapMode = (m_toneMapMode + 1) % 3;

	const char* names[3] = {
		"[POST] Tone mapping OFF (clipping)\n",
		"[POST] Tone mapping: Reinhard\n",
		"[POST] Tone mapping: exposure\n"
	};
	OutputDebugStringA(names[m_toneMapMode]);
}

void PostProcess::ScaleExposure(float factor)
{
	m_exposure = std::max(0.05f, std::min(m_exposure * factor, 20.0f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[POST] Exposure = %.2f\n", m_exposure);
	OutputDebugStringA(buf);
#endif
}

void PostProcess::ScaleBloomThreshold(float factor)
{
	m_bloomThreshold = std::max(0.05f, std::min(m_bloomThreshold * factor, 10.0f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[POST] Bloom threshold = %.2f\n", m_bloomThreshold);
	OutputDebugStringA(buf);
#endif
}
