#include "model.h"
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <climits>

namespace box {
bool InPanelTransit(POINT point, RECT icon, RECT panel) {
    // Only the narrow gap between facing edges is protected. This never enlarges
    // the icon's opening target or the blank area around an expanded panel.
    RECT gap{};
    if (icon.right <= panel.left) gap = {icon.right, std::max(icon.top, panel.top), panel.left, std::min(icon.bottom, panel.bottom)};
    else if (panel.right <= icon.left) gap = {panel.right, std::max(icon.top, panel.top), icon.left, std::min(icon.bottom, panel.bottom)};
    else if (icon.bottom <= panel.top) gap = {std::max(icon.left, panel.left), icon.bottom, std::min(icon.right, panel.right), panel.top};
    else if (panel.bottom <= icon.top) gap = {std::max(icon.left, panel.left), panel.bottom, std::min(icon.right, panel.right), icon.top};
    return !IsRectEmpty(&gap) && PtInRect(&gap, point);
}
bool MatchesSearch(const std::wstring& name, const std::wstring& query) {
    // Match every space-separated term, ignoring case, without touching the filesystem.
    size_t start = 0;
    while (start < query.size()) {
        start = query.find_first_not_of(L" \t\r\n\u3000", start);
        if (start == std::wstring::npos) break;
        size_t end = query.find_first_of(L" \t\r\n\u3000", start);
        if (end == std::wstring::npos) end = query.size();
        if (FindStringOrdinal(FIND_FROMSTART, name.c_str(), static_cast<int>(name.size()),
                              query.data() + start, static_cast<int>(end - start), TRUE) < 0) return false;
        start = end;
    }
    return true;
}
double LayoutEase(ULONGLONG elapsed, ULONGLONG duration) {
    if (!duration || elapsed >= duration) return 1.;
    const double remaining = 1. - static_cast<double>(elapsed) / static_cast<double>(duration);
    return 1. - remaining * remaining * remaining;
}
RECT InterpolateRect(RECT from, RECT to, double progress) {
    auto value = [progress](LONG a, LONG b) { return static_cast<LONG>(a + (b - a) * progress); };
    return {value(from.left, to.left), value(from.top, to.top), value(from.right, to.right), value(from.bottom, to.bottom)};
}
Placement PlacePanel(RECT anchor, RECT work, int width, int height, int gap, Direction direction) {
    width = std::min(width, static_cast<int>(work.right - work.left));
    height = std::min(height, static_cast<int>(work.bottom - work.top));
    const int rightSpace = work.right - anchor.right - gap;
    const int leftSpace = anchor.left - work.left - gap;
    const int aboveSpace = anchor.top - work.top - gap;
    const int belowSpace = work.bottom - anchor.bottom - gap;
    if (direction == Direction::Automatic) {
        if (rightSpace >= width) direction = Direction::Right;
        else if (leftSpace >= width) direction = Direction::Left;
        else if (belowSpace >= height) direction = Direction::Down;
        else if (aboveSpace >= height) direction = Direction::Up;
        else direction = leftSpace > rightSpace ? Direction::Left : Direction::Right;
    }
    if (direction == Direction::Left && leftSpace < width && rightSpace >= width) direction = Direction::Right;
    else if (direction == Direction::Right && rightSpace < width && leftSpace >= width) direction = Direction::Left;
    else if (direction == Direction::Up && aboveSpace < height && belowSpace >= height) direction = Direction::Down;
    else if (direction == Direction::Down && belowSpace < height && aboveSpace >= height) direction = Direction::Up;
    int x = anchor.left, y = anchor.top;
    if (direction == Direction::Left) x = anchor.left - gap - width;
    else if (direction == Direction::Right) x = anchor.right + gap;
    else if (direction == Direction::Up) y = anchor.top - gap - height;
    else y = anchor.bottom + gap;
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    return {{x, y, x + width, y + height}, direction == Direction::Left, direction};
}
POINT ClampAnchor(POINT point, SIZE size, RECT work) {
    return {std::clamp(point.x, work.left, std::max(work.left, work.right - size.cx)),
            std::clamp(point.y, work.top, std::max(work.top, work.bottom - size.cy))};
}
POINT SnapAnchor(POINT point, SIZE cell, RECT work, const std::vector<RECT>& occupied) {
    if (cell.cx <= 0 || cell.cy <= 0) return ClampAnchor(point, cell, work);
    const int columns = std::max(1L, (work.right - work.left) / cell.cx);
    const int rows = std::max(1L, (work.bottom - work.top) / cell.cy);
    POINT nearest = ClampAnchor(point, cell, work);
    long long best = LLONG_MAX;
    for (int column = 0; column < columns; ++column) for (int row = 0; row < rows; ++row) {
        POINT candidate{work.left + column * cell.cx, work.top + row * cell.cy};
        RECT bounds{candidate.x, candidate.y, candidate.x + cell.cx, candidate.y + cell.cy};
        bool free = true;
        for (const auto& used : occupied) {
            RECT intersection{};
            if (IntersectRect(&intersection, &bounds, &used)) { free = false; break; }
        }
        if (!free) continue;
        const long long dx = static_cast<long long>(candidate.x) - point.x, dy = static_cast<long long>(candidate.y) - point.y;
        const long long distance = dx * dx + dy * dy;
        if (distance < best) { best = distance; nearest = candidate; }
    }
    return ClampAnchor(nearest, cell, work);
}
std::wstring SafeName(std::wstring name) {
    for (auto& c : name) if (c < 32 || std::wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"快捷方式";
    std::wstring upper = name.substr(0, name.find(L'.'));
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return std::towupper(c); });
    if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL" ||
        (upper.size() == 4 && (upper.substr(0, 3) == L"COM" || upper.substr(0, 3) == L"LPT") && upper[3] >= L'1' && upper[3] <= L'9')) name = L"_" + name;
    if (name.size() > 100) name.resize(100);
    return name;
}
std::wstring FullPath(const std::wstring& path) {
    wchar_t small[MAX_PATH];
    DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, small, nullptr);
    if (!length) throw HRESULT_FROM_WIN32(GetLastError());
    if (length < MAX_PATH) return {small, length};
    if (length > 32768) throw HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    std::wstring result(length, L'\0');
    length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
    if (!length || length >= result.size()) throw HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    result.resize(length); return result;
}
std::wstring BaseName(const std::wstring& path) {
    size_t end = path.find_last_not_of(L"\\/");
    if (end == std::wstring::npos) return L"磁盘";
    const size_t start = path.find_last_of(L"\\/", end);
    return path.substr(start == std::wstring::npos ? 0 : start + 1, end - (start == std::wstring::npos ? 0 : start + 1) + 1);
}
std::wstring ParentPath(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    if (pos == 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, pos);
}
bool IsLink(const std::wstring& path) {
    const auto pos = path.find_last_of(L'.');
    if (pos == std::wstring::npos) return false;
    const auto ext = path.substr(pos);
    return _wcsicmp(ext.c_str(), L".lnk") == 0 || _wcsicmp(ext.c_str(), L".url") == 0;
}
std::wstring UniquePath(const std::wstring& directory, const std::wstring& stem, const std::wstring& extension) {
    const auto safe = SafeName(stem);
    for (int i = 0; i < 10000; ++i) {
        auto path = directory + L"\\" + safe + (i == 0 ? L"" : L" (" + std::to_wstring(i + 1) + L")") + extension;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    }
    return L"";
}
}
