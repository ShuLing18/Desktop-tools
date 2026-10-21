#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>

namespace box {

// Presentation only: the dialog keeps its native controls, notifications and settings model.
class FluentSettings {
public:
    FluentSettings();
    ~FluentSettings();
    FluentSettings(const FluentSettings&) = delete;
    FluentSettings& operator=(const FluentSettings&) = delete;
    void Initialize(HWND dialog);
    bool Message(UINT message, WPARAM w, LPARAM l, INT_PTR& result);
    void SetIcon(HICON icon, const std::wstring& name);
    void SetGlassEnabled(bool enabled);

private:
    enum class Kind { Button, Combo, Slider, Value, Icon };
    struct Control {
        FluentSettings* owner;
        Kind kind;
        bool hovered = false;
    };
    HWND dialog_ = nullptr;
    UINT dpi_ = 96;
    ULONG_PTR graphicsToken_ = 0;
    HFONT body_ = nullptr, small_ = nullptr, label_ = nullptr, title_ = nullptr;
    HBRUSH background_ = nullptr, content_ = nullptr;
    HICON icon_ = nullptr; // Borrowed from the application; updated before repainting.
    bool glassEnabled_ = true;
    int Px(int value) const;
    RECT Rect(int x, int y, int width, int height) const;
    void Fonts();
    void Layout(bool center);
    void Attach(int id, Kind kind);
    void PaintBackground(HDC dc);
    void PaintControl(HWND window, Control& control, HDC dc);
    void PaintSlider(HWND window, HDC dc);
    static LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM w, LPARAM l,
                                        UINT_PTR subclassId, DWORD_PTR data);
};
} // namespace box
