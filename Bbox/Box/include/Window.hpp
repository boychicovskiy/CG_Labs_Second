#ifndef WINDOW_HPP
#define WINDOW_HPP

#include <Windows.h>

struct IWindowMessageHandler {
	virtual ~IWindowMessageHandler() = default;
	virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;
};

class Window {
public:
	Window(int width, int height, const wchar_t* title, IWindowMessageHandler* handler = nullptr);
	~Window();

	bool ProcessMessages();

	HWND GetHWND() const;


private:
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	HWND m_hwnd = nullptr;
	bool m_running = true;
	IWindowMessageHandler* m_handler = nullptr;
};


#endif // WINDOW_HPP