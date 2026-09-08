#include "GBuffer.hpp"

#include <cstring>

namespace {
	// Форматы таргетов G-Buffer. Порядок совпадает с RtIndex.
	const DXGI_FORMAT kRtFormats[GBuffer::RT_Count] = {
		DXGI_FORMAT_R8G8B8A8_UNORM,       // Albedo
		DXGI_FORMAT_R16G16B16A16_FLOAT,   // Normal
		DXGI_FORMAT_R8G8B8A8_UNORM        // Specular
	};

	const float kClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
}

DXGI_FORMAT GBuffer::Format(UINT index)
{
	return kRtFormats[index];
}

void GBuffer::Init(ID3D12Device* device)
{
	m_rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// RTV-куча: не shader-visible, дескрипторы идут подряд, чтобы
	// OMSetRenderTargets можно было вызвать с одним handle и флагом
	// RTsSingleHandleToDescriptorRange = TRUE.
	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
	rtvDesc.NumDescriptors = RT_Count;
	rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));

	// SRV-куча: shader-visible, её биндим на light pass как одну
	// descriptor table t0..t3.
	D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
	srvDesc.NumDescriptors = SrvCount;
	srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)));
}

void GBuffer::Resize(ID3D12Device* device, UINT width, UINT height, ID3D12Resource* depthBuffer)
{
	if (width == 0 || height == 0) return;

	m_width  = width;
	m_height = height;
	m_depth  = depthBuffer;

	// Framework только что пересоздал depth-буфер и перевёл его в DEPTH_WRITE.
	m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	m_colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask     = 1;
	heapProps.VisibleNodeMask      = 1;

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < RT_Count; ++i)
	{
		m_rt[i].Reset();

		D3D12_RESOURCE_DESC texDesc = {};
		texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Alignment        = 0;
		texDesc.Width            = width;
		texDesc.Height           = height;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels        = 1;
		texDesc.Format           = kRtFormats[i];
		texDesc.SampleDesc.Count = 1;
		texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		// Optimized clear value обязан совпадать с тем, чем реально чистим,
		// иначе драйвер теряет fast clear, а debug layer выдаёт warning.
		D3D12_CLEAR_VALUE clearValue = {};
		clearValue.Format = kRtFormats[i];
		memcpy(clearValue.Color, kClearColor, sizeof(kClearColor));

		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,   // стартовое состояние
			&clearValue,
			IID_PPV_ARGS(&m_rt[i])));

		device->CreateRenderTargetView(m_rt[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_rtvSize;

		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Format                        = kRtFormats[i];
		srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MostDetailedMip     = 0;
		srv.Texture2D.MipLevels           = 1;
		srv.Texture2D.PlaneSlice          = 0;
		srv.Texture2D.ResourceMinLODClamp = 0.0f;

		device->CreateShaderResourceView(m_rt[i].Get(), &srv, srvHandle);
		srvHandle.ptr += m_srvSize;
	}

	// ── SRV на depth-буфер (слот t3) ────────────────────────────────────────
	// Ресурс создан как R24G8_TYPELESS: DSV смотрит на него как
	// D24_UNORM_S8_UINT, а SRV — как R24_UNORM_X8_TYPELESS.
	if (m_depth)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Format                        = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MostDetailedMip     = 0;
		srv.Texture2D.MipLevels           = 1;
		srv.Texture2D.PlaneSlice          = 0;
		srv.Texture2D.ResourceMinLODClamp = 0.0f;

		device->CreateShaderResourceView(m_depth, &srv, srvHandle);
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::RtvStart() const
{
	return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::SrvGpuStart() const
{
	return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

void GBuffer::TransitionToWrite(ID3D12GraphicsCommandList* cmd)
{
	if (!IsReady()) return;

	D3D12_RESOURCE_BARRIER barriers[RT_Count + 1] = {};
	UINT count = 0;

	if (m_colorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
	{
		for (UINT i = 0; i < RT_Count; ++i)
		{
			barriers[count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[count].Transition.pResource   = m_rt[i].Get();
			barriers[count].Transition.StateBefore = m_colorState;
			barriers[count].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			++count;
		}
		m_colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	if (m_depth && m_depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
	{
		barriers[count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[count].Transition.pResource   = m_depth;
		barriers[count].Transition.StateBefore = m_depthState;
		barriers[count].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		++count;
		m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}

	if (count > 0)
		cmd->ResourceBarrier(count, barriers);
}

void GBuffer::TransitionToRead(ID3D12GraphicsCommandList* cmd)
{
	if (!IsReady()) return;

	D3D12_RESOURCE_BARRIER barriers[RT_Count + 1] = {};
	UINT count = 0;

	if (m_colorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	{
		for (UINT i = 0; i < RT_Count; ++i)
		{
			barriers[count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[count].Transition.pResource   = m_rt[i].Get();
			barriers[count].Transition.StateBefore = m_colorState;
			barriers[count].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			++count;
		}
		m_colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}

	if (m_depth && m_depthState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	{
		barriers[count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[count].Transition.pResource   = m_depth;
		barriers[count].Transition.StateBefore = m_depthState;
		barriers[count].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		++count;
		m_depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}

	if (count > 0)
		cmd->ResourceBarrier(count, barriers);
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
	if (!IsReady()) return;

	D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < RT_Count; ++i)
	{
		cmd->ClearRenderTargetView(h, kClearColor, 0, nullptr);
		h.ptr += m_rtvSize;
	}

	cmd->ClearDepthStencilView(
		dsv,
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
		1.0f, 0, 0, nullptr);
}
