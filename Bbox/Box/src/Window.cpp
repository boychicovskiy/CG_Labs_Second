#include "Window.hpp"

Window::Window(int width, int height, const wchar_t* title, IWindowMessageHandler* handler) : m_handler(handler) 
{
	WNDCLASS window_class = {};
	window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
	window_class.hInstance = GetModuleHandle(nullptr);
	window_class.lpszClassName = L"WindowClass";
	window_class.style = CS_HREDRAW | CS_VREDRAW;
	window_class.lpfnWndProc = WndProc;

	RegisterClass(&window_class);

	m_hwnd = CreateWindowW(
		window_class.lpszClassName,
		title,
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		width,
		height,
		nullptr,
		nullptr,
		window_class.hInstance,
		this
	);
}

Window::~Window() {
	if (m_hwnd)
		DestroyWindow(m_hwnd);
}

bool Window::ProcessMessages() {
	MSG msg = {};

	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
		if (msg.message == WM_QUIT) {
			m_running = false;
			return false;
		}

		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return m_running;
}

HWND Window::GetHWND() const {
	return m_hwnd;
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	Window* window = nullptr;

	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
		window = static_cast<Window*>(cs->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));

		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	window = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

	if (window && window->m_handler)
		return window->m_handler->MsgProc(hwnd, msg, wParam, lParam);

	return DefWindowProc(hwnd, msg, wParam, lParam);
}