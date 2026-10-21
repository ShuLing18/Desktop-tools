#include "fluent_settings.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>

namespace box {
namespace {
constexpr COLORREF kBackground = RGB(247, 248, 250), kContent = RGB(255, 255, 255);
constexpr COLORREF kText = RGB(32, 33, 36), kMuted = RGB(107, 114, 128);
constexpr COLORREF kBorder = RGB(229, 231, 235), kAccent = RGB(38, 119, 232);
constexpr COLORREF kHover = RGB(240, 244, 248), kDisabled = RGB(163, 170, 180);

Gdiplus::Color Color(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}
void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void Rounded(HDC dc, RECT rect, float radius, COLORREF fill, COLORREF stroke, float width = 1.0f) {
    using namespace Gdiplus;
    Graphics graphics(dc); graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const float inset = width / 2.0f;
    const float x = rect.left + inset, y = rect.top + inset;
    const float w = rect.right - rect.left - width, h = rect.bottom - rect.top - width;
    const float diameter = std::min(radius * 2, std::min(w, h));
    if (w <= 0 || h <= 0) return;
    GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + w - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + w - diameter, y + h - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + h - diameter, diameter, diameter, 90, 90); path.CloseFigure();
    SolidBrush brush(Color(fill)); graphics.FillPath(&brush, &path);
    if (width > 0) { Pen pen(Color(stroke), width); graphics.DrawPath(&pen, &path); }
}
void Text(HDC dc, HFONT font, const std::wstring& text, RECT rect, COLORREF color, UINT flags = DT_LEFT) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              flags | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}
std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(length + 1, L'\0'); GetWindowTextW(window, text.data(), length + 1);
    text.resize(length); return text;
}
class Buffer {
    HDC target_, dc_;
    HBITMAP bitmap_;
    HGDIOBJ previous_;
    RECT rect_;
public:
    Buffer(HDC target, RECT rect) : target_(target), rect_(rect) {
        dc_ = CreateCompatibleDC(target);
        bitmap_ = CreateCompatibleBitmap(target, std::max(1L, rect.right), std::max(1L, rect.bottom));
        previous_ = SelectObject(dc_, bitmap_);
    }
    ~Buffer() {
        BitBlt(target_, 0, 0, rect_.right, rect_.bottom, dc_, 0, 0, SRCCOPY);
        SelectObject(dc_, previous_); DeleteObject(bitmap_); DeleteDC(dc_);
    }
    operator HDC() const { return dc_; }
};
} // namespace

FluentSettings::FluentSettings() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&graphicsToken_, &input, nullptr);
    background_ = CreateSolidBrush(kBackground); content_ = CreateSolidBrush(kContent);
}
FluentSettings::~FluentSettings() {
    for (auto font : {body_, small_, label_, title_}) if (font) DeleteObject(font);
    DeleteObject(background_); DeleteObject(content_);
    if (graphicsToken_) Gdiplus::GdiplusShutdown(graphicsToken_);
}
int FluentSettings::Px(int value) const { return MulDiv(value, dpi_, 96); }
RECT FluentSettings::Rect(int x, int y, int width, int height) const {
    return {Px(x), Px(y), Px(x + width), Px(y + height)};
}
void FluentSettings::Fonts() {
    for (auto font : {body_, small_, label_, title_}) if (font) DeleteObject(font);
    auto make = [this](int height, int weight) {
        return CreateFontW(-Px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    body_ = make(14, FW_NORMAL); small_ = make(12, FW_NORMAL);
    label_ = make(14, FW_SEMIBOLD); title_ = make(20, FW_SEMIBOLD);
    SendMessageW(dialog_, WM_SETFONT, reinterpret_cast<WPARAM>(body_), FALSE);
    for (HWND child = GetWindow(dialog_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(child);
        HFONT font = id == 1120 ? title_ :
            (id == 1121 || id == 1122 || id == 1123 || id == 1131 ? small_ :
            (id == 1110 || id == 1111 || id == 1112 || id == 1130 ? label_ : body_));
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
}
void FluentSettings::Layout(bool center) {
    // All measurements are DIPs, including common-control item heights at a new monitor DPI.
    RECT windowRect = Rect(0, 0, 480, 564);
    AdjustWindowRectExForDpi(&windowRect, static_cast<DWORD>(GetWindowLongPtrW(dialog_, GWL_STYLE)),
                            FALSE, static_cast<DWORD>(GetWindowLongPtrW(dialog_, GWL_EXSTYLE)), dpi_);
    const int width = windowRect.right - windowRect.left, height = windowRect.bottom - windowRect.top;
    RECT current{}; GetWindowRect(dialog_, &current);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(dialog_, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = current.left, y = current.top;
    if (center) {
        RECT owner{}; GetWindowRect(GetWindow(dialog_, GW_OWNER), &owner);
        x = (owner.left + owner.right - width) / 2; y = (owner.top + owner.bottom - height) / 2;
    }
    x = std::max<int>(monitor.rcWork.left, std::min<int>(x, monitor.rcWork.right - width));
    y = std::max<int>(monitor.rcWork.top, std::min<int>(y, monitor.rcWork.bottom - height));
    SetWindowPos(dialog_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    auto move = [this](int id, int x, int y, int width, int height) {
        const RECT rect = Rect(x, y, width, height);
        SetWindowPos(GetDlgItem(dialog_, id), nullptr, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    move(1120, 24, 20, 432, 28); move(1121, 24, 54, 432, 20);
    move(1110, 40, 102, 400, 22); move(1101, 40, 132, 400, 196);
    SendDlgItemMessageW(dialog_, 1101, CB_SETITEMHEIGHT, 0, Px(34));
    SendDlgItemMessageW(dialog_, 1101, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(34));
    SendDlgItemMessageW(dialog_, 1101, CB_SETMINVISIBLE, 4, 0);
    move(1111, 40, 190, 300, 22); move(1104, 384, 187, 56, 28);
    move(1102, 40, 223, 400, 32);
    move(1112, 40, 280, 300, 22); move(1105, 384, 277, 56, 28);
    move(1103, 40, 313, 400, 32);
    for (int id : {1102, 1103}) SendDlgItemMessageW(dialog_, id, TBM_SETTHUMBLENGTH, Px(22), 0);
    move(1122, 44, 347, 120, 18); move(1123, 324, 347, 112, 18);
    move(1132, 40, 410, 40, 40); move(1130, 96, 400, 150, 22);
    move(1131, 96, 426, 150, 20);
    move(1140, 264, 400, 88, 36); move(1141, 364, 400, 76, 36);
    move(1142, 302, 442, 138, 26);
    move(IDCANCEL, 268, 510, 88, 38); move(IDOK, 368, 510, 88, 38);
    InvalidateRect(dialog_, nullptr, TRUE);
}
void FluentSettings::Attach(int id, Kind kind) {
    auto* control = new Control{this, kind};
    if (!SetWindowSubclass(GetDlgItem(dialog_, id), ControlProc, 1, reinterpret_cast<DWORD_PTR>(control))) delete control;
}
void FluentSettings::Initialize(HWND dialog) {
    dialog_ = dialog; dpi_ = GetDpiForWindow(dialog);
    SetWindowLongPtrW(dialog, GWL_STYLE, GetWindowLongPtrW(dialog, GWL_STYLE) | WS_CLIPCHILDREN);
    const BOOL dark = FALSE; const DWORD corner = 2; const COLORREF caption = kBackground;
    DwmSetWindowAttribute(dialog, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    DwmSetWindowAttribute(dialog, static_cast<DWMWINDOWATTRIBUTE>(33), &corner, sizeof(corner));
    DwmSetWindowAttribute(dialog, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption));
    Fonts();
    for (int id : {IDOK, IDCANCEL, 1140, 1141, 1142}) Attach(id, Kind::Button);
    Attach(1101, Kind::Combo);
    Attach(1102, Kind::Slider); Attach(1103, Kind::Slider);
    Attach(1104, Kind::Value); Attach(1105, Kind::Value); Attach(1132, Kind::Icon);
    SetDlgItemTextW(dialog, 1102, L"透明度"); SetDlgItemTextW(dialog, 1103, L"模糊程度");
    SetDlgItemTextW(dialog, 1132, L"当前收纳盒图标");
    Layout(true);
}
void FluentSettings::SetIcon(HICON icon, const std::wstring& name) {
    icon_ = icon; SetDlgItemTextW(dialog_, 1131, name.c_str());
    InvalidateRect(GetDlgItem(dialog_, 1132), nullptr, FALSE);
}
void FluentSettings::SetGlassEnabled(bool enabled) {
    glassEnabled_ = enabled;
    for (int id : {1102, 1103}) EnableWindow(GetDlgItem(dialog_, id), enabled);
    for (int id : {1111, 1112, 1104, 1105, 1122, 1123}) InvalidateRect(GetDlgItem(dialog_, id), nullptr, FALSE);
}
void FluentSettings::PaintBackground(HDC dc) {
    RECT client{}; GetClientRect(dialog_, &client); Buffer buffer(dc, client);
    Fill(buffer, client, kBackground);
    Rounded(buffer, Rect(24, 88, 432, 282), static_cast<float>(Px(8)), kContent, kBorder);
    Rounded(buffer, Rect(24, 388, 432, 88), static_cast<float>(Px(8)), kContent, kBorder);
    Fill(buffer, Rect(24, 493, 432, 1), kBorder);
}
void FluentSettings::PaintSlider(HWND window, HDC dc) {
    RECT client{}, channel{}, thumb{}; GetClientRect(window, &client);
    SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    Buffer buffer(dc, client); Fill(buffer, client, kContent);
    const int cy = (thumb.top + thumb.bottom) / 2;
    const int cx = (thumb.left + thumb.right) / 2;
    RECT rail{channel.left, cy - Px(2), channel.right, cy + Px(2)};
    Rounded(buffer, rail, static_cast<float>(Px(2)), kBorder, kBorder, 0);
    rail.right = cx;
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const COLORREF accent = enabled ? kAccent : kDisabled;
    Rounded(buffer, rail, static_cast<float>(Px(2)), accent, accent, 0);
    auto* state = reinterpret_cast<Control*>(0); DWORD_PTR data = 0;
    if (GetWindowSubclass(window, ControlProc, 1, &data)) state = reinterpret_cast<Control*>(data);
    const bool pressed = GetCapture() == window, hover = state && state->hovered;
    const int radius = Px(10), dot = Px(pressed ? 5 : hover ? 7 : 6);
    RECT outer{cx - radius, cy - radius, cx + radius, cy + radius};
    const UINT uiState = static_cast<UINT>(SendMessageW(window, WM_QUERYUISTATE, 0, 0));
    if (GetFocus() == window && !(uiState & UISF_HIDEFOCUS)) {
        RECT focus = outer; InflateRect(&focus, Px(3), Px(3));
        Rounded(buffer, focus, static_cast<float>(Px(13)), RGB(226, 238, 255), kAccent);
    }
    Rounded(buffer, outer, static_cast<float>(radius), kContent, RGB(205, 210, 217));
    RECT inner{cx - dot, cy - dot, cx + dot, cy + dot};
    Rounded(buffer, inner, static_cast<float>(dot), accent, accent, 0);
}
void FluentSettings::PaintControl(HWND window, Control& control, HDC dc) {
    RECT rect{}; GetClientRect(window, &rect); Buffer buffer(dc, rect);
    const bool enabled = IsWindowEnabled(window) != FALSE;
    Fill(buffer, rect, control.kind == Kind::Button && (GetDlgCtrlID(window) == IDOK || GetDlgCtrlID(window) == IDCANCEL) ? kBackground : kContent);
    const COLORREF text = enabled ? kText : kDisabled;
    const bool focus = GetFocus() == window;
    const UINT uiState = static_cast<UINT>(SendMessageW(window, WM_QUERYUISTATE, 0, 0));
    if (control.kind == Kind::Icon) {
        if (icon_) DrawIconEx(buffer, 0, 0, icon_, rect.right, rect.bottom, 0, nullptr, DI_NORMAL);
    } else if (control.kind == Kind::Value) {
        Rounded(buffer, rect, static_cast<float>(Px(6)), kHover, kHover, 0);
        Text(buffer, body_, WindowText(window), rect, glassEnabled_ ? kText : kDisabled, DT_CENTER);
    } else if (control.kind == Kind::Combo) {
        const bool dropped = SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        const COLORREF border = focus || dropped ? kAccent : control.hovered ? RGB(169, 181, 197) : kBorder;
        Rounded(buffer, rect, static_cast<float>(Px(6)), control.hovered ? RGB(250, 252, 255) : kContent, border,
                 static_cast<float>(focus ? Px(2) : 1));
        RECT label = rect; label.left += Px(12); label.right -= Px(38);
        Text(buffer, body_, WindowText(window), label, text);
        Gdiplus::Graphics graphics(buffer); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Color(kMuted), static_cast<float>(Px(1)));
        const int x = rect.right - Px(20), y = rect.bottom / 2;
        const Gdiplus::Point points[] = {{x - Px(4), y - Px(2)}, {x, y + Px(2)}, {x + Px(4), y - Px(2)}};
        graphics.DrawLines(&pen, points, 3);
    } else if (control.kind == Kind::Button) {
        const int id = GetDlgCtrlID(window);
        const bool primary = id == IDOK, link = id == 1142;
        const UINT state = static_cast<UINT>(SendMessageW(window, BM_GETSTATE, 0, 0));
        const bool pressed = (state & BST_PUSHED) != 0;
        COLORREF fill = primary ? kAccent : kContent, border = primary ? kAccent : kBorder;
        if (primary && control.hovered) fill = RGB(31, 107, 215);
        if (primary && pressed) fill = RGB(25, 94, 193);
        if (!primary && (control.hovered || pressed)) fill = pressed ? RGB(229, 235, 243) : kHover;
        if (link) { fill = control.hovered || pressed ? kHover : kContent; border = fill; }
        if (!enabled) fill = RGB(236, 239, 243);
        Rounded(buffer, rect, static_cast<float>(Px(6)), fill, primary ? fill : border);
        const bool showFocus = focus && !(uiState & UISF_HIDEFOCUS);
        if (showFocus) {
            RECT inner = rect; InflateRect(&inner, -Px(3), -Px(3));
            Rounded(buffer, inner, static_cast<float>(Px(4)), fill, primary ? kContent : kAccent);
        }
        Text(buffer, body_, WindowText(window), rect, !enabled ? kDisabled : primary ? kContent : link ? kAccent : kText, DT_CENTER);
    }
}
LRESULT CALLBACK FluentSettings::ControlProc(HWND window, UINT message, WPARAM w, LPARAM l,
                                             UINT_PTR subclassId, DWORD_PTR data) {
    auto* state = reinterpret_cast<Control*>(data); auto* owner = state->owner;
    switch (message) {
    case WM_PAINT: {
        // Native input/accessibility are retained; painting also replaces the legacy focus rectangle.
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        if (state->kind == Kind::Slider) owner->PaintSlider(window, dc);
        else owner->PaintControl(window, *state, dc);
        EndPaint(window, &paint); return 0;
    }
    case WM_PRINTCLIENT:
        if (state->kind == Kind::Slider) owner->PaintSlider(window, reinterpret_cast<HDC>(w));
        else owner->PaintControl(window, *state, reinterpret_cast<HDC>(w));
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (!state->hovered) {
            state->hovered = true; TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track); InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE: state->hovered = false; InvalidateRect(window, nullptr, FALSE); break;
    case WM_NCDESTROY: {
        RemoveWindowSubclass(window, ControlProc, subclassId);
        const auto result = DefSubclassProc(window, message, w, l); delete state; return result;
    }
    }
    const auto result = DefSubclassProc(window, message, w, l);
    switch (message) {
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_UPDATEUISTATE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_CAPTURECHANGED: case WM_KEYDOWN: case WM_KEYUP:
    case BM_SETSTATE: case BM_SETSTYLE: case CB_SETCURSEL: case WM_SETTEXT:
        InvalidateRect(window, nullptr, FALSE); break;
    }
    return result;
}
bool FluentSettings::Message(UINT message, WPARAM w, LPARAM l, INT_PTR& result) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(dialog_, &paint);
        PaintBackground(dc); EndPaint(dialog_, &paint); result = TRUE; return true;
    }
    case WM_ERASEBKGND: result = TRUE; return true;
    case WM_CTLCOLORDLG: result = reinterpret_cast<INT_PTR>(background_); return true;
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
        const bool secondary = id == 1121 || id == 1122 || id == 1123 || id == 1131;
        const bool disabled = !glassEnabled_ && (id == 1111 || id == 1112 || id == 1122 || id == 1123);
        SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(w), disabled ? kDisabled : secondary ? kMuted : kText);
        result = reinterpret_cast<INT_PTR>(id == 1120 || id == 1121 ? background_ : content_); return true;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        if (measure->CtlID == 1101) { measure->itemHeight = Px(34); result = TRUE; return true; }
        break;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (draw->CtlID != 1101) break;
        Fill(draw->hDC, draw->rcItem, kContent);
        if (draw->itemState & ODS_SELECTED) {
            RECT selection = draw->rcItem; InflateRect(&selection, -Px(4), -Px(2));
            Rounded(draw->hDC, selection, static_cast<float>(Px(4)), RGB(233, 242, 255), RGB(233, 242, 255), 0);
        }
        wchar_t text[128]{};
        if (draw->itemID != static_cast<UINT>(-1)) SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, reinterpret_cast<LPARAM>(text));
        RECT label = draw->rcItem; label.left += Px(12); label.right -= Px(8);
        Text(draw->hDC, body_, text, label, kText);
        result = TRUE; return true;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w); const auto* rect = reinterpret_cast<RECT*>(l);
        SetWindowPos(dialog_, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Fonts(); Layout(false); result = TRUE; return true;
    }
    }
    return false;
}
} // namespace box
