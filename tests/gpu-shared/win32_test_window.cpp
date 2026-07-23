// win32_test_window.cpp — RET-2: the REAL-window helper for present gates. <windows.h> lives HERE and nowhere else.

#include "win32_test_window.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace crd::gputest
{

namespace
{
const wchar_t* const kClassName = L"CeridPresentGateWindow";

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

void* create_test_window(unsigned width, unsigned height)
{
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW       wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClassName;
    (void)RegisterClassW(&wc); // idempotent across tests — a re-register failure is fine, the class exists

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    (void)AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kClassName, L"cerid present gate", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, inst,
                                nullptr);
    if (hwnd != nullptr) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); }
    return hwnd;
}

void pump_test_window()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void destroy_test_window(void* hwnd)
{
    if (hwnd != nullptr) { DestroyWindow(static_cast<HWND>(hwnd)); }
    pump_test_window();
}

} // namespace crd::gputest

#else // non-Windows: the helper reports "no window" and the gate skips (Linux present rides the RET linux sweep)

namespace crd::gputest
{
void* create_test_window(unsigned, unsigned) { return nullptr; }
void  pump_test_window() {}
void  destroy_test_window(void*) {}
} // namespace crd::gputest

#endif
