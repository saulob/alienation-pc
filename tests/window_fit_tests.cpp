// Focused tests of the windowed-mode fit: fit only when needed, one common factor, border fallback, centring, and the
// launcher/window source contracts that keep the saved size untouched.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "aln_window_fit.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

struct Area {
    int x, y, w, h;
};

const aln::WindowBorders kBars{31, 1, 1, 1};

std::string ReadSource(const char* path) {
    std::ifstream file(std::string(ALN_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool Inside(const aln::WindowFit& f, aln::WindowBorders b, const Area& a) {
    return f.x - b.left >= a.x && f.y - b.top >= a.y && f.x + f.width + b.right <= a.x + a.w &&
           f.y + f.height + b.bottom <= a.y + a.h;
}

void TestAlreadyFitsIsUntouched() {
    const aln::WindowFit f = aln::FitWindowToArea(1280, 720, kBars, 0, 0, 1920, 1040);
    CHECK(f.width == 1280 && f.height == 720);

    // Fits the usable area but exceeds 80% of it: the old share rule shrank this.
    const aln::WindowFit near = aln::FitWindowToArea(1800, 960, kBars, 0, 0, 1920, 1040);
    CHECK(near.width == 1800 && near.height == 960);
    CHECK(1800 > 1920 * 0.8 && 960 > 1040 * 0.8);
}

void TestExactFit() {
    const aln::WindowFit f = aln::FitWindowToArea(1918, 1008, kBars, 0, 0, 1920, 1040);
    CHECK(f.width == 1918 && f.height == 1008);
    CHECK(f.x - 1 == 0 && f.y - 31 == 0);
}

void TestConstrainedAxes() {
    // Width-limited.
    const aln::WindowFit w = aln::FitWindowToArea(3840, 1000, kBars, 0, 0, 1920, 2000);
    CHECK(w.width == 1918 && w.height == 499);
    // Height-limited.
    const aln::WindowFit h = aln::FitWindowToArea(1000, 3840, kBars, 0, 0, 4000, 1040);
    CHECK(h.height == 1008 && h.width == 262);
    // Both constrained: the smaller scale wins.
    const aln::WindowFit both = aln::FitWindowToArea(3840, 2160, kBars, 0, 0, 1920, 1040);
    CHECK(both.height == 1008 && both.width == 1792);
    CHECK(both.width + 2 <= 1920 && both.height + 32 <= 1040);
}

void TestSizesAcrossAreas() {
    const Area areas[] = {{0, 0, 1920, 1040}, {0, 0, 2560, 1400}, {0, 0, 3840, 2100}, {0, 0, 1366, 728}};
    const int sources[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (const Area& a : areas) {
        for (const auto& s : sources) {
            const aln::WindowFit f = aln::FitWindowToArea(s[0], s[1], kBars, a.x, a.y, a.w, a.h);
            CHECK(f.width > 0 && f.height > 0);
            CHECK(f.width <= s[0] && f.height <= s[1]);
            CHECK(std::abs(f.width * 9 - f.height * 16) <= 16);
            CHECK(Inside(f, kBars, a));
            if (s[0] + 2 <= a.w && s[1] + 32 <= a.h) {
                CHECK(f.width == s[0] && f.height == s[1]);
            }
        }
    }
}

void TestOtherAspectRatios() {
    const int shapes[][2] = {{3440, 1440}, {5120, 1440}, {1440, 1440}};
    for (const auto& s : shapes) {
        const aln::WindowFit f = aln::FitWindowToArea(s[0], s[1], kBars, 0, 0, 1920, 1040);
        CHECK(std::abs(f.width * s[1] - f.height * s[0]) <= s[0] + s[1]);
        CHECK(Inside(f, kBars, {0, 0, 1920, 1040}));
    }
}

void TestTinyArea() {
    const aln::WindowFit f = aln::FitWindowToArea(3840, 2160, kBars, 0, 0, 10, 10);
    CHECK(f.width >= 1 && f.height >= 1);
    const aln::WindowFit z = aln::FitWindowToArea(1280, 720, kBars, 0, 0, 0, 0);
    CHECK(z.width == 1280 && z.height == 720);
}

void TestBorders() {
    const aln::WindowFit none = aln::FitWindowToArea(1920, 1080, {0, 0, 0, 0}, 0, 0, 1920, 1080);
    CHECK(none.width == 1920 && none.height == 1080 && none.x == 0 && none.y == 0);

    const aln::WindowBorders hidpi{70, 2, 2, 2};
    const Area a{0, 0, 3840, 2100};
    const aln::WindowFit f = aln::FitWindowToArea(3840, 2160, hidpi, a.x, a.y, a.w, a.h);
    CHECK(Inside(f, hidpi, a) && f.width < 3840);
    const aln::WindowFit win = aln::FitWindowToArea(3840, 2160, kBars, a.x, a.y, a.w, a.h);
    CHECK(Inside(win, kBars, a) && win.width > f.width);
}

void TestEstimatedBorders() {
    const float scales[] = {1.0f, 1.5f, 2.25f};
    int previous_top = 0;
    for (const float s : scales) {
        const aln::WindowBorders b = aln::EstimatedBorders(s);
        CHECK(b.top >= 0 && b.left >= 0 && b.bottom >= 0 && b.right >= 0);
        CHECK(b.top > previous_top && b.top < 100 && b.left <= 4);
        previous_top = b.top;
    }
    const aln::WindowBorders one = aln::EstimatedBorders(1.0f);
    CHECK(one.top == 31 && one.left == 1);
    const aln::WindowBorders bad = aln::EstimatedBorders(0.0f);
    CHECK(bad.top == 31);
}

void TestScaleIsNotAppliedToClient() {
    // The requested client size is in pixels; only the borders scale.
    for (const float s : {1.0f, 1.5f, 2.25f}) {
        const aln::WindowFit f = aln::FitWindowToArea(1280, 720, aln::EstimatedBorders(s), 0, 0, 3840, 2100);
        CHECK(f.width == 1280 && f.height == 720);
    }
}

void TestCentring() {
    const aln::WindowFit f = aln::FitWindowToArea(1280, 720, kBars, 0, 0, 1920, 1040);
    CHECK(f.x - 1 == (1920 - (f.width + 2)) / 2);
    CHECK(f.y - 31 == (1040 - (f.height + 32)) / 2);

    const aln::WindowFit n = aln::FitWindowToArea(1920, 1080, kBars, -2560, -200, 2560, 1400);
    CHECK(Inside(n, kBars, {-2560, -200, 2560, 1400}));
    CHECK((n.x - 1) - (-2560) == (2560 - (n.width + 2)) / 2);
}

void TestMultiMonitor() {
    const Area small{0, 0, 1920, 1040};
    const Area large{1920, -200, 3840, 2100};
    const aln::WindowFit a = aln::FitWindowToArea(3840, 2160, kBars, small.x, small.y, small.w, small.h);
    const aln::WindowFit b = aln::FitWindowToArea(3840, 2160, kBars, large.x, large.y, large.w, large.h);
    CHECK(a.width < 3840);
    CHECK(b.width < 3840 && b.height <= 2068 && b.height >= 2066);
    const aln::WindowFit c = aln::FitWindowToArea(1920, 1080, kBars, large.x, large.y, large.w, large.h);
    CHECK(c.width == 1920 && c.height == 1080);
    CHECK(Inside(a, kBars, small) && Inside(b, kBars, large) && Inside(c, kBars, large));
    const aln::WindowFit fits_big = aln::FitWindowToArea(3800, 2000, kBars, large.x, large.y, large.w, large.h);
    CHECK(fits_big.width == 3800 && fits_big.height == 2000);
}

void TestWindowSourceContract() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    const auto at = [&](const std::string& needle, size_t from = 0) { return window.find(needle, from); };
    const size_t hidden = at("SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN");
    const size_t create = at("SDL_CreateWindowWithProperties");
    const size_t borders = at("SDL_GetWindowBordersSize");
    const size_t estimate = at("EstimatedBorders(display->scale)");
    const size_t fit = at("FitWindowToArea(");
    const size_t position = at("SDL_SetWindowPosition(window, fit.x, fit.y)");
    const size_t show = at("SDL_ShowWindow(window)");
    CHECK(hidden != std::string::npos && create != std::string::npos && hidden < create);
    CHECK(create < borders && borders < estimate && estimate < fit && fit < position && position < show);
    CHECK(Contains(window, "if (!SDL_GetWindowBordersSize("));
    CHECK(window.find("FitWindowToArea(", fit + 1) == std::string::npos);
    CHECK(window.find("SDL_SetWindowPosition(", position + 1) == std::string::npos);
    CHECK(!Contains(window, "FitToDisplay"));
    CHECK(Contains(window, "Window fit: bounds"));
    CHECK(!Contains(window, "SaveSettings") && !Contains(window, "settings.Save") && !Contains(window, "SetInt("));
}

void TestLauncherSourceContract() {
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    const size_t combo = launcher.find("void WindowSizeCombo()");
    const size_t end = launcher.find("void GraphicsCardCombo()");
    CHECK(combo != std::string::npos && end != std::string::npos && combo < end);
    const std::string body = launcher.substr(combo, end - combo);
    CHECK(Contains(body, "WindowSizesFor(SavedAspectRatio())"));
    CHECK(!Contains(body, "continue;"));
    CHECK(!Contains(body, "Fits") && !Contains(body, "Display"));
    CHECK(Contains(body, "settings_.SetInt(kWindowWidthKey, size.width)"));
    CHECK(Contains(body, "settings_.SetInt(kWindowHeightKey, size.height)"));
    CHECK(!Contains(launcher, "WindowSizeFits") && !Contains(launcher, "FitGameWindow"));

    const std::string options = ReadSource("launcher/options.cpp") + ReadSource("launcher/options.h");
    CHECK(!Contains(options, "WindowSizeFits") && !Contains(options, "FitGameWindow"));
    CHECK(Contains(options, "WindowSizesFor(const AlnAspectRatio& aspect)"));
    CHECK(!Contains(ReadSource("src/aln_displays.h"), "FitToDisplay"));
}

} // namespace

int main() {
    TestAlreadyFitsIsUntouched();
    TestExactFit();
    TestConstrainedAxes();
    TestSizesAcrossAreas();
    TestOtherAspectRatios();
    TestTinyArea();
    TestBorders();
    TestEstimatedBorders();
    TestScaleIsNotAppliedToClient();
    TestCentring();
    TestMultiMonitor();
    TestWindowSourceContract();
    TestLauncherSourceContract();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
