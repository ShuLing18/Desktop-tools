#include "selection.h"
#include <algorithm>
#include <numeric>

namespace box {
void Selection::Reset(size_t size) { selected_.assign(size, 0); focus_ = anchor_ = -1; count_ = 0; }
void Selection::Clear() { std::fill(selected_.begin(), selected_.end(), 0); focus_ = anchor_ = -1; count_ = 0; }
void Selection::Set(int index, bool selected) {
    if (!Valid(index) || Contains(index) == selected) return;
    selected_[index] = selected ? 1 : 0; count_ += selected ? 1 : -1;
}
void Selection::Click(int index, bool control, bool shift) {
    if (!Valid(index)) return;
    if (shift) {
        const int start = Valid(anchor_) ? anchor_ : Valid(focus_) ? focus_ : index;
        if (!control) { std::fill(selected_.begin(), selected_.end(), 0); count_ = 0; }
        for (int i = std::min(start, index); i <= std::max(start, index); ++i) Set(i, true);
        anchor_ = start;
    } else if (control) {
        Set(index, !Contains(index)); anchor_ = index;
    } else {
        Clear(); Set(index, true); anchor_ = index;
    }
    focus_ = index;
}
void Selection::All() {
    std::fill(selected_.begin(), selected_.end(), 1); count_ = static_cast<int>(selected_.size());
    if (count_ && !Valid(focus_)) focus_ = 0;
    anchor_ = focus_;
}
void Selection::Restore(const Mask& mask, int focus, int anchor) {
    count_ = 0;
    for (size_t i = 0; i < selected_.size(); ++i) {
        selected_[i] = i < mask.size() && mask[i] ? 1 : 0; count_ += selected_[i];
    }
    focus_ = Valid(focus) ? focus : -1;
    if (focus_ < 0) for (size_t i = 0; i < selected_.size(); ++i) if (selected_[i]) { focus_ = static_cast<int>(i); break; }
    anchor_ = Valid(anchor) ? anchor : focus_;
}
void Selection::Box(const std::vector<int>& hits, const Mask& baseline, bool control, bool shift) {
    if (control || shift) Restore(baseline, focus_, anchor_); else Clear();
    for (int index : hits) Set(index, control && !shift ? !(Valid(index) && static_cast<size_t>(index) < baseline.size() && baseline[index]) : true);
    if (!Contains(focus_)) {
        focus_ = -1;
        for (size_t i = 0; i < selected_.size(); ++i) if (selected_[i]) { focus_ = static_cast<int>(i); break; }
    }
    anchor_ = focus_;
}
std::vector<int> Selection::Indices() const {
    std::vector<int> result; result.reserve(static_cast<size_t>(count_));
    for (size_t i = 0; i < selected_.size(); ++i) if (selected_[i]) result.push_back(static_cast<int>(i));
    return result;
}
std::vector<int> ReorderSelection(size_t size, const std::vector<int>& selected, int target) {
    std::vector<int> unchanged(size); std::iota(unchanged.begin(), unchanged.end(), 0);
    if (target < 0 || target > static_cast<int>(size)) return unchanged;
    std::vector<bool> included(size, false);
    for (int index : selected) if (index >= 0 && static_cast<size_t>(index) < size) included[index] = true;
    if (target < static_cast<int>(size) && included[target]) return unchanged;
    std::vector<int> block, rest;
    for (size_t i = 0; i < size; ++i) (included[i] ? block : rest).push_back(static_cast<int>(i));
    if (block.empty()) return unchanged;
    size_t insertion = rest.size();
    if (target < static_cast<int>(size)) {
        const auto found = std::find(rest.begin(), rest.end(), target);
        insertion = static_cast<size_t>(found - rest.begin()) + (target > block.front() ? 1 : 0);
    }
    rest.insert(rest.begin() + insertion, block.begin(), block.end());
    return rest;
}
}
