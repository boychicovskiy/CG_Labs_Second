#ifndef FRAMEWORK_HPP
#define FRAMEWORK_HPP

#include <array>
#include <string>
#include <memory>
#include <vector>
#include <Windows.h>
#include <windowsx.h>

#include "Window.hpp"
#include "Timer.hpp"
#include "Dx12Common.hpp"
#include "RenderStructs.hpp"
#include "RenderingSystem.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Framework — «платформенный» слой приложения:
//   окно, DXGI, устройство, очередь команд, swap chain, depth-буфер,
//   камера и ввод.
//
// Всё, что касается собственно отрисовки сцены (G-Buffer, PSO, материалы,
// свет, проходы) вынесено в RenderingSystem — см. ДЗ №2 лекции 03.
// ─────────────────────────────────────────────────────────────────────────────
class Framework : public IWindowMessageHandler {
public:
	explicit Framework(int width, int height, const wchar_t* title);
	virtual ~Framework();

	bool Init();
	int  Run();

	LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

protected:
	virtual void CreateRtvAndDsvDescriptorHeaps();
	virtual void OnResize();
	virtual void Update(const double& dt);
	virtual void Draw();

	virtual void OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y);
	virtual void OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y);
	virtual void OnMouseMove(HWND hwnd, WPARAM btnState, int x, int y);

	HWND MainWnd()     const { return m_window ? m_window->GetHWND() : nullptr; }
	int  ClientWidth()  const { return m_clientWidth; }
	int  ClientHeight() const { return m_clientHeight; }

	Timer m_timer;

private:
	// ── Окно ────────────────────────────────────────────────────────────────
	int             m_initWidth  = 0;
	int             m_initHeight = 0;
	const wchar_t*  m_title      = nullptr;

	std::unique_ptr<Window> m_window;

	int m_clientWidth  = 0;
	int m_clientHeight = 0;

	bool m_appPaused = false;
	bool m_minimized = false;
	bool m_maximized = false;
	bool m_resizing  = false;

	HINSTANCE m_hInstance = nullptr;
	POINT     m_lastMousePos = { 0, 0 };

	// ── D3D12 ───────────────────────────────────────────────────────────────
	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGIAdapter1> m_dxgiAdapter;
	ComPtr<ID3D12Device>  m_device;
	std::wstring          m_adapterName;

	ComPtr<ID3D12CommandQueue>        m_commandQueue;
	ComPtr<ID3D12CommandAllocator>    m_directCmdListAlloc;
	ComPtr<ID3D12GraphicsCommandList> m_commandList;

	ComPtr<ID3D12Fence> m_fence;
	UINT64              m_currentFence = 0;
	HANDLE              m_fenceEvent   = nullptr;

	static const int SwapChainBufferCount = 2;

	ComPtr<IDXGISwapChain4> m_swapChain;
	int                     m_currBackBuffer = 0;

	// Swap chain обязан быть в «сыром» UNORM: flip-модель не принимает
	// _SRGB-формат для самой цепочки. Зато RTV на те же буферы можно создать
	// с форматом _SRGB — тогда гамма-кодирование делает аппаратура
	// (лекция 08.1, слайд 43).
	DXGI_FORMAT m_backBufferFormat    = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT m_backBufferRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

	// Depth хранится в TYPELESS-формате: DSV смотрит на него как
	// D24_UNORM_S8_UINT, а GBuffer создаёт SRV как R24_UNORM_X8_TYPELESS,
	// чтобы light pass мог восстановить мировые координаты из глубины.
	DXGI_FORMAT m_depthStencilResourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
	DXGI_FORMAT m_depthStencilFormat         = DXGI_FORMAT_D24_UNORM_S8_UINT;

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

	UINT m_rtvDescriptorSize       = 0;
	UINT m_dsvDescriptorSize       = 0;
	UINT m_cbvSrvUavDescriptorSize = 0;

	ComPtr<ID3D12Resource> m_swapChainBuffer[SwapChainBufferCount];
	ComPtr<ID3D12Resource> m_depthStencilBuffer;

	D3D12_VIEWPORT m_screenViewport = {};
	D3D12_RECT     m_scissorRect    = {};

	// ── Рендер ──────────────────────────────────────────────────────────────
	std::unique_ptr<RenderingSystem> m_renderer;

	// ── Ввод ────────────────────────────────────────────────────────────────
	std::array<bool, 256> m_keyDown{};
	std::array<bool, 256> m_keyDownPrev{};

	// ── Камера ──────────────────────────────────────────────────────────────
	float m_cameraMoveSpeed = 3.0f;

	DirectX::XMFLOAT3 m_camPos    = { 2.0f, 2.0f, -5.0f };
	DirectX::XMFLOAT3 m_camTarget = { 0.0f, 0.0f,  0.0f };
	DirectX::XMFLOAT3 m_camUp     = { 0.0f, 1.0f,  0.0f };

	bool  m_rmbDown          = false;
	float m_yaw              = 0.0f;
	float m_pitch            = 0.0f;
	float m_mouseSensitivity = 0.0025f;

	// ── Инициализация ───────────────────────────────────────────────────────
	void InitDxgi();
	void PickAdapter();
	void LogAdapters();
	void LogAdapterOutputs(IDXGIAdapter1* adapter);
	void InitD3D12Device();
	void CreateCommandObjects();
	void CreateFence();
	void FlushCommandQueue();
	void CreateSwapChain();

	void HandleKeyboardShortcuts();
	void UpdateWindowTitle(double dt);

	// Накопители для усреднения FPS в заголовке окна
	double m_titleAccum  = 0.0;
	int    m_titleFrames = 0;

	ID3D12Resource* CurrentBackBuffer() const {
		return m_swapChainBuffer[m_currBackBuffer].Get();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const {
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(m_currBackBuffer) * m_rtvDescriptorSize;
		return h;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const {
		return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	}
};

#endif // FRAMEWORK_HPP
