#include <Windows.h>
#include <exception>
#include "Framework.hpp"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    try
    {
        Framework app(1280, 720, L"CG Window");
        if (!app.Init()) return 0;
        return app.Run();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Fatal error", MB_OK | MB_ICONERROR);
        return -1;
    }

}
