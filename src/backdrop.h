#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <vector>

namespace box {
class Backdrop {
    int width_ = 0, height_ = 0, strength_ = -1;
    RECT screen_{};
    std::shared_ptr<const std::vector<DWORD>> source_;
    std::vector<DWORD> blurred_;
public:
    bool Capture(RECT screen);
    void SetSource(int width, int height, std::vector<DWORD> pixels);
    bool Blur(int strength, const std::atomic<bool>* cancelled = nullptr);
    bool Draw(DWORD* target, int width, int height, int transparency, COLORREF tint) const;
    Backdrop SourceSnapshot() const;
    void Clear();
    bool Matches(int width, int height) const { return width_ == width && height_ == height && source_ && !source_->empty(); }
    bool Matches(RECT screen) const { return Matches(screen.right - screen.left, screen.bottom - screen.top) && EqualRect(&screen_, &screen); }
    bool Ready(int strength) const { return !blurred_.empty() && strength_ == strength; }
    bool HasPixels() const { return !blurred_.empty(); }
    size_t MemoryBytes() const { return ((source_ ? source_->capacity() : 0) + blurred_.capacity()) * sizeof(DWORD); }
    const std::vector<DWORD>& Pixels() const { return blurred_; }
};
}
