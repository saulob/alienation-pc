// SPDX-License-Identifier: GPL-2.0-or-later
// The frame rate as the game makes it (guest flips) and as the window shows it (presents, repeats included), for the
// frame counter, the window title and the periodic log line.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace AlnFrameReport {

struct Counters {
    std::uint64_t guest_flips = 0; ///< flips the game submitted and the vblank thread completed
    std::uint64_t presents = 0;    ///< every present to the swapchain, repeats included
    std::uint64_t repeats = 0;     ///< presents of a frame already shown
};

struct Rates {
    double game_fps = 0.0;
    double presented_fps = 0.0;
    double new_fps = 0.0;
    double repeat_fps = 0.0;
};

inline Rates RatesBetween(const Counters& from, const Counters& to, double seconds) {
    if (seconds <= 0.0) {
        return {};
    }
    const auto rate = [seconds](std::uint64_t a, std::uint64_t b) { return b > a ? double(b - a) / seconds : 0.0; };
    Rates r;
    r.game_fps = rate(from.guest_flips, to.guest_flips);
    r.presented_fps = rate(from.presents, to.presents);
    r.repeat_fps = std::min(rate(from.repeats, to.repeats), r.presented_fps);
    r.new_fps = r.presented_fps - r.repeat_fps;
    return r;
}

// 30 FPS halves the shown frames and repeats add presents: only then is the presentation rate worth naming.
inline bool PresentationDiffers(const Rates& r) {
    return std::abs(r.presented_fps - r.game_fps) >= 1.0;
}

// The frame counter: "34 FPS  29.4 ms", then "(160 presented)" when the window shows a different rate.
inline std::string CounterText(const Rates& r) {
    char text[96];
    const double ms = r.game_fps > 0.0 ? 1000.0 / r.game_fps : 0.0;
    if (PresentationDiffers(r)) {
        std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms  (%.0f presented)", r.game_fps, ms, r.presented_fps);
    } else {
        std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms", r.game_fps, ms);
    }
    return text;
}

// Shares of wall time of the GPU command thread, in percent.
struct GpuThreadLoad {
    double cpu = 0.0;        ///< on a CPU
    double idle = 0.0;       ///< waiting for the game to submit
    double gpu_wait = 0.0;   ///< waiting for GPU work to finish
    double recorder = 0.0;   ///< waiting for the Vulkan recording thread
    double host_copies = 0.0;
    double draws_per_frame = 0.0;
};

inline std::string SummaryLine(const Rates& r, const GpuThreadLoad& load) {
    char text[384];
    std::snprintf(text, sizeof(text),
                  "Frame rate: game %.1f FPS; presented %.1f a second (%.1f new, %.1f repeats); GPU command thread: "
                  "CPU %.0f%%, idle %.0f%%, waiting for the GPU %.0f%%, recorder %.0f%%, host copies %.0f%%; "
                  "%.0f draws a frame",
                  r.game_fps, r.presented_fps, r.new_fps, r.repeat_fps, load.cpu, load.idle, load.gpu_wait,
                  load.recorder, load.host_copies, load.draws_per_frame);
    return text;
}

// One summary a minute, and none for a minute without a guest flip (loading, a paused window).
class SummaryClock {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kInterval{60};

    explicit SummaryClock(Clock::time_point start) : window_start_(start) {}

    bool Due(Clock::time_point now) const { return now - window_start_ >= kInterval; }
    double Seconds(Clock::time_point now) const { return std::chrono::duration<double>(now - window_start_).count(); }
    void Restart(Clock::time_point now) { window_start_ = now; }

private:
    Clock::time_point window_start_;
};

} // namespace AlnFrameReport
