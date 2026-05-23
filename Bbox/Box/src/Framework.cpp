#include "Framework.hpp"
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <cmath>
#include <string>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <filesystem>
#include <vector>
#include <algorithm>
#include <cfloat>

#if defined(_DEBUG)
#include <d3d12sdklayers.h>
#endif

using namespace DirectX;

Framework::Framework(int width, int height, const wchar_t* title)
	: m_initWidth(width)
	, m_initHeight(height)
	, m_title(title ? title : L"")
	, m_clientWidth(width)
	, m_clientHeight(height)
{
}

Framework::~Framework() {
	if (m_device)
		FlushCommandQueue();

	if (m_fenceEvent) {
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}
}

bool Framework::Init() {
	m_window = std::make_unique<Window>(m_initWidth, m_initHeight, m_title, this);

	InitDxgi();
	InitD3D12Device();
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CreateCommandObjects();
	CreateFence();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();
	BuildShaders();
	BuildConstantBuffers();
	BuildCbvHeap();
	BuildCbvViews();
	BuildRootSignature();
	BuildPSO();
	BuildBoxGeometry();
	CreateWhiteTexture();   // m_textures[0] = белая заглушка 1×1
	BuildObjVB_Upload();    // загружает OBJ + DDS текстуры в m_textures[1..]
	BuildSrvHeap();         // создаёт SRV-кучу под sizeof(m_textures) слотов
	BuildSrvViews();        // заполняет дескрипторы текстур

	OnResize();

	return MainWnd() != nullptr;
}

int Framework::Run() {
	m_timer.Reset();

	while (m_window->ProcessMessages()) {
		m_timer.Tick();

		if (!m_appPaused) {
			const double dt = m_timer.DeltaTime();
			Update(dt);
			Draw();
		}
		else {
			Sleep(100);
		}
	}

	return 0;
}

LRESULT Framework::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_SIZE:
		m_clientWidth = LOWORD(lParam);
		m_clientHeight = HIWORD(lParam);

		if (wParam == SIZE_MINIMIZED) {
			m_appPaused = true;
			m_minimized = true;
			m_maximized = false;
			m_timer.Stop();
		}
		else if (wParam == SIZE_MAXIMIZED) {
			m_appPaused = false;
			m_minimized = false;
			m_maximized = true;
			m_timer.Start();
			OnResize();
		}
		else if (wParam == SIZE_RESTORED) {
			if (m_minimized) {
				m_appPaused = false;
				m_minimized = false;
				m_timer.Start();
				OnResize();
			}
			else if (m_maximized) {
				m_appPaused = false;
				m_maximized = false;
				m_timer.Start();
				OnResize();
			}
			else if (m_resizing) {

			}
			else {
				OnResize();
			}
		}

		return 0;

	case WM_ACTIVATEAPP:
		if (wParam == FALSE)
		{
			m_appPaused = true;
			m_timer.Stop();
		}
		else
		{
			m_appPaused = false;
			m_timer.Start();
		}
		return 0;

	case WM_ENTERSIZEMOVE:
		m_appPaused = true;
		m_resizing = true;
		m_timer.Stop();
		return 0;

	case WM_EXITSIZEMOVE:
		m_appPaused = false;
		m_resizing = false;
		m_timer.Start();
		OnResize();
		return 0;

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_MOUSEMOVE:
		OnMouseMove(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		const uint8_t vk = static_cast<uint8_t>(wParam);
		m_keyDown[vk] = true;
		return 0;
	}

	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		const uint8_t vk = static_cast<uint8_t>(wParam);
		m_keyDown[vk] = false;
		return 0;
	}

	// ����� ��� ������ ������ �� ���� "��������" ������
	case WM_KILLFOCUS:
	{
		m_keyDown.fill(false);
		return 0;
	}

	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Framework::CreateRtvAndDsvDescriptorHeaps()
{
	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
}

void Framework::OnResize()
{
	if (!m_device || !m_swapChain || !m_commandQueue || !m_directCmdListAlloc || !m_commandList)
		return;

	FlushCommandQueue();

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		m_swapChainBuffer[i].Reset();
	}

	m_depthStencilBuffer.Reset();

	ThrowIfFailed(m_swapChain->ResizeBuffers(SwapChainBufferCount, m_clientWidth, m_clientHeight, m_backBufferFormat, 0));

	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffer[i])));
		m_device->CreateRenderTargetView(m_swapChainBuffer[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_rtvDescriptorSize;
	}

	D3D12_RESOURCE_DESC depthDesc = {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Alignment = 0;
	depthDesc.Width = static_cast<UINT64>(m_clientWidth);
	depthDesc.Height = static_cast<UINT64>(m_clientHeight);
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = m_depthStencilFormat;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.SampleDesc.Quality = 0;
	depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear = {};
	optClear.Format = m_depthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_COMMON, &optClear, IID_PPV_ARGS(&m_depthStencilBuffer)));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = m_depthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, DepthStencilView());

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = m_depthStencilBuffer.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &barrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdsLists);

	FlushCommandQueue();

	m_screenViewport.TopLeftX = 0.0f;
	m_screenViewport.TopLeftY = 0.0f;
	m_screenViewport.Width = static_cast<float>(m_clientWidth);
	m_screenViewport.Height = static_cast<float>(m_clientHeight);
	m_screenViewport.MinDepth = 0.0f;
	m_screenViewport.MaxDepth = 1.0f;

	m_scissorRect = { 0, 0, m_clientWidth, m_clientHeight };
}

void Framework::Update(const double& dt)
{
	ObjectConstants obj = {};
	XMMATRIX world =
		XMMatrixTranslation(-m_modelCenter.x, -m_modelCenter.y, -m_modelCenter.z) *
		XMMatrixScaling(m_modelScale, m_modelScale, m_modelScale);
	XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	XMStoreFloat4x4(&obj.World, XMMatrixTranspose(world));
	XMStoreFloat4x4(&obj.WorldInvTranspose, worldInvTranspose);

	// ── Определяем «только что нажата» (rising edge) ────────────────────────
	auto JustPressed = [&](uint8_t vk) {
		return m_keyDown[vk] && !m_keyDownPrev[vk];
	};

	// T — вкл/выкл UV-анимацию
	if (JustPressed('T')) {
		m_uvAnimEnabled = !m_uvAnimEnabled;
		OutputDebugStringA(m_uvAnimEnabled
			? "[UV] Animation ON\n"
			: "[UV] Animation OFF\n");
	}

	// [ — тайлинг ÷2    ] — тайлинг ×2
	if (JustPressed(VK_OEM_4)) {          // [ (уменьшить)
		m_uvTile.x *= 0.5f;
		m_uvTile.y *= 0.5f;
	}
	if (JustPressed(VK_OEM_6)) {          // ] (увеличить)
		m_uvTile.x *= 2.0f;
		m_uvTile.y *= 2.0f;
	}

	// 0 — сбросить тайлинг и смещение
	if (JustPressed('0')) {
		m_uvTile   = { 1.0f, 1.0f };
		m_uvOffset = { 0.0f, 0.0f };
		OutputDebugStringA("[UV] Reset tile=1 offset=0\n");
	}

	// UV-анимация: смещение по оси U
	if (m_uvAnimEnabled) {
		m_uvOffset.x += m_uvAnimSpeed * static_cast<float>(dt);
		if (m_uvOffset.x > 1.0f) m_uvOffset.x -= 1.0f;
	}

	obj.UvOffset = m_uvOffset;
	obj.UvScale  = m_uvTile;

	// Сохраняем состояние клавиш для следующего кадра
	m_keyDownPrev = m_keyDown;

	m_objectCB->CopyData(0, obj);

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMVECTOR target = XMLoadFloat3(&m_camTarget);
	XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&m_camUp));

	XMVECTOR forward = XMVector3Normalize(target - pos);
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

	float speed = m_cameraMoveSpeed;
	if (m_keyDown[VK_SHIFT]) speed *= 3.0f; 

	float step = speed * static_cast<float>(dt);
	XMVECTOR move = XMVectorZero();

	if (m_keyDown['W']) move += forward;
	if (m_keyDown['S']) move -= forward;
	if (m_keyDown['D']) move += right;
	if (m_keyDown['A']) move -= right;

	if (m_keyDown[VK_SPACE])   move += up;
	if (m_keyDown[VK_CONTROL]) move -= up;

	if (!XMVector3Equal(move, XMVectorZero()))
		move = XMVector3Normalize(move) * step;

	pos += move;
	target += move;

	XMStoreFloat3(&m_camPos, pos);
	XMStoreFloat3(&m_camTarget, target);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);

	float aspect = (float)m_clientWidth / (float)m_clientHeight;
	XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.1f, 1000.0f);

	XMMATRIX viewProj = view * proj;

	PassConstants pass{};
	XMStoreFloat4x4(&pass.ViewProj, XMMatrixTranspose(viewProj));

	XMStoreFloat3(&pass.EyePosW, pos);

	pass.LightDirW = { 0.577f, -0.3f, 0.577f };
	pass.Ambient = { 0.2f, 0.2f, 0.2f, 1.0f };
	pass.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	pass.Specular = { 1.0f, 1.0f, 1.0f, 1.0f };
	pass.SpecPower = 32.0f;

	m_passCB->CopyData(0, pass);
}

void Framework::Draw()
{
	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), m_pso.Get()));

	D3D12_RESOURCE_BARRIER toRT{};
	toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRT.Transition.pResource = CurrentBackBuffer();
	toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toRT);

	m_commandList->RSSetViewports(1, &m_screenViewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

	// Root-CBV: биндим константные буферы по GPU-адресу (без heap)
	m_commandList->SetGraphicsRootConstantBufferView(
		0, m_objectCB->Resource()->GetGPUVirtualAddress());
	m_commandList->SetGraphicsRootConstantBufferView(
		1, m_passCB->Resource()->GetGPUVirtualAddress());

	// Биндим SRV-кучу (slot 2 = descriptor table для текстур)
	if (m_srvHeap)
	{
		ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
		m_commandList->SetDescriptorHeaps(1, heaps);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferView();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();
	m_commandList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

	m_commandList->ClearRenderTargetView(rtv, DirectX::Colors::White, 0, nullptr);
	m_commandList->ClearDepthStencilView(
		dsv,
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
		1.0f,
		0,
		0,
		nullptr
	);

	m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (m_modelVB && !m_subMeshes.empty() && m_srvHeap)
	{
		m_commandList->IASetVertexBuffers(0, 1, &m_modelVBV);

		D3D12_GPU_DESCRIPTOR_HANDLE srvBase =
			m_srvHeap->GetGPUDescriptorHandleForHeapStart();

		for (const auto& sm : m_subMeshes)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = srvBase;
			srvHandle.ptr += static_cast<UINT64>(sm.srvIndex) * m_srvDescSize;
			m_commandList->SetGraphicsRootDescriptorTable(2, srvHandle);
			m_commandList->DrawInstanced(sm.vertexCount, 1, sm.vertexOffset, 0);
		}
	}
	else
	{
		// fallback: box (если OBJ не загрузился)
		if (m_srvHeap)
		{
			// белая текстура всегда в srvIndex=0
			m_commandList->SetGraphicsRootDescriptorTable(
				2, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
		}
		m_commandList->IASetVertexBuffers(0, 1, &m_boxVBView);
		m_commandList->IASetIndexBuffer(&m_boxIBView);
		m_commandList->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
	}

	D3D12_RESOURCE_BARRIER toPresent{};
	toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toPresent.Transition.pResource = CurrentBackBuffer();
	toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toPresent);

	ThrowIfFailed(m_commandList->Close());

	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(m_swapChain->Present(0, 0));
	m_currBackBuffer = (m_currBackBuffer + 1) % SwapChainBufferCount;

	FlushCommandQueue();
}


void Framework::InitDxgi() {
	UINT factoryFlags = 0;

#if defined(_DEBUG)
	factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));

#if defined(_DEBUG)
	LogAdapters();
#endif

	PickAdapter();
}

void Framework::PickAdapter() {
	m_dxgiAdapter.Reset();
	m_adapterName.clear();

	ComPtr<IDXGIFactory6> factory6;

	if (SUCCEEDED(m_dxgiFactory.As(&factory6))) {
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;

			if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc = {};
			ThrowIfFailed(adapter->GetDesc1(&desc));

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			ComPtr<ID3D12Device> testDevice;

			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice)))) {
				m_dxgiAdapter = adapter;
				m_adapterName = desc.Description;
				break;
			}
		}
	}

	if (!m_dxgiAdapter) {
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;

			if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc = {};
			ThrowIfFailed(adapter->GetDesc1(&desc));

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			ComPtr<ID3D12Device> testDevice;

			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice)))) {
				m_dxgiAdapter = adapter;
				m_adapterName = desc.Description;
				break;
			}
		}
	}

	if (!m_dxgiAdapter) {
		throw std::runtime_error("No suitable DXGI adapter found (D3D12-capable).");
	}

#if defined(_DEBUG)
	std::wstring msg = L"[DXGI] Using adapter: " + m_adapterName + L"\n";
	OutputDebugStringW(msg.c_str());
#endif
}

void Framework::LogAdapters() {
#if defined(_DEBUG)
	OutputDebugStringW(L"[DXGI] Adapters:\n");

	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		
		HRESULT hr = m_dxgiFactory->EnumAdapters1(i, &adapter);
		if (hr == DXGI_ERROR_NOT_FOUND) break;
		ThrowIfFailed(hr);

		if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_ADAPTER_DESC1 desc = {};
		ThrowIfFailed(adapter->GetDesc1(&desc));

		std::wstring line = L"  -  ";
		line += desc.Description;
		line += (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" (SOPTWARE)\n" : L"\n";
		OutputDebugStringW(line.c_str());

		LogAdapterOutputs(adapter.Get());
	}
#endif
}

void Framework::LogAdapterOutputs(IDXGIAdapter1* adapter) {
#if defined(_DEBUG)
	for (UINT j = 0;; ++j) {
		ComPtr<IDXGIOutput> output;

		if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_OUTPUT_DESC outDesc = {};
		ThrowIfFailed(output->GetDesc(&outDesc));

		std::wstring line = L"		Ouput: ";
		line += outDesc.DeviceName;
		line += L"\n";
		OutputDebugStringW(line.c_str());
	}
#endif
}

void Framework::InitD3D12Device() {
#if defined(_DEBUG)
	ComPtr<ID3D12Debug> debugController;

	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();
		OutputDebugStringW(L"[D3d12] Debug layer enabled\n");
	}
	else {
		OutputDebugStringW(L"[D3D12] Debug layer NOT available (Graphics Tools may be missing)\n");
	}
#endif

	HRESULT hr = D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

	if (FAILED(hr)) {
		OutputDebugStringW(L"[D3D12] Hardware device failed, falling back to WARP\n");

		ThrowIfFailed(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&m_dxgiAdapter)));
		ThrowIfFailed(D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));
	}

#if defined(_DEBUG)
	OutputDebugStringW(L"[D3D12] Device created \n");

	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_device.As(&infoQueue))) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		// WARNING намеренно не разрываем — debug-layer даёт harmless warnings
		// (root CBV + DENY flags), которые не являются реальными ошибками.
	}
#endif
}

void Framework::CreateCommandObjects() {
	D3D12_COMMAND_QUEUE_DESC qdesc = {};
	qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(m_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_commandQueue)));

	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_directCmdListAlloc)));

	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_directCmdListAlloc.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));

	ThrowIfFailed(m_commandList->Close());

#if defined(_DEBUG)
	OutputDebugStringW(L"[D3D12] Command queue/allocator/list created\n");
#endif
}

void Framework::CreateFence() {
	ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

	m_currentFence = 0;

	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (m_fenceEvent == nullptr)
		throw std::runtime_error("CreateEvent failed for fence event.");
}

void Framework::FlushCommandQueue() {
	if (!m_commandQueue || !m_fence || !m_fenceEvent)
		return;

	++m_currentFence;
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_currentFence));

	if (m_fence->GetCompletedValue() < m_currentFence) {
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_currentFence, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}
}

void Framework::CreateSwapChain() {
	m_swapChain.Reset();

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = m_clientWidth;
	sd.Height = m_clientHeight;
	sd.Format = m_backBufferFormat;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	sd.Flags = 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(m_commandQueue.Get(), MainWnd(), &sd, nullptr, nullptr, &swapChain1));
	ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(MainWnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain1.As(&m_swapChain));
	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());
}

void Framework::BuildShaders()
{
	const std::wstring shaderFile = L"shader\\Phong.hlsl";

	m_vsByteCode = CompileShader(shaderFile, nullptr, "VS", "vs_5_1");
	m_psByteCode = CompileShader(shaderFile, nullptr, "PS", "ps_5_1");
}

void Framework::BuildConstantBuffers()
{
	m_objectCB = std::make_unique<UploadBuffer<ObjectConstants>>(m_device.Get(), 1, true);
	m_passCB = std::make_unique<UploadBuffer<PassConstants>>(m_device.Get(), 1, true);
}

void Framework::BuildCbvHeap()
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = 2;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_cbvHeap.GetAddressOf())));
}

void Framework::BuildCbvViews()
{
	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_objectCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(ObjectConstants));

		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
		m_device->CreateConstantBufferView(&cbvDesc, h);
	}

	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_passCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(PassConstants));

		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)m_cbvSrvUavDescriptorSize;
		m_device->CreateConstantBufferView(&cbvDesc, h);
	}
}

void Framework::BuildRootSignature()
{
	// --- Slot 2: descriptor table с 1 SRV (t0 = diffuse texture) ---
	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors                    = 1;
	srvRange.BaseShaderRegister                = 0;
	srvRange.RegisterSpace                     = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// DX12: нельзя иметь две кучи одного типа (CBV_SRV_UAV) одновременно.
	// Поэтому CBV-ы отдаём как root-дескрипторы (GPU VA), а SRV — через heap.
	D3D12_ROOT_PARAMETER rootParams[3] = {};

	// Slot 0: root CBV — ObjectCB (b0), только VS (PS его не читает: UV-трансформ делает VS)
	rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParams[0].Descriptor.ShaderRegister = 0;
	rootParams[0].Descriptor.RegisterSpace  = 0;
	rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

	// Slot 1: root CBV — PassCB (b1), ALL: VS нужен ViewProj, PS нужны свет + eye
	rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParams[1].Descriptor.ShaderRegister = 1;
	rootParams[1].Descriptor.RegisterSpace  = 0;
	rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

	// Slot 2: descriptor table — diffuse SRV (t0), только PS
	rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[2].DescriptorTable.pDescriptorRanges   = &srvRange;
	rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

	// --- Статический сэмплер s0: линейная фильтрация, Wrap ---
	D3D12_STATIC_SAMPLER_DESC staticSampler = {};
	staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSampler.MipLODBias       = 0.0f;
	staticSampler.MaxAnisotropy    = 1;
	staticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
	staticSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	staticSampler.MinLOD           = 0.0f;
	staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
	staticSampler.ShaderRegister   = 0;
	staticSampler.RegisterSpace    = 0;
	staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
	rootSigDesc.NumParameters     = 3;
	rootSigDesc.pParameters       = rootParams;
	rootSigDesc.NumStaticSamplers = 1;
	rootSigDesc.pStaticSamplers   = &staticSampler;
	// Флаги DENY убраны: при root CBV с ALL-visibility они давали D3D12 WARNING.
	// Для нашего пайплайна (только VS+PS) DENY-флаги не нужны.
	rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> serializedRootSig;
	ComPtr<ID3DBlob> errorBlob;

	HRESULT hr = D3D12SerializeRootSignature(
		&rootSigDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(),
		errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
		OutputDebugStringA((char*)errorBlob->GetBufferPointer());

	ThrowIfFailed(hr);

	ThrowIfFailed(m_device->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_rootSignature.GetAddressOf())));
}

void Framework::BuildPSO()
{
	D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_RASTERIZER_DESC rasterDesc = {};
	rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterDesc.CullMode = D3D12_CULL_MODE_BACK;
	rasterDesc.FrontCounterClockwise = FALSE;
	rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	rasterDesc.DepthClipEnable = TRUE;
	rasterDesc.MultisampleEnable = FALSE;
	rasterDesc.AntialiasedLineEnable = FALSE;
	rasterDesc.ForcedSampleCount = 0;
	rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = FALSE;
	blendDesc.IndependentBlendEnable = FALSE;
	{
		D3D12_RENDER_TARGET_BLEND_DESC rt = {};
		rt.BlendEnable = FALSE;
		rt.LogicOpEnable = FALSE;
		rt.SrcBlend = D3D12_BLEND_ONE;
		rt.DestBlend = D3D12_BLEND_ZERO;
		rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_ZERO;
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt.LogicOp = D3D12_LOGIC_OP_NOOP;
		rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		blendDesc.RenderTarget[0] = rt;
	}

	D3D12_DEPTH_STENCIL_DESC dsDesc = {};
	dsDesc.DepthEnable = TRUE;
	dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	dsDesc.StencilEnable = FALSE;
	dsDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
	dsDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	dsDesc.BackFace = dsDesc.FrontFace;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = { m_vsByteCode->GetBufferPointer(), m_vsByteCode->GetBufferSize() };
	psoDesc.PS = { m_psByteCode->GetBufferPointer(), m_psByteCode->GetBufferSize() };
	psoDesc.RasterizerState = rasterDesc;
	psoDesc.BlendState = blendDesc;
	psoDesc.DepthStencilState = dsDesc;
	psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = m_backBufferFormat;
	psoDesc.DSVFormat = m_depthStencilFormat;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pso.GetAddressOf())));
}

void Framework::BuildBoxGeometry()
{
	auto ColorFromPos = [](float x, float y, float z)
		{
			return DirectX::XMFLOAT4(
				(x + 1.0f) * 0.5f,
				(y + 1.0f) * 0.5f,
				(z + 1.0f) * 0.5f,
				1.0f
			);
		};

	std::array<Vertex, 24> vertices =
	{
		Vertex{ { -1.0f, -1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },
		Vertex{ { -1.0f,  1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f, -1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },

		Vertex{ { -1.0f, -1.0f,  1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f, -1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ { -1.0f, -1.0f, -1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },

		Vertex{ { -1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },
		Vertex{ {  1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },
	};

	std::array<std::uint16_t, 36> indices =
	{
		0, 1, 2,  0, 2, 3,
		4, 5, 6,  4, 6, 7,
		8, 9,10,  8,10,11,
		12,13,14, 12,14,15,
		16,17,18, 16,18,19,
		20,21,22, 20,22,23
	};

	m_boxIndexCount = (UINT)indices.size();

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto MakeBufferDesc = [](UINT64 byteSize)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Alignment = 0;
			d.Width = byteSize;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.SampleDesc.Quality = 0;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			d.Flags = D3D12_RESOURCE_FLAG_NONE;
			return d;
		};

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	auto vbDesc = MakeBufferDesc(vbByteSize);
	auto ibDesc = MakeBufferDesc(ibByteSize);

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(m_boxVB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&ibDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(m_boxIB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_boxVBUpload.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&ibDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_boxIBUpload.GetAddressOf())));

	{
		void* mapped = nullptr;
		ThrowIfFailed(m_boxVBUpload->Map(0, nullptr, &mapped));
		memcpy(mapped, vertices.data(), vbByteSize);
		m_boxVBUpload->Unmap(0, nullptr);
	}
	{
		void* mapped = nullptr;
		ThrowIfFailed(m_boxIBUpload->Map(0, nullptr, &mapped));
		memcpy(mapped, indices.data(), ibByteSize);
		m_boxIBUpload->Unmap(0, nullptr);
	}

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_commandList->CopyBufferRegion(m_boxVB.Get(), 0, m_boxVBUpload.Get(), 0, vbByteSize);
	m_commandList->CopyBufferRegion(m_boxIB.Get(), 0, m_boxIBUpload.Get(), 0, ibByteSize);

	D3D12_RESOURCE_BARRIER barriers[2] = {};

	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = m_boxVB.Get();
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = m_boxIB.Get();
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	m_commandList->ResourceBarrier(2, barriers);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmds[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmds);

	FlushCommandQueue();

	m_boxVBView.BufferLocation = m_boxVB->GetGPUVirtualAddress();
	m_boxVBView.StrideInBytes = sizeof(Vertex);
	m_boxVBView.SizeInBytes = vbByteSize;

	m_boxIBView.BufferLocation = m_boxIB->GetGPUVirtualAddress();
	m_boxIBView.Format = DXGI_FORMAT_R16_UINT;
	m_boxIBView.SizeInBytes = ibByteSize;

	m_boxVBUpload.Reset();
	m_boxIBUpload.Reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateWhiteTexture — 1×1 белая текстура RGBA (fallback для мешей без diffuse)
// ─────────────────────────────────────────────────────────────────────────────
void Framework::CreateWhiteTexture()
{
	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width              = 1;
	texDesc.Height             = 1;
	texDesc.DepthOrArraySize   = 1;
	texDesc.MipLevels          = 1;
	texDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count   = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	TexEntry entry;
	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&entry.resource)));

	// Размер промежуточного буфера
	UINT64 uploadSize = 0;
	m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width              = uploadSize;
	bufDesc.Height             = 1;
	bufDesc.DepthOrArraySize   = 1;
	bufDesc.MipLevels          = 1;
	bufDesc.Format             = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count   = 1;
	bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&entry.uploadHeap)));

	// Записываем пиксель { 255, 255, 255, 255 }
	uint8_t whitePixel[4] = { 255, 255, 255, 255 };

	D3D12_SUBRESOURCE_DATA subData = {};
	subData.pData      = whitePixel;
	subData.RowPitch   = 4;
	subData.SlicePitch = 4;

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	UpdateSubresources(m_commandList.Get(),
		entry.resource.Get(), entry.uploadHeap.Get(),
		0, 0, 1, &subData);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = entry.resource.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &barrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmds[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmds);
	FlushCommandQueue();

	entry.uploadHeap.Reset();       // upload-буфер больше не нужен
	m_textures.push_back(std::move(entry)); // index 0 = белая заглушка
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildSrvHeap — создаёт SRV-кучу на sizeof(m_textures) слотов
// ─────────────────────────────────────────────────────────────────────────────
void Framework::BuildSrvHeap()
{
	if (m_textures.empty()) return;

	m_srvDescSize = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = static_cast<UINT>(m_textures.size());
	heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	heapDesc.NodeMask       = 0;

	ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)));
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildSrvViews — заполняет SRV-дескрипторы для каждой текстуры в m_textures
// ─────────────────────────────────────────────────────────────────────────────
void Framework::BuildSrvViews()
{
	if (!m_srvHeap) return;

	D3D12_CPU_DESCRIPTOR_HANDLE handle =
		m_srvHeap->GetCPUDescriptorHandleForHeapStart();

	for (auto& tex : m_textures)
	{
		if (!tex.resource) {
			handle.ptr += m_srvDescSize;
			continue;
		}

		D3D12_RESOURCE_DESC desc = tex.resource->GetDesc();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format                        = desc.Format;
		srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MostDetailedMip     = 0;
		srvDesc.Texture2D.MipLevels           = desc.MipLevels;
		srvDesc.Texture2D.PlaneSlice          = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		m_device->CreateShaderResourceView(tex.resource.Get(), &srvDesc, handle);
		handle.ptr += m_srvDescSize;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// (Устаревший хелпер оставлен для справки, не вызывается)
// ─────────────────────────────────────────────────────────────────────────────
static void LoadObjAsTriangleList(
	const std::wstring& objPathW,
	std::vector<Vertex>& outVertices)
{
	using namespace DirectX;

	std::string objPath(objPathW.begin(), objPathW.end());

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn, err;

	std::filesystem::path p(objPath);
	std::string baseDir = p.parent_path().string();
	if (!baseDir.empty() && baseDir.back() != '/' && baseDir.back() != '\\')
		baseDir += "/";

	bool ok = tinyobj::LoadObj(
		&attrib, &shapes, &materials,
		&warn, &err,
		objPath.c_str(), baseDir.c_str(),
		true);

	if (!warn.empty())
		OutputDebugStringA((std::string("tinyobj warn: ") + warn + "\n").c_str());

	if (!ok)
		throw std::runtime_error("tinyobj error: " + err);

	outVertices.clear();

	const bool hasNormals = !attrib.normals.empty();

	for (const auto& shape : shapes)
	{
		size_t indexOffset = 0;

		for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
		{
			int fv = shape.mesh.num_face_vertices[f];
			if (fv != 3) { indexOffset += fv; continue; }

			Vertex tri[3]{};

			for (int v = 0; v < 3; v++)
			{
				tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];

				tri[v].Pos = {
					attrib.vertices[3 * idx.vertex_index + 0],
					attrib.vertices[3 * idx.vertex_index + 1],
					attrib.vertices[3 * idx.vertex_index + 2]
				};

				if (hasNormals && idx.normal_index >= 0)
				{
					tri[v].Normal = {
						attrib.normals[3 * idx.normal_index + 0],
						attrib.normals[3 * idx.normal_index + 1],
						attrib.normals[3 * idx.normal_index + 2]
					};
				}
				else
				{
					tri[v].Normal = { 0,0,0 };
				}
			}

			if (!hasNormals)
			{
				XMVECTOR A = XMLoadFloat3(&tri[0].Pos);
				XMVECTOR B = XMLoadFloat3(&tri[1].Pos);
				XMVECTOR C = XMLoadFloat3(&tri[2].Pos);

				XMVECTOR N = XMVector3Normalize(XMVector3Cross(B - A, C - A));
				XMFLOAT3 n;
				XMStoreFloat3(&n, N);

				tri[0].Normal = n;
				tri[1].Normal = n;
				tri[2].Normal = n;
			}

			outVertices.push_back(tri[0]);
			outVertices.push_back(tri[1]);
			outVertices.push_back(tri[2]);

			indexOffset += 3;
		}
	}
}

void Framework::BuildObjVB_Upload()
{
	using namespace DirectX;

	// ── 0) Путь к OBJ ───────────────────────────────────────────────────────
	const std::wstring objPathW = L"assets\\sponza.obj";

	auto WideToUtf8 = [](const std::wstring& w) -> std::string
	{
		if (w.empty()) return {};
		int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string s((n > 0) ? (n - 1) : 0, '\0');
		if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
		return s;
	};

	std::string objPath = WideToUtf8(objPathW);

	std::wstring dirW = objPathW;
	size_t slashPos = dirW.find_last_of(L"\\/");
	std::wstring baseDirW = (slashPos != std::wstring::npos) ? dirW.substr(0, slashPos + 1) : L"";
	std::string  baseDir  = WideToUtf8(baseDirW);

	// ── 1) tinyobj LoadObj ──────────────────────────────────────────────────
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t>    shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn, err;

	bool ok = tinyobj::LoadObj(
		&attrib, &shapes, &materials,
		&warn, &err,
		objPath.c_str(),
		baseDir.empty() ? nullptr : baseDir.c_str(),
		/*triangulate*/ true);

	if (!warn.empty()) OutputDebugStringA(("[tinyobj warn] " + warn + "\n").c_str());
	if (!err.empty())  OutputDebugStringA(("[tinyobj err ] " + err  + "\n").c_str());
	if (!ok)
	{
		OutputDebugStringA("[OBJ] LoadObj failed — falling back to box geometry.\n");
		return;   // m_subMeshes остаётся пустым, Draw() нарисует куб
	}

	// ── 2) Загружаем DDS-текстуры для материалов ────────────────────────────
	// m_textures[0] уже = белая заглушка (создана вызовом CreateWhiteTexture)
	// matToSrv[i] → индекс в m_textures для material_id=i
	std::vector<int> matToSrv(materials.size(), 0); // по умолчанию → заглушка

	// Собираем все copy-команды в один проход командного списка
	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	for (size_t mi = 0; mi < materials.size(); ++mi)
	{
		const std::string& texName = materials[mi].diffuse_texname;
		if (texName.empty()) continue;

		// Строим полный путь к DDS
		std::wstring texPathW = baseDirW + std::wstring(texName.begin(), texName.end());

		TexEntry entry;
		HRESULT hr = DirectX::CreateDDSTextureFromFile12(
			m_device.Get(),
			m_commandList.Get(),
			texPathW.c_str(),
			entry.resource,
			entry.uploadHeap);

		if (SUCCEEDED(hr) && entry.resource)
		{
			matToSrv[mi] = static_cast<int>(m_textures.size());
			m_textures.push_back(std::move(entry));
#if defined(_DEBUG)
			OutputDebugStringW((L"[TEX] Loaded: " + texPathW + L"\n").c_str());
#endif
		}
		else
		{
			OutputDebugStringW((L"[TEX] FAILED (fallback white): " + texPathW + L"\n").c_str());
		}
	}

	ThrowIfFailed(m_commandList->Close());
	{
		ID3D12CommandList* cmds[] = { m_commandList.Get() };
		m_commandQueue->ExecuteCommandLists(1, cmds);
	}
	FlushCommandQueue();

	// После FlushCommandQueue upload-буферы текстур больше не нужны
	for (auto& tex : m_textures)
		tex.uploadHeap.Reset();

	// ── 3) Читаем вершины, группируем по material_id ─────────────────────────
	const bool hasNormals  = !attrib.normals.empty();
	const bool hasTexcoord = !attrib.texcoords.empty();

	// groups[material_id] → список вершин для этого материала
	// material_id == -1 → нет материала
	std::unordered_map<int, std::vector<Vertex>> groups;

	XMFLOAT3 minP = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
	XMFLOAT3 maxP = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

	auto ExpandBounds = [&](const XMFLOAT3& p)
	{
		if (p.x < minP.x) minP.x = p.x;  if (p.x > maxP.x) maxP.x = p.x;
		if (p.y < minP.y) minP.y = p.y;  if (p.y > maxP.y) maxP.y = p.y;
		if (p.z < minP.z) minP.z = p.z;  if (p.z > maxP.z) maxP.z = p.z;
	};

	auto ReadPos = [&](int vi) -> XMFLOAT3
	{
		if (vi < 0) return { 0,0,0 };
		return { attrib.vertices[3*vi+0], attrib.vertices[3*vi+1], attrib.vertices[3*vi+2] };
	};

	auto ReadNrm = [&](int ni) -> XMFLOAT3
	{
		if (!hasNormals || ni < 0) return { 0,1,0 };
		return { attrib.normals[3*ni+0], attrib.normals[3*ni+1], attrib.normals[3*ni+2] };
	};

	auto ReadUV = [&](int ti) -> XMFLOAT2
	{
		if (!hasTexcoord || ti < 0) return { 0.0f, 0.0f };
		// DX: V flip (tinyobj V идёт снизу вверх, DX — сверху вниз)
		return { attrib.texcoords[2*ti+0], 1.0f - attrib.texcoords[2*ti+1] };
	};

	for (const auto& sh : shapes)
	{
		size_t indexOffset = 0;

		for (size_t f = 0; f < sh.mesh.num_face_vertices.size(); ++f)
		{
			int fv = sh.mesh.num_face_vertices[f];
			if (fv != 3) { indexOffset += (size_t)fv; continue; }

			int matId = (f < sh.mesh.material_ids.size())
				? sh.mesh.material_ids[f] : -1;

			tinyobj::index_t i0 = sh.mesh.indices[indexOffset + 0];
			tinyobj::index_t i1 = sh.mesh.indices[indexOffset + 1];
			tinyobj::index_t i2 = sh.mesh.indices[indexOffset + 2];

			XMFLOAT3 p0 = ReadPos(i0.vertex_index);
			XMFLOAT3 p1 = ReadPos(i1.vertex_index);
			XMFLOAT3 p2 = ReadPos(i2.vertex_index);

			XMFLOAT3 n0 = ReadNrm(i0.normal_index);
			XMFLOAT3 n1 = ReadNrm(i1.normal_index);
			XMFLOAT3 n2 = ReadNrm(i2.normal_index);

			// Если нормалей нет → face normal
			if (!hasNormals || i0.normal_index < 0 || i1.normal_index < 0 || i2.normal_index < 0)
			{
				XMVECTOR A = XMLoadFloat3(&p0), B = XMLoadFloat3(&p1), C = XMLoadFloat3(&p2);
				XMVECTOR fn = XMVector3Normalize(XMVector3Cross(B - A, C - A));
				XMStoreFloat3(&n0, fn); n1 = n0; n2 = n0;
			}

			XMFLOAT2 uv0 = ReadUV(i0.texcoord_index);
			XMFLOAT2 uv1 = ReadUV(i1.texcoord_index);
			XMFLOAT2 uv2 = ReadUV(i2.texcoord_index);

			auto& g = groups[matId];
			g.push_back(Vertex{ p0, n0, XMFLOAT4(1,1,1,1), uv0 });
			g.push_back(Vertex{ p1, n1, XMFLOAT4(1,1,1,1), uv1 });
			g.push_back(Vertex{ p2, n2, XMFLOAT4(1,1,1,1), uv2 });

			ExpandBounds(p0); ExpandBounds(p1); ExpandBounds(p2);
			indexOffset += 3;
		}
	}

	// ── 4) Сплющиваем в один VB, заполняем m_subMeshes ──────────────────────
	std::vector<Vertex> vertices;
	vertices.reserve(500000);
	m_subMeshes.clear();

	for (auto& [matId, verts] : groups)
	{
		SubMesh sm;
		sm.vertexOffset = static_cast<UINT>(vertices.size());
		sm.vertexCount  = static_cast<UINT>(verts.size());

		// srvIndex: если matId корректный и в matToSrv — берём оттуда, иначе 0
		if (matId >= 0 && matId < (int)matToSrv.size())
			sm.srvIndex = matToSrv[matId];
		else
			sm.srvIndex = 0; // белая заглушка

		m_subMeshes.push_back(sm);
		vertices.insert(vertices.end(), verts.begin(), verts.end());
	}

	if (vertices.empty())
		throw std::runtime_error("OBJ loaded but produced 0 vertices.");

	// ── 5) Центр и масштаб ───────────────────────────────────────────────────
	m_modelCenter =
	{
		0.5f * (minP.x + maxP.x),
		0.5f * (minP.y + maxP.y),
		0.5f * (minP.z + maxP.z)
	};
	{
		float dx = maxP.x - minP.x, dy = maxP.y - minP.y, dz = maxP.z - minP.z;
		float maxDim = dx;
		if (dy > maxDim) maxDim = dy;
		if (dz > maxDim) maxDim = dz;
		m_modelScale = (maxDim > 1e-6f) ? (2.0f / maxDim) : 1.0f;
	}

	// ── 6) Вершинный буфер в UPLOAD heap ────────────────────────────────────
	m_modelVertexCount = static_cast<UINT>(vertices.size());
	const UINT vbByteSize = m_modelVertexCount * sizeof(Vertex);

	D3D12_RESOURCE_DESC vbDesc{};
	vbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	vbDesc.Width            = vbByteSize;
	vbDesc.Height           = 1;
	vbDesc.DepthOrArraySize = 1;
	vbDesc.MipLevels        = 1;
	vbDesc.Format           = DXGI_FORMAT_UNKNOWN;
	vbDesc.SampleDesc.Count = 1;
	vbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES uploadProps{};
	uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadProps,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_modelVB.GetAddressOf())));

	void* mapped = nullptr;
	ThrowIfFailed(m_modelVB->Map(0, nullptr, &mapped));
	memcpy(mapped, vertices.data(), vbByteSize);
	m_modelVB->Unmap(0, nullptr);

	m_modelVBV.BufferLocation = m_modelVB->GetGPUVirtualAddress();
	m_modelVBV.StrideInBytes  = sizeof(Vertex);
	m_modelVBV.SizeInBytes    = vbByteSize;
}

void Framework::OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y)
{
	if (btnState & MK_RBUTTON)
	{
		m_rmbDown = true;
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;

		// ������ ����, ����� ������� ��� ���� ���� ����� �� ����
		SetCapture(hwnd);

		// �����������: ������ ������
		// ShowCursor(FALSE);
	}
}

void Framework::OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y)
{
	(void)btnState; (void)x; (void)y;

	if (m_rmbDown)
	{
		m_rmbDown = false;
		ReleaseCapture();

		// ����������� ������� ������
		// ShowCursor(TRUE);
	}
}

void Framework::OnMouseMove(HWND hwnd,	WPARAM btnState, int x, int y)
{
	(void)btnState;

	if (!m_rmbDown)
	{
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;
		return;
	}

	int dx = x - m_lastMousePos.x;
	int dy = y - m_lastMousePos.y;

	m_lastMousePos.x = x;
	m_lastMousePos.y = y;

	// yaw: ������ -> �������������
	m_yaw += dx * m_mouseSensitivity;

	// pitch: ����� ������ ��������� y (dy �������������),
	// ������� ������ "-" ����� ����� => ������������� pitch
	m_pitch -= dy * m_mouseSensitivity;

	// ��������� pitch, ����� �� ���������������� ������
	const float limit = DirectX::XM_PIDIV2 - 0.1f; // ~ 89�
	if (m_pitch > limit) m_pitch = limit;
	if (m_pitch < -limit) m_pitch = -limit;

	using namespace DirectX;

	// ������ "�����" �� yaw/pitch (LH �������)
	XMVECTOR forward = XMVectorSet(
		cosf(m_pitch) * sinf(m_yaw),
		sinf(m_pitch),
		cosf(m_pitch) * cosf(m_yaw),
		0.0f);

	forward = XMVector3Normalize(forward);

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMVECTOR tgt = pos + forward;

	XMStoreFloat3(&m_camTarget, tgt);
}
