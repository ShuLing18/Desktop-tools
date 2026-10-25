#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace box {
class DibSurface {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    DWORD* pixels_ = nullptr;
    int width_ = 0, height_ = 0;
public:
    DibSurface() = default;
    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;
    ~DibSurface() { Clear(); }
    bool Ensure(HDC reference, int width, int height) {
        if (dc_ && width_ == width && height_ == height) return true;
        Clear();
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
        dc_ = CreateCompatibleDC(reference); if (!dc_) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (!bitmap_) { Clear(); return false; }
        previous_ = SelectObject(dc_, bitmap_); width_ = width; height_ = height;
        return true;
    }
    bool EnsureAtLeast(HDC reference, int width, int height) {
        return (dc_ && width <= width_ && height <= height_) || Ensure(reference, std::max(width, width_), std::max(height, height_));
    }
    void Clear() {
        if (dc_ && previous_) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr; bitmap_ = nullptr; previous_ = nullptr; pixels_ = nullptr; width_ = height_ = 0;
    }
    HDC DC() const { return dc_; }
    DWORD* Pixels() const { return pixels_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
};

class TextMasks {
public:
    struct Glyph {
        std::wstring text;
        HFONT font = nullptr;
        UINT flags = 0;
        int width = 0, height = 0;
        RECT ink{};
        ULONGLONG used = 0;
        std::vector<BYTE> coverage;
        unsigned At(int x, int y) const {
            return x < ink.left || x >= ink.right || y < ink.top || y >= ink.bottom ? 0 :
                coverage[static_cast<size_t>(y - ink.top) * (ink.right - ink.left) + x - ink.left];
        }
        size_t Bytes() const { return coverage.size() + text.size() * sizeof(wchar_t); }
    };
private:
    DibSurface scratch_;
    std::vector<Glyph> glyphs_;
    Glyph transient_;
    size_t bytes_ = 0;
    ULONGLONG clock_ = 0;
    static constexpr size_t kLimit = 192 * 1024, kCount = 48;
public:
    const Glyph* Get(HDC reference, const std::wstring& text, HFONT font, int width, int height, UINT flags) {
        for (auto& glyph : glyphs_) if (glyph.font == font && glyph.width == width && glyph.height == height &&
            glyph.flags == flags && glyph.text == text) { glyph.used = ++clock_; return &glyph; }
        if (!scratch_.EnsureAtLeast(reference, width, height)) return nullptr;
        HDC dc = scratch_.DC(); DWORD* pixels = scratch_.Pixels(); const int stride = scratch_.Width();
        std::fill_n(pixels, static_cast<size_t>(stride) * scratch_.Height(), 0u);
        const auto oldFont = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT layout{0, 0, width, height}; DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &layout, flags | DT_NOPREFIX);
        GdiFlush(); SelectObject(dc, oldFont);
        Glyph glyph; glyph.text = text; glyph.font = font; glyph.flags = flags; glyph.width = width; glyph.height = height;
        glyph.ink = {width, height, 0, 0}; glyph.used = ++clock_;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) if (pixels[y * stride + x] & 255) {
            glyph.ink.left = std::min<LONG>(glyph.ink.left, x); glyph.ink.top = std::min<LONG>(glyph.ink.top, y);
            glyph.ink.right = std::max<LONG>(glyph.ink.right, x + 1); glyph.ink.bottom = std::max<LONG>(glyph.ink.bottom, y + 1);
        }
        if (IsRectEmpty(&glyph.ink)) glyph.ink = {};
        else for (LONG y = glyph.ink.top; y < glyph.ink.bottom; ++y) for (LONG x = glyph.ink.left; x < glyph.ink.right; ++x)
            glyph.coverage.push_back(static_cast<BYTE>(pixels[y * stride + x] & 255));
        const size_t needed = glyph.Bytes();
        // Exceptionally large accessibility fonts are still drawn, but their
        // masks must not defeat the retained-cache budget.
        if (needed > kLimit) { transient_ = std::move(glyph); return &transient_; }
        while (!glyphs_.empty() && (bytes_ + needed > kLimit || glyphs_.size() >= kCount)) {
            const auto oldest = std::min_element(glyphs_.begin(), glyphs_.end(), [](const Glyph& a, const Glyph& b) { return a.used < b.used; });
            bytes_ -= oldest->Bytes(); glyphs_.erase(oldest);
        }
        bytes_ += needed; glyphs_.push_back(std::move(glyph)); return &glyphs_.back();
    }
    void ReleaseTransient() { if (!transient_.coverage.empty()) transient_ = Glyph{}; }
    void Clear() { std::vector<Glyph>().swap(glyphs_); transient_ = Glyph{}; scratch_.Clear(); bytes_ = 0; clock_ = 0; }
    size_t Bytes() const { return bytes_; }
    size_t Count() const { return glyphs_.size(); }
};
}
