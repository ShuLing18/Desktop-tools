#include "desktop.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shlguid.h>
#include <algorithm>

namespace {
template<class T> struct Interface {
    T* value = nullptr;
    ~Interface() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
};
struct Search { DWORD shellProcess = 0; HWND surface = nullptr; };
BOOL CALLBACK FindSurface(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    if (process != search.shellProcess) return TRUE;
    HWND view = FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr);
    if (view && IsWindowVisible(view)) {
        HWND icons = FindWindowExW(view, nullptr, L"SysListView32", nullptr);
        search.surface = icons ? icons : view;
        return FALSE;
    }
    return TRUE;
}
}

namespace box {
HWND DesktopSurface() {
    Search search;
    HWND shell = GetShellWindow();
    if (!shell) return nullptr;
    GetWindowThreadProcessId(shell, &search.shellProcess);
    EnumWindows(FindSurface, reinterpret_cast<LPARAM>(&search));
    return search.surface;
}

DesktopGrid ReadDesktopGrid(UINT dpi, bool includeOccupied) {
    DesktopGrid result;
    ICONMETRICSW metrics{}; metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETICONMETRICS, sizeof(metrics), &metrics, 0, dpi))
        result.cell = {metrics.iHorzSpacing, metrics.iVertSpacing};
    else result.cell = {MulDiv(76, dpi, 96), MulDiv(100, dpi, 96)};
    result.iconSize = MulDiv(48, dpi, 96);
    // Explorer owns native grid placement; hidden owner windows need only metrics.
    if (!includeOccupied) return result;
    Interface<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, reinterpret_cast<void**>(windows.put())))) return result;
    VARIANT empty{}; VariantInit(&empty);
    long desktop = 0;
    Interface<IDispatch> dispatch;
    if (FAILED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH, dispatch.put())) || !dispatch.value) return result;
    Interface<IServiceProvider> services;
    Interface<IShellBrowser> browser;
    Interface<IShellView> view;
    Interface<IFolderView2> folder;
    if (FAILED(dispatch->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(services.put()))) ||
        FAILED(services->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(browser.put()))) ||
        FAILED(browser->QueryActiveShellView(view.put())) ||
        FAILED(view->QueryInterface(IID_IFolderView2, reinterpret_cast<void**>(folder.put())))) return result;
    POINT spacing{};
    if (SUCCEEDED(folder->GetSpacing(&spacing)) && spacing.x >= 32 && spacing.y >= 32 && spacing.x < 1024 && spacing.y < 1024)
        result.cell = {spacing.x, spacing.y};
    FOLDERVIEWMODE mode{}; int iconSize = 0;
    // Explorer exposes the view's nominal icon size in DIPs, while GetSpacing/GetItemPosition use view pixels.
    if (SUCCEEDED(folder->GetViewModeAndIconSize(&mode, &iconSize)) && iconSize >= 16 && iconSize <= 512) result.iconSize = MulDiv(iconSize, dpi, 96);
    // Shell item positions are expressed relative to the desktop list view.
    HWND surface = DesktopSurface();
    int count = 0;
    if (surface && SUCCEEDED(folder->ItemCount(SVGIO_ALLVIEW, &count))) {
        for (int index = 0; index < std::min(count, 10000); ++index) {
            PITEMID_CHILD item = nullptr;
            if (FAILED(folder->Item(index, &item))) continue;
            POINT position{}; const HRESULT positioned = folder->GetItemPosition(item, &position);
            CoTaskMemFree(item);
            if (SUCCEEDED(positioned)) {
                ClientToScreen(surface, &position);
                // GetItemPosition reports the image origin, not the grid-cell origin.
                // Convert to the nearest cell before checking occupancy, otherwise every icon falsely occupies two cells.
                MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
                GetMonitorInfoW(MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST), &monitor);
                position.x -= (result.cell.cx - result.iconSize) / 2;
                position.y -= 2;
                const LONG column = (position.x - monitor.rcWork.left + result.cell.cx / 2) / result.cell.cx;
                const LONG row = (position.y - monitor.rcWork.top + result.cell.cy / 2) / result.cell.cy;
                position = {monitor.rcWork.left + column * result.cell.cx, monitor.rcWork.top + row * result.cell.cy};
                result.occupied.push_back({position.x, position.y, position.x + result.cell.cx, position.y + result.cell.cy});
            }
        }
    }
    result.cell.cx = std::max<LONG>(result.cell.cx, result.iconSize + MulDiv(12, dpi, 96));
    result.cell.cy = std::max<LONG>(result.cell.cy, result.iconSize + MulDiv(40, dpi, 96));
    return result;
}
}
