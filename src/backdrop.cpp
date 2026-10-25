#include "backdrop.h"
#include <algorithm>

namespace {
template<bool horizontal>
bool BoxPass(std::vector<DWORD>& pixels, std::vector<DWORD>& lineBuffer, const std::vector<BYTE>& average,
             int width, int height, int radius, const std::atomic<bool>* cancelled) {
    const int lines = horizontal ? height : width, length = horizontal ? width : height;
    for (int line = 0; line < lines; ++line) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return false;
        const size_t start = horizontal ? static_cast<size_t>(line) * width : line;
        const size_t stride = horizontal ? 1 : width;
        if constexpr (horizontal) std::copy_n(pixels.data() + start, length, lineBuffer.data());
        else for (int pixel = 0; pixel < length; ++pixel) lineBuffer[pixel] = pixels[start + static_cast<size_t>(pixel) * stride];
        unsigned red = 0, green = 0, blue = 0;
        auto add = [&](int pixel) {
            const DWORD color = lineBuffer[std::clamp(pixel, 0, length - 1)];
            red += (color >> 16) & 255; green += (color >> 8) & 255; blue += color & 255;
        };
        for (int pixel = -radius; pixel <= radius; ++pixel) add(pixel);
        size_t output = start;
        for (int pixel = 0; pixel < length; ++pixel) {
            pixels[output] = 0xFF000000 | (static_cast<DWORD>(average[red]) << 16) |
                                          (static_cast<DWORD>(average[green]) << 8) | average[blue];
            output += stride;
            const DWORD before = lineBuffer[std::max(0, pixel - radius)];
            const DWORD after = lineBuffer[std::min(length - 1, pixel + radius + 1)];
            red += ((after >> 16) & 255) - ((before >> 16) & 255);
            green += ((after >> 8) & 255) - ((before >> 8) & 255);
            blue += (after & 255) - (before & 255);
        }
    }
    return true;
}
}
namespace box {
void Backdrop::SetSource(int width, int height, std::vector<DWORD> pixels) {
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<size_t>(width) * height) { Clear(); return; }
    width_ = width; height_ = height; screen_ = {0, 0, width, height};
    source_ = std::make_shared<const std::vector<DWORD>>(std::move(pixels)); strength_ = -1; blurred_.clear();
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
        if (captured) {
            SetSource(width, height, std::vector<DWORD>(bits, bits + static_cast<size_t>(width) * height));
            screen_ = screen;
        }
        SelectObject(dc, old); DeleteObject(bitmap);
    }
    DeleteDC(dc); ReleaseDC(nullptr, display); return captured;
}
bool Backdrop::Blur(int strength, const std::atomic<bool>* cancelled) {
    strength = std::clamp(strength, 0, 120);
    if (!source_ || source_->empty() || (cancelled && cancelled->load(std::memory_order_relaxed))) return false;
    if (strength_ == strength) return true;
    strength_ = -1; blurred_ = *source_;
    if (!strength) { for (auto& pixel : blurred_) pixel |= 0xFF000000; strength_ = 0; return true; }
    std::vector<DWORD> scratch(static_cast<size_t>(std::max(width_, height_)));
    // Three running-sum box passes approximate a Gaussian blur in O(number of pixels).
    const int radius = std::max(1, strength / 3);
    // Every channel sum is bounded. One tiny lookup table replaces millions of
    // integer divisions while preserving the old blur's exact rounding.
    const unsigned divisor = 2 * radius + 1;
    std::vector<BYTE> average(divisor * 255 + 1);
    for (size_t i = 0; i < average.size(); ++i) average[i] = static_cast<BYTE>(i / divisor);
    for (int pass = 0; pass < 3; ++pass) {
        if (!BoxPass<true>(blurred_, scratch, average, width_, height_, radius, cancelled) ||
            !BoxPass<false>(blurred_, scratch, average, width_, height_, radius, cancelled)) {
            std::vector<DWORD>().swap(blurred_); return false;
        }
    }
    strength_ = strength; return true;
}
bool Backdrop::Draw(DWORD* target, int width, int height, int transparency, COLORREF tint) const {
    if (width != width_ || height != height_ || blurred_.empty()) return false;
    const unsigned background = std::clamp(transparency, 0, 100), overlay = 100 - background;
    auto compose = [&](DWORD pixel) {
        const unsigned red = (((pixel >> 16) & 255) * background + GetRValue(tint) * overlay + 50) / 100;
        const unsigned green = (((pixel >> 8) & 255) * background + GetGValue(tint) * overlay + 50) / 100;
        const unsigned blue = ((pixel & 255) * background + GetBValue(tint) * overlay + 50) / 100;
        return 0xFF000000 | (red << 16) | (green << 8) | blue;
    };
    for (size_t index = 0; index < blurred_.size(); ++index) target[index] = compose(blurred_[index]);
    return true;
}
Backdrop Backdrop::SourceSnapshot() const {
    Backdrop snapshot; snapshot.width_ = width_; snapshot.height_ = height_;
    snapshot.screen_ = screen_; snapshot.source_ = source_; return snapshot;
}
void Backdrop::Clear() { width_ = height_ = 0; strength_ = -1; screen_ = {}; source_.reset(); std::vector<DWORD>().swap(blurred_); }
}
