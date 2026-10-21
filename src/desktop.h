#pragma once
#include <windows.h>
#include <vector>

namespace box {
// Read-only access to the Explorer desktop. No hooks or code are injected into Explorer.
HWND DesktopSurface();
struct DesktopGrid {
    SIZE cell{};
    int iconSize = 0;
    std::vector<RECT> occupied;
};
DesktopGrid ReadDesktopGrid(UINT dpi);
class WindowDpiScope {
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
public:
    explicit WindowDpiScope(HWND window) {
        if (window) previous_ = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
    }
    ~WindowDpiScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
};
}
