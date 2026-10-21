#include "backdrop.h"
#include <algorithm>

namespace {
void BoxPass(std::vector<DWORD>& pixels, std::vector<DWORD>& lineBuffer, int width, int height, int radius, bool horizontal) {
    const int lines = horizontal ? height : width, length = horizontal ? width : height;
    const unsigned divisor = 2 * radius + 1;
    auto index = [&](int line, int pixel) { return horizontal ? line * width + pixel : pixel * width + line; };
    for (int line = 0; line < lines; ++line) {
        for (int pixel = 0; pixel < length; ++pixel) lineBuffer[pixel] = pixels[index(line, pixel)];
        unsigned red = 0, green = 0, blue = 0;
        auto add = [&](int pixel, bool remove) {
            const DWORD color = lineBuffer[std::clamp(pixel, 0, length - 1)];
            if (remove) { red -= (color >> 16) & 255; green -= (color >> 8) & 255; blue -= color & 255; }
            else { red += (color >> 16) & 255; green += (color >> 8) & 255; blue += color & 255; }
        };
        for (int pixel = -radius; pixel <= radius; ++pixel) add(pixel, false);
        for (int pixel = 0; pixel < length; ++pixel) {
            pixels[index(line, pixel)] = 0xFF000000 | ((red / divisor) << 16) | ((green / divisor) << 8) | (blue / divisor);
            add(pixel - radius, true); add(pixel + radius + 1, false);
        }
    }
}
}
namespace box {
void Backdrop::SetSource(int width, int height, std::vector<DWORD> pixels) {
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<size_t>(width) * height) { Clear(); return; }
    width_ = width; height_ = height; source_ = std::move(pixels); strength_ = -1; blurred_.clear();
}
bool Backdrop::Capture(RECT screen) {
    const int width = screen.right - screen.left, height = screen.bottom - screen.top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    HDC display = GetDC(nullptr), dc = CreateCompatibleDC(display);
    if (!display || !dc) { if (dc) DeleteDC(dc); if (display) ReleaseDC(nullptr, display); return false; }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; DWORD* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    bool captured = false;
    if (bitmap) {
        auto old = SelectObject(dc, bitmap);
        captured = BitBlt(dc, 0, 0, width, height, display, screen.left, screen.top, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush();
        if (captured) SetSource(width, height, std::vector<DWORD>(bits, bits + static_cast<size_t>(width) * height));
        SelectObject(dc, old); DeleteObject(bitmap);
    }
    DeleteDC(dc); ReleaseDC(nullptr, display); return captured;
}
void Backdrop::Blur(int strength) {
    strength = std::clamp(strength, 0, 120);
    if (source_.empty() || strength_ == strength) return;
    strength_ = strength; blurred_ = source_;
    if (!strength) { for (auto& pixel : blurred_) pixel |= 0xFF000000; return; }
    std::vector<DWORD> scratch(static_cast<size_t>(std::max(width_, height_)));
    // Three running-sum box passes approximate a Gaussian blur in O(number of pixels).
    const int radius = std::max(1, strength / 3);
    for (int pass = 0; pass < 3; ++pass) {
        BoxPass(blurred_, scratch, width_, height_, radius, true);
        BoxPass(blurred_, scratch, width_, height_, radius, false);
    }
}
bool Backdrop::Draw(DWORD* target, int width, int height, int transparency, COLORREF tint) const {
    if (width != width_ || height != height_ || blurred_.empty()) return false;
    const unsigned background = std::clamp(transparency, 0, 100), overlay = 100 - background;
    for (size_t index = 0; index < blurred_.size(); ++index) {
        const DWORD pixel = blurred_[index];
        const unsigned red = (((pixel >> 16) & 255) * background + GetRValue(tint) * overlay + 50) / 100;
        const unsigned green = (((pixel >> 8) & 255) * background + GetGValue(tint) * overlay + 50) / 100;
        const unsigned blue = ((pixel & 255) * background + GetBValue(tint) * overlay + 50) / 100;
        target[index] = 0xFF000000 | (red << 16) | (green << 8) | blue;
    }
    return true;
}
void Backdrop::Clear() { width_ = height_ = 0; strength_ = -1; std::vector<DWORD>().swap(source_); std::vector<DWORD>().swap(blurred_); }
}
