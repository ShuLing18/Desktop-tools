#pragma once
#include <cstddef>
#include <vector>

namespace box {
class Selection {
public:
    using Mask = std::vector<unsigned char>;
private:
    Mask selected_;
    int focus_ = -1, anchor_ = -1, count_ = 0;
    bool Valid(int index) const { return index >= 0 && static_cast<size_t>(index) < selected_.size(); }
public:
    void Reset(size_t size);
    void Clear();
    void Set(int index, bool selected);
    void Click(int index, bool control = false, bool shift = false);
    void Focus(int index) { if (Valid(index)) focus_ = index; }
    void All();
    void Restore(const Mask& mask, int focus = -1, int anchor = -1);
    void Box(const std::vector<int>& hits, const Mask& baseline, bool control, bool shift);
    bool Contains(int index) const { return Valid(index) && selected_[index] != 0; }
    int Count() const { return count_; }
    int Focus() const { return focus_; }
    int Anchor() const { return anchor_; }
    const Mask& Values() const { return selected_; }
    std::vector<int> Indices() const;
};
// The returned permutation moves the selected block together, preserving its order.
// A target equal to size means the empty space after the final item.
std::vector<int> ReorderSelection(size_t size, const std::vector<int>& selected, int target);
}
