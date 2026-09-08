#include "Framework.hpp"

#include <DirectXColors.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <cmath>
#include <string>

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

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════
bool Framework::Init() {
	m_window = std::make_unique<Window>(m_initWidth, m_initHeight, m_title, this);

	InitDxgi();
	InitD3D12Device();

	m_cbvSrvUavDescriptorSize =
		m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	CreateCommandObjects();
	CreateFence();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();

	// Вся работа по сцене и проходам — внутри RenderingSystem
	m_renderer = std::make_unique<RenderingSystem>();
	m_renderer->Init(
		m_device.Get(),
		m_commandQueue.Get(),
		m_directCmdListAlloc.Get(),
		m_commandList.Get(),
		m_backBufferFormat,
		m_depthStencilFormat);

	// OnResize создаёт depth-буфер и отдаёт его в G-Buffer
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

// ═════════════════════════════════════════════════════════════════════════════
// Оконная процедура
// ═════════════════════════════════════════════════════════════════════════════
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
				// ждём WM_EXITSIZEMOVE
			}
			else {
				OnResize();
			}
		}
		return 0;

	case WM_ACTIVATEAPP:
		if (wParam == FALSE) {
			m_appPaused = true;
			m_timer.Stop();
		}
		else {
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

	// Потеря фокуса — сбрасываем все «залипшие» клавиши
	case WM_KILLFOCUS:
		m_keyDown.fill(false);
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ═════════════════════════════════════════════════════════════════════════════
// Кучи дескрипторов RTV/DSV
// ═════════════════════════════════════════════════════════════════════════════
void Framework::CreateRtvAndDsvDescriptorHeaps()
{
	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
}

// ═════════════════════════════════════════════════════════════════════════════
// OnResize
// ═════════════════════════════════════════════════════════════════════════════
void Framework::OnResize()
{
	if (!m_device || !m_swapChain || !m_commandQueue || !m_directCmdListAlloc || !m_commandList)
		return;

	FlushCommandQueue();

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	for (UINT i = 0; i < SwapChainBufferCount; ++i)
		m_swapChainBuffer[i].Reset();

	m_depthStencilBuffer.Reset();

	ThrowIfFailed(m_swapChain->ResizeBuffers(
		SwapChainBufferCount, m_clientWidth, m_clientHeight, m_backBufferFormat, 0));

	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffer[i])));
		m_device->CreateRenderTargetView(m_swapChainBuffer[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_rtvDescriptorSize;
	}

	// ── Depth-буфер ─────────────────────────────────────────────────────────
	// Формат ресурса TYPELESS: одно и то же хранилище нужно одновременно
	// и как DSV (запись глубины в geometry pass), и как SRV (чтение глубины
	// в light pass для восстановления мировых координат).
	D3D12_RESOURCE_DESC depthDesc = {};
	depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Alignment        = 0;
	depthDesc.Width            = static_cast<UINT64>(m_clientWidth);
	depthDesc.Height           = static_cast<UINT>(m_clientHeight);
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels        = 1;
	depthDesc.Format           = m_depthStencilResourceFormat;   // R24G8_TYPELESS
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear = {};
	optClear.Format               = m_depthStencilFormat;        // D24_UNORM_S8_UINT
	optClear.DepthStencil.Depth   = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask     = 1;
	heapProps.VisibleNodeMask      = 1;

	ThrowIfFailed(m_device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
		D3D12_RESOURCE_STATE_COMMON, &optClear,
		IID_PPV_ARGS(&m_depthStencilBuffer)));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags         = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format        = m_depthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, DepthStencilView());

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = m_depthStencilBuffer.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &barrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdsLists);
	FlushCommandQueue();

	// G-Buffer пересоздаётся под новый размер и получает новый depth-ресурс
	if (m_renderer)
		m_renderer->OnResize(m_device.Get(),
		                     static_cast<UINT>(m_clientWidth),
		                     static_cast<UINT>(m_clientHeight),
		                     m_depthStencilBuffer.Get());

	m_screenViewport.TopLeftX = 0.0f;
	m_screenViewport.TopLeftY = 0.0f;
	m_screenViewport.Width    = static_cast<float>(m_clientWidth);
	m_screenViewport.Height   = static_cast<float>(m_clientHeight);
	m_screenViewport.MinDepth = 0.0f;
	m_screenViewport.MaxDepth = 1.0f;

	m_scissorRect = { 0, 0, m_clientWidth, m_clientHeight };
}

// ═════════════════════════════════════════════════════════════════════════════
// Горячие клавиши (edge-detect: срабатывают один раз на нажатие)
// ═════════════════════════════════════════════════════════════════════════════
void Framework::HandleKeyboardShortcuts()
{
	auto JustPressed = [&](uint8_t vk) {
		return m_keyDown[vk] && !m_keyDownPrev[vk];
	};

	if (!m_renderer) return;

	if (JustPressed('T'))       m_renderer->ToggleUvAnimation();
	if (JustPressed(VK_OEM_4))  m_renderer->ScaleTiling(0.5f);   // [
	if (JustPressed(VK_OEM_6))  m_renderer->ScaleTiling(2.0f);   // ]
	if (JustPressed('0'))       m_renderer->ResetUv();

	// F1..F5 — показать отдельный канал G-Buffer
	for (uint8_t i = 0; i < 5; ++i)
		if (JustPressed(static_cast<uint8_t>(VK_F1 + i)))
			m_renderer->SetDebugMode(static_cast<uint32_t>(i + 1));

	if (JustPressed(VK_OEM_PLUS))  m_renderer->ScaleLightIntensity(1.25f);
	if (JustPressed(VK_OEM_MINUS)) m_renderer->ScaleLightIntensity(0.8f);

	// ── ДЗ №3: тесселяция и карты нормалей ──────────────────────────────────
	if (JustPressed('G')) m_renderer->ToggleTessellation();
	if (JustPressed('F')) m_renderer->ToggleWireframe();
	if (JustPressed('N')) m_renderer->ToggleNormalMapping();
	if (JustPressed('B')) m_renderer->ToggleGreenChannelFlip();
	if (JustPressed('C')) m_renderer->ToggleHullBackfaceCulling();

	if (JustPressed(VK_OEM_COMMA))  m_renderer->ScaleDisplacement(0.75f);   // ,
	if (JustPressed(VK_OEM_PERIOD)) m_renderer->ScaleDisplacement(1.33f);   // .

	if (JustPressed(VK_PRIOR)) m_renderer->ScaleMaxTessFactor(+1.0f);       // PageUp
	if (JustPressed(VK_NEXT))  m_renderer->ScaleMaxTessFactor(-1.0f);       // PageDown
}

// ═════════════════════════════════════════════════════════════════════════════
// Update — камера + делегирование в RenderingSystem
// ═════════════════════════════════════════════════════════════════════════════
void Framework::Update(const double& dt)
{
	HandleKeyboardShortcuts();
	m_keyDownPrev = m_keyDown;

	// ── Движение камеры ─────────────────────────────────────────────────────
	XMVECTOR pos    = XMLoadFloat3(&m_camPos);
	XMVECTOR target = XMLoadFloat3(&m_camTarget);
	XMVECTOR up     = XMVector3Normalize(XMLoadFloat3(&m_camUp));

	XMVECTOR forward = XMVector3Normalize(target - pos);
	XMVECTOR right   = XMVector3Normalize(XMVector3Cross(up, forward));

	float speed = m_cameraMoveSpeed;
	if (m_keyDown[VK_SHIFT]) speed *= 3.0f;

	const float step = speed * static_cast<float>(dt);
	XMVECTOR move = XMVectorZero();

	if (m_keyDown['W']) move += forward;
	if (m_keyDown['S']) move -= forward;
	if (m_keyDown['D']) move += right;
	if (m_keyDown['A']) move -= right;
	if (m_keyDown[VK_SPACE])   move += up;
	if (m_keyDown[VK_CONTROL]) move -= up;

	if (!XMVector3Equal(move, XMVectorZero()))
		move = XMVector3Normalize(move) * step;

	pos    += move;
	target += move;

	XMStoreFloat3(&m_camPos, pos);
	XMStoreFloat3(&m_camTarget, target);

	// ── Матрицы ─────────────────────────────────────────────────────────────
	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);

	const float aspect = (m_clientHeight > 0)
		? static_cast<float>(m_clientWidth) / static_cast<float>(m_clientHeight)
		: 1.0f;

	XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.05f, 100.0f);

	if (m_renderer)
		m_renderer->Update(dt, view, proj, m_camPos,
		                   static_cast<UINT>(m_clientWidth),
		                   static_cast<UINT>(m_clientHeight));
}

// ═════════════════════════════════════════════════════════════════════════════
// Draw
// ═════════════════════════════════════════════════════════════════════════════
void Framework::Draw()
{
	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	D3D12_RESOURCE_BARRIER toRT = {};
	toRT.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRT.Transition.pResource   = CurrentBackBuffer();
	toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toRT);

	// Оба прохода deferred-рендера
	if (m_renderer)
		m_renderer->Render(m_commandList.Get(),
		                   CurrentBackBufferView(),
		                   DepthStencilView(),
		                   m_screenViewport,
		                   m_scissorRect);

	D3D12_RESOURCE_BARRIER toPresent = {};
	toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toPresent.Transition.pResource   = CurrentBackBuffer();
	toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
	toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toPresent);

	ThrowIfFailed(m_commandList->Close());

	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(m_swapChain->Present(0, 0));
	m_currBackBuffer = (m_currBackBuffer + 1) % SwapChainBufferCount;

	// Простая синхронизация «кадр в кадр». Не оптимально (см. лекцию 05),
	// но исключает гонки при использовании одного командного аллокатора.
	FlushCommandQueue();
}

// ═════════════════════════════════════════════════════════════════════════════
// DXGI / устройство / очередь / swap chain
// ═════════════════════════════════════════════════════════════════════════════
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

			if (factory6->EnumAdapterByGpuPreference(
					i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
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

	if (!m_dxgiAdapter)
		throw std::runtime_error("No suitable DXGI adapter found (D3D12-capable).");

#if defined(_DEBUG)
	OutputDebugStringW((L"[DXGI] Using adapter: " + m_adapterName + L"\n").c_str());
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

		DXGI_ADAPTER_DESC1 desc = {};
		ThrowIfFailed(adapter->GetDesc1(&desc));

		std::wstring line = L"  -  ";
		line += desc.Description;
		line += (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" (SOFTWARE)\n" : L"\n";
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

		std::wstring line = L"        Output: ";
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
		OutputDebugStringW(L"[D3D12] Debug layer enabled\n");
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
	OutputDebugStringW(L"[D3D12] Device created\n");

	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_device.As(&infoQueue))) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
	}
#endif
}

void Framework::CreateCommandObjects() {
	D3D12_COMMAND_QUEUE_DESC qdesc = {};
	qdesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
	qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(m_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_commandQueue)));
	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_directCmdListAlloc)));
	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_directCmdListAlloc.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));

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
	sd.Width              = m_clientWidth;
	sd.Height             = m_clientHeight;
	sd.Format             = m_backBufferFormat;
	sd.SampleDesc.Count   = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount        = SwapChainBufferCount;
	sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling            = DXGI_SCALING_STRETCH;
	sd.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
	sd.Flags              = 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(
		m_commandQueue.Get(), MainWnd(), &sd, nullptr, nullptr, &swapChain1));
	ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(MainWnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain1.As(&m_swapChain));
	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());
}

// ═════════════════════════════════════════════════════════════════════════════
// Мышь
// ═════════════════════════════════════════════════════════════════════════════
void Framework::OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y)
{
	if (btnState & MK_RBUTTON)
	{
		m_rmbDown = true;
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;
		SetCapture(hwnd);
	}
}

void Framework::OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y)
{
	(void)hwnd; (void)btnState; (void)x; (void)y;

	if (m_rmbDown)
	{
		m_rmbDown = false;
		ReleaseCapture();
	}
}

void Framework::OnMouseMove(HWND hwnd, WPARAM btnState, int x, int y)
{
	(void)hwnd; (void)btnState;

	if (!m_rmbDown)
	{
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;
		return;
	}

	const int dx = x - m_lastMousePos.x;
	const int dy = y - m_lastMousePos.y;

	m_lastMousePos.x = x;
	m_lastMousePos.y = y;

	m_yaw   += dx * m_mouseSensitivity;
	m_pitch -= dy * m_mouseSensitivity;

	const float limit = XM_PIDIV2 - 0.1f;   // ~89°
	m_pitch = (m_pitch >  limit) ?  limit : m_pitch;
	m_pitch = (m_pitch < -limit) ? -limit : m_pitch;

	XMVECTOR forward = XMVector3Normalize(XMVectorSet(
		cosf(m_pitch) * sinf(m_yaw),
		sinf(m_pitch),
		cosf(m_pitch) * cosf(m_yaw),
		0.0f));

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMStoreFloat3(&m_camTarget, pos + forward);
}
