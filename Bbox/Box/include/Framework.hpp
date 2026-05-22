#ifndef FRAMEWORK_HPP
#define FRAMEWORK_HPP

#include <array>
#include <string>
#include <memory>
#include <Windows.h>
#include <windowsx.h>
#include "Window.hpp"
#include "Timer.hpp"
#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include "RenderStructs.hpp"

class Framework : public IWindowMessageHandler {
public:
	explicit Framework(int width, int height, const wchar_t* title);
	virtual ~Framework();

	bool Init();
	int Run();

	LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

protected:
	virtual void CreateRtvAndDsvDescriptorHeaps();
	virtual void OnResize();
	virtual void Update(const double& dt);
	virtual void Draw();

	virtual void OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y);
	virtual void OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y);
	virtual void OnMouseMove(HWND hwnd, WPARAM btnState, int x, int y);

	HWND MainWnd() const { return m_window ? m_window->GetHWND() : nullptr; }
	int ClientWidth() const { return m_clientWidth; }
	int ClientHeight() const { return m_clientHeight; }

	Timer m_timer;

private:
	int m_initWidth = 0;
	int m_initHeight = 0;
	const wchar_t* m_title = nullptr;

	std::unique_ptr<Window> m_window;

	int m_clientWidth = 0;
	int m_clientHeight = 0;

	bool m_appPaused = false;
	bool m_minimized = false;
	bool m_maximized = false;
	bool m_resizing = false;

	HINSTANCE m_hInstance = nullptr;

	POINT m_lastMousePos = { 0,0 };

	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGIAdapter1> m_dxgiAdapter;
	ComPtr<ID3D12Device> m_device;
	std::wstring m_adapterName;

	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<ID3D12CommandAllocator> m_directCmdListAlloc;
	ComPtr<ID3D12GraphicsCommandList> m_commandList;

	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_currentFence = 0;
	HANDLE m_fenceEvent = nullptr;

	static const int SwapChainBufferCount = 2;

	ComPtr<IDXGISwapChain4> m_swapChain;
	int m_currBackBuffer = 0;

	DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

	UINT m_rtvDescriptorSize = 0;
	UINT m_dsvDescriptorSize = 0;
	UINT m_cbvSrvUavDescriptorSize = 0;

	ComPtr<ID3D12Resource> m_swapChainBuffer[SwapChainBufferCount];
	ComPtr<ID3D12Resource> m_depthStencilBuffer;

	DXGI_FORMAT m_depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	D3D12_VIEWPORT m_screenViewport = {};
	D3D12_RECT m_scissorRect = {};

	ComPtr<ID3DBlob> m_vsByteCode;
	ComPtr<ID3DBlob> m_psByteCode;

	std::unique_ptr<UploadBuffer<ObjectConstants>> m_objectCB;
	std::unique_ptr<UploadBuffer<PassConstants>>   m_passCB;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cbvHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;

	void InitDxgi();
	void PickAdapter();
	void LogAdapters();
	void LogAdapterOutputs(IDXGIAdapter1* adapter);
	void InitD3D12Device();
	void CreateCommandObjects();
	void CreateFence();
	void FlushCommandQueue();
	void CreateSwapChain();
	void BuildShaders();
	void BuildConstantBuffers();
	void BuildCbvHeap();
	void BuildCbvViews();
	void BuildRootSignature();
	void BuildPSO();
	void BuildObjVB_Upload();

	void BuildBoxGeometry();

	ComPtr<ID3D12Resource> m_boxVB;
	ComPtr<ID3D12Resource> m_boxIB;

	ComPtr<ID3D12Resource> m_boxVBUpload;
	ComPtr<ID3D12Resource> m_boxIBUpload;

	D3D12_VERTEX_BUFFER_VIEW m_boxVBView = {};
	D3D12_INDEX_BUFFER_VIEW  m_boxIBView = {};

	UINT m_boxIndexCount = 0;
	
	Microsoft::WRL::ComPtr<ID3D12Resource> m_modelVB;
	D3D12_VERTEX_BUFFER_VIEW m_modelVBV{};
	UINT m_modelVertexCount = 0;

	DirectX::XMFLOAT3 m_modelCenter = { 0.0f, 0.0f, 0.0f };
	float m_modelScale = 1.0f;
	std::array<bool, 256> m_keyDown{}; // состояние VK_*

	float m_cameraMoveSpeed = 3.0f;   // units/sec, подстрой под сцену

	DirectX::XMFLOAT3 m_camPos = { 2.0f, 2.0f, -5.0f };
	DirectX::XMFLOAT3 m_camTarget = { 0.0f, 0.0f,  0.0f };
	DirectX::XMFLOAT3 m_camUp = { 0.0f, 1.0f,  0.0f };

	// --- Mouse look state ---
	bool  m_rmbDown = false;

	// углы камеры
	float m_yaw = 0.0f;   // поворот вокруг Y
	float m_pitch = 0.0f;   // наклон вверх/вниз

	// чувствительность мыши
	float m_mouseSensitivity = 0.0025f; // радиан на пиксель (подстрой)

	// дистанция до target (если хочешь "orbital"), для FPS не нужна
	// float m_camDistance = 5.0f;


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