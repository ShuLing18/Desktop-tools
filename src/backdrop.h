#pragma once
#include <windows.h>
#include <vector>

namespace box {
class Backdrop {
    int width_ = 0, height_ = 0, strength_ = -1;
    std::vector<DWORD> source_, blurred_;
public:
    bool Capture(RECT screen);
    void SetSource(int width, int height, std::vector<DWORD> pixels);
    void Blur(int strength);
    bool Draw(DWORD* target, int width, int height, int transparency, COLORREF tint) const;
    void Clear();
    bool Matches(int width, int height) const { return width_ == width && height_ == height && !source_.empty(); }
    const std::vector<DWORD>& Pixels() const { return blurred_; }
};
}
