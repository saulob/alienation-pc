// Windowed-mode sizing: pure arithmetic, no SDL.
#pragma once

#include <algorithm>
#include <cmath>

namespace aln {

struct WindowBorders {
    int top = 0, left = 0, bottom = 0, right = 0;
};

struct WindowFit {
    int width = 0, height = 0; // client size
    int x = 0, y = 0;          // client origin that centres the decorated window in the area
};

// Title bar and frame at 100% scaling, used when the window system cannot report the real ones.
inline WindowBorders EstimatedBorders(float scale) {
    const float s = scale > 0.0f ? scale : 1.0f;
    const int edge = static_cast<int>(std::lround(1.0f * s));
    return {static_cast<int>(std::lround(31.0f * s)), edge, edge, edge};
}

// A decorated window that already fits the area is left alone; otherwise the client size shrinks by one common
// factor until it does. Never enlarges.
inline WindowFit FitWindowToArea(int width, int height, WindowBorders borders, int area_x, int area_y, int area_w,
                                 int area_h) {
    const int extra_w = borders.left + borders.right;
    const int extra_h = borders.top + borders.bottom;
    double scale = 1.0;
    if (width > 0 && height > 0 && area_w > 0 && area_h > 0) {
        const double room_w = std::max(1, area_w - extra_w);
        const double room_h = std::max(1, area_h - extra_h);
        scale = std::min({1.0, room_w / width, room_h / height});
    }
    WindowFit fit;
    fit.width = std::max(1, static_cast<int>(scale < 1.0 ? std::floor(width * scale) : width));
    fit.height = std::max(1, static_cast<int>(scale < 1.0 ? std::floor(height * scale) : height));
    fit.x = area_x + (area_w - (fit.width + extra_w)) / 2 + borders.left;
    fit.y = area_y + (area_h - (fit.height + extra_h)) / 2 + borders.top;
    return fit;
}

} // namespace aln
