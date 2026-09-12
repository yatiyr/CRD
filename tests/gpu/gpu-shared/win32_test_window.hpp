#pragma once

// win32_test_window.hpp — RET-2: a minimal REAL Win32 window for present-path gates. Isolated in its own TU so
// <windows.h> never touches the main test translation units. Returns the HWND as void* (the IPresentSurface
// native-window currency). No-ops (nullptr) on non-Windows platforms — callers skip.

namespace crd::gputest
{

// Create a plain top-level window of the given client size (message-pumped by pump; never requires user interaction).
[[nodiscard]] void* create_test_window(unsigned width, unsigned height);

// Drain pending messages (call between presents so the window stays responsive to the OS).
void pump_test_window();

void destroy_test_window(void* hwnd);

} // namespace crd::gputest
