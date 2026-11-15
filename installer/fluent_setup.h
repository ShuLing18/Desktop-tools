#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>

namespace setupui {
enum : int { kTitle = 301, kSubtitle, kProgramLabel, kProgramHint, kDataLabel, kDataHint, kVersion };
constexpr COLORREF kBackground = RGB(243, 243, 243), kCard = RGB(255, 255, 255);
constexpr COLORREF kText = RGB(26, 26, 26), kMuted = RGB(100, 100, 100);
constexpr COLORREF kBorder = RGB(224, 224, 224), kAccent = RGB(0, 103, 192), kFooter = RGB(249, 249, 249);

class FluentSetup {
    enum class Kind { Button, Check, Edit };
    struct State { FluentSetup* owner; Kind kind; bool hovered = false; };
    struct Buffer {
        HDC target, dc; HBITMAP bitmap; HGDIOBJ previous; RECT rect;
        Buffer(HDC destination, RECT bounds) : target(destination), rect(bounds) {
            dc = CreateCompatibleDC(target);
            bitmap = CreateCompatibleBitmap(target, std::max(1L, rect.right), std::max(1L, rect.bottom));
            previous = SelectObject(dc, bitmap);
        }
        ~Buffer() {
            BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
        }
    };
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    ULONG_PTR graphics_ = 0;
    HFONT body_ = nullptr, small_ = nullptr, label_ = nullptr, title_ = nullptr;
    HBRUSH background_ = nullptr, card_ = nullptr;
    int program_ = 0, data_ = 0, primary_ = 0, cancel_ = 0;
    HICON icon_ = nullptr;

    static Gdiplus::Color Color(COLORREF color) {
        return {255, GetRValue(color), GetGValue(color), GetBValue(color)};
    }
    static void Fill(HDC dc, RECT rect, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
    }
    static void Rounded(HDC dc, RECT rect, float radius, COLORREF fill, COLORREF border, float width = 1) {
        using namespace Gdiplus;
        Graphics graphics(dc); graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        const float inset = width / 2, x = rect.left + inset, y = rect.top + inset;
        const float w = rect.right - rect.left - width, h = rect.bottom - rect.top - width;
        const float diameter = std::min(radius * 2, std::min(w, h));
        if (diameter <= 0) return;
        GraphicsPath path;
        path.AddArc(x, y, diameter, diameter, 180, 90);
        path.AddArc(x + w - diameter, y, diameter, diameter, 270, 90);
        path.AddArc(x + w - diameter, y + h - diameter, diameter, diameter, 0, 90);
        path.AddArc(x, y + h - diameter, diameter, diameter, 90, 90); path.CloseFigure();
        SolidBrush brush(Color(fill)); graphics.FillPath(&brush, &path);
        if (width > 0) { Pen pen(Color(border), width); graphics.DrawPath(&pen, &path); }
    }
    static std::wstring Text(HWND control) {
        const int count = GetWindowTextLengthW(control);
        std::wstring text(count + 1, L'\0'); GetWindowTextW(control, text.data(), count + 1); text.resize(count); return text;
    }
    void Label(HDC dc, const std::wstring& text, RECT rect, COLORREF color, UINT flags = DT_CENTER) {
        const auto previous = SelectObject(dc, body_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    void EditFrame(HDC dc, int id, int y) {
        const bool focus = GetFocus() == GetDlgItem(window_, id);
        const RECT rect = Rect(44, y, 432, 38);
        Rounded(dc, rect, static_cast<float>(Px(5)), kCard, focus ? kAccent : RGB(204, 204, 204));
        RECT underline{rect.left + Px(5), rect.bottom - Px(focus ? 2 : 1), rect.right - Px(5), rect.bottom};
        Fill(dc, underline, focus ? kAccent : RGB(138, 138, 138));
    }
    void PaintControl(HWND control, State& state, HDC dc) {
        RECT rect{}; GetClientRect(control, &rect); Buffer buffer(dc, rect);
        const int id = GetDlgCtrlID(control);
        const bool enabled = IsWindowEnabled(control) != FALSE;
        const bool primary = id == primary_;
        const bool footer = primary || id == cancel_ || state.kind == Kind::Check;
        Fill(buffer.dc, rect, footer ? kFooter : kCard);
        const UINT button = static_cast<UINT>(SendMessageW(control, BM_GETSTATE, 0, 0));
        const bool pressed = (button & BST_PUSHED) != 0;
        const UINT ui = static_cast<UINT>(SendMessageW(control, WM_QUERYUISTATE, 0, 0));
        const bool focus = GetFocus() == control && !(ui & UISF_HIDEFOCUS);
        if (state.kind == Kind::Check) {
            const bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const int top = (rect.bottom - Px(18)) / 2;
            const RECT mark{0, top, Px(18), top + Px(18)};
            const auto fill = checked ? state.hovered ? RGB(25, 117, 197) : kAccent : state.hovered ? RGB(239, 239, 239) : kCard;
            Rounded(buffer.dc, mark, static_cast<float>(Px(4)), fill, checked ? fill : RGB(126, 126, 126));
            if (checked) {
                Gdiplus::Graphics graphics(buffer.dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen pen(Color(kCard), static_cast<float>(Px(2)));
                const Gdiplus::Point points[] = {{Px(4), top + Px(9)}, {Px(8), top + Px(13)}, {Px(14), top + Px(5)}};
                graphics.DrawLines(&pen, points, 3);
            }
            RECT label = rect; label.left = Px(28);
            Label(buffer.dc, Text(control), label, enabled ? kText : kMuted, DT_LEFT);
            if (focus) { InflateRect(&rect, -1, -1); DrawFocusRect(buffer.dc, &rect); }
        } else {
            COLORREF fill = primary ? kAccent : kCard, border = primary ? kAccent : RGB(212, 212, 212);
            if (state.hovered) fill = primary ? RGB(25, 117, 197) : RGB(246, 246, 246);
            if (pressed) fill = primary ? RGB(0, 84, 160) : RGB(238, 238, 238);
            if (!enabled) { fill = RGB(232, 232, 232); border = fill; }
            Rounded(buffer.dc, rect, static_cast<float>(Px(5)), fill, border);
            Label(buffer.dc, Text(control), rect, !enabled ? RGB(148, 148, 148) : primary ? kCard : kText);
            if (focus) {
                RECT ring = rect; InflateRect(&ring, -Px(3), -Px(3));
                HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
                HPEN pen = CreatePen(PS_SOLID, Px(1), primary ? kCard : kText);
                const auto oldBrush = SelectObject(buffer.dc, hollow), oldPen = SelectObject(buffer.dc, pen);
                RoundRect(buffer.dc, ring.left, ring.top, ring.right, ring.bottom, Px(5), Px(5));
                SelectObject(buffer.dc, oldPen); SelectObject(buffer.dc, oldBrush); DeleteObject(pen);
            }
        }
    }
    static LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto* state = reinterpret_cast<State*>(data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, ControlProc, id); delete state; return DefSubclassProc(control, message, w, l);
        }
        if (state->kind != Kind::Edit && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(w);
            state->owner->PaintControl(control, *state, dc);
            if (message == WM_PAINT) EndPaint(control, &paint);
            return 0;
        }
        if (state->kind != Kind::Edit && message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEMOVE && !state->hovered) {
            state->hovered = true; TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0}; TrackMouseEvent(&track);
            InvalidateRect(control, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE) { state->hovered = false; InvalidateRect(control, nullptr, FALSE); }
        const LRESULT result = DefSubclassProc(control, message, w, l);
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == BM_SETSTATE ||
            message == BM_SETCHECK || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_UPDATEUISTATE) {
            InvalidateRect(control, nullptr, FALSE);
            if (state->kind == Kind::Edit) InvalidateRect(state->owner->window_, nullptr, FALSE);
        }
        return result;
    }
    void Attach(int id, Kind kind) {
        auto* state = new State{this, kind};
        if (!SetWindowSubclass(GetDlgItem(window_, id), ControlProc, 1, reinterpret_cast<DWORD_PTR>(state))) delete state;
    }
public:
    FluentSetup() {
        Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&graphics_, &input, nullptr);
        background_ = CreateSolidBrush(kBackground); card_ = CreateSolidBrush(kCard);
    }
    ~FluentSetup() {
        for (HFONT font : {body_, small_, label_, title_}) if (font) DeleteObject(font);
        DeleteObject(background_); DeleteObject(card_);
        if (icon_) DestroyIcon(icon_);
        if (graphics_) Gdiplus::GdiplusShutdown(graphics_);
    }
    int Px(int value) const { return MulDiv(value, dpi_, 96); }
    RECT Rect(int x, int y, int width, int height) const { return {Px(x), Px(y), Px(x + width), Px(y + height)}; }
    HFONT BodyFont() const { return body_; }
    void SetDpi(UINT dpi) {
        dpi_ = dpi;
        for (HFONT font : {body_, small_, label_, title_}) if (font) DeleteObject(font);
        auto make = [this](int size, int weight) {
            return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        };
        body_ = make(14, FW_NORMAL); small_ = make(12, FW_NORMAL);
        label_ = make(14, FW_SEMIBOLD); title_ = make(24, FW_SEMIBOLD);
        if (icon_) DestroyIcon(icon_);
        icon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, Px(44), Px(44), 0));
        if (!window_) return;
        for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            const int id = GetDlgCtrlID(child);
            const HFONT font = id == kTitle ? title_ : id == kProgramLabel || id == kDataLabel ? label_ :
                id == kSubtitle || id == kProgramHint || id == kDataHint || id == kVersion ? small_ : body_;
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
    void Initialize(HWND window, int program, int data, int browse, int browseData, int primary, int check, int cancel) {
        window_ = window; program_ = program; data_ = data; primary_ = primary; cancel_ = cancel;
        SetDpi(GetDpiForWindow(window));
        const BOOL dark = FALSE; const DWORD corner = 2; const COLORREF caption = kBackground;
        DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
        DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(33), &corner, sizeof(corner));
        DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption));
        for (int id : {browse, browseData, primary, cancel}) Attach(id, Kind::Button);
        Attach(check, Kind::Check); Attach(program, Kind::Edit); Attach(data, Kind::Edit);
    }
    bool Message(UINT message, WPARAM w, LPARAM l, LRESULT& result) {
        if (message == WM_ERASEBKGND) { result = 1; return true; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window_, &paint);
            RECT client{}; GetClientRect(window_, &client);
            {
                Buffer buffer(dc, client); Fill(buffer.dc, client, kBackground);
                Rounded(buffer.dc, Rect(28, 24, 56, 56), static_cast<float>(Px(12)), RGB(231, 240, 251), RGB(231, 240, 251), 0);
                if (icon_) DrawIconEx(buffer.dc, Px(34), Px(30), icon_, Px(44), Px(44), 0, nullptr, DI_NORMAL);
                Rounded(buffer.dc, Rect(28, 112, 584, 138), static_cast<float>(Px(8)), kCard, kBorder);
                Rounded(buffer.dc, Rect(28, 264, 584, 142), static_cast<float>(Px(8)), kCard, kBorder);
                EditFrame(buffer.dc, program_, 158); EditFrame(buffer.dc, data_, 310);
                Fill(buffer.dc, Rect(0, 428, 640, 82), kFooter);
                Fill(buffer.dc, Rect(0, 428, 640, 1), kBorder);
            }
            EndPaint(window_, &paint); result = 0; return true;
        }
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            const bool card = message == WM_CTLCOLOREDIT || id == kProgramLabel || id == kProgramHint || id == kDataLabel || id == kDataHint;
            const bool muted = id == kSubtitle || id == kProgramHint || id == kDataHint || id == kVersion;
            HDC dc = reinterpret_cast<HDC>(w); SetTextColor(dc, muted ? kMuted : kText);
            SetBkMode(dc, TRANSPARENT); SetBkColor(dc, card ? kCard : kBackground);
            result = reinterpret_cast<LRESULT>(card ? card_ : background_); return true;
        }
        return false;
    }
};
}
