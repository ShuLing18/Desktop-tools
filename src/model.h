#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace box {
enum class Direction { Automatic = 0, Left = 1, Right = 2, Up = 3, Down = 4 };
enum class SortMode { Manual = 0, Modified = 1, Name = 2 };
struct Placement { RECT rect; bool opensLeft; Direction direction; };
Placement PlacePanel(RECT anchor, RECT work, int width, int height, int gap, Direction direction);
POINT ClampAnchor(POINT point, SIZE size, RECT work);
POINT SnapAnchor(POINT point, SIZE cell, RECT work, const std::vector<RECT>& occupied = {});
std::wstring SafeName(std::wstring name);
std::wstring FullPath(const std::wstring& path);
std::wstring BaseName(const std::wstring& path);
std::wstring ParentPath(const std::wstring& path);
bool IsLink(const std::wstring& path);
bool InPanelTransit(POINT point, RECT icon, RECT panel);
bool MatchesSearch(const std::wstring& name, const std::wstring& query);
double LayoutEase(ULONGLONG elapsed, ULONGLONG duration = 220);
RECT InterpolateRect(RECT from, RECT to, double progress);
std::wstring UniquePath(const std::wstring& directory, const std::wstring& stem, const std::wstring& extension);
}

