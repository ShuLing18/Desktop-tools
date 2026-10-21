#include <windows.h>
#include <ole2.h>
#include <cstdio>
#include "../src/desktop.h"
#include "../src/model.h"
#include "../src/native_entry.h"

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    HWND surface = box::DesktopSurface();
    const UINT dpi = surface ? GetDpiForWindow(surface) : GetDpiForSystem();
    const auto grid = box::ReadDesktopGrid(dpi);
    wchar_t name[64]{}; GetClassNameW(surface, name, 64);
    std::printf("desktop=%ls dpi=%u cell=%ldx%ld icon=%d occupied=%zu\n", name, dpi, grid.cell.cx, grid.cell.cy, grid.iconSize, grid.occupied.size());
    for (size_t i = 0; i < grid.occupied.size() && i < 20; ++i) {
        const auto r = grid.occupied[i]; std::printf("item[%zu]=%ld,%ld..%ld,%ld\n", i, r.left, r.top, r.right, r.bottom);
    }
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({80, 180}, MONITOR_DEFAULTTONEAREST), &monitor);
    for (const auto point : {POINT{80,180}, POINT{670,465}, POINT{1,1}, POINT{1800,1000}}) {
        const auto snapped = box::SnapAnchor(point, grid.cell, monitor.rcWork, grid.occupied);
        std::printf("snap %ld,%ld -> %ld,%ld\n", point.x, point.y, snapped.x, snapped.y);
    }
    wchar_t executable[32768]{}, shortcut[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    const auto ini = box::ParentPath(executable) + L"\\data\\settings.ini";
    GetPrivateProfileStringW(L"DesktopBox", L"DesktopLink", L"", shortcut, 32768, ini.c_str());
    {
        box::NativeEntry native(shortcut); RECT cell{}, image{};
        const bool located = native.Bounds(cell, image);
        std::printf("native_entry located=%d cell=%ld,%ld..%ld,%ld image=%ld,%ld..%ld,%ld image_size=%ldx%ld\n", located,
            cell.left, cell.top, cell.right, cell.bottom, image.left, image.top, image.right, image.bottom, image.right - image.left, image.bottom - image.top);
    }
    OleUninitialize();
    return surface && !grid.occupied.empty() ? 0 : 1;
}
