// SPDX-License-Identifier: GPL-2.0-or-later
// The Frame rate setting as presentation timing. The game runs at least one simulation step per frame and waits for
// each flip, which completes on the 60 Hz guest vblank, so the vblank and the flips stay exactly as they are: the
// setting only chooses which flips reach the window and how often the last frame is shown again between two flips.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace AlnFramePacing {

// The values alienation.ini stores; 0 is Unlimited.
inline constexpr std::array kFrameRateModes{30, 60, 90, 120, 144, 165, 240, 0};

// The ceiling of the repeat cadence when the setting is Unlimited and nothing else bounds it.
inline constexpr int kUnlimitedCadenceHz = 480;
inline constexpr int kGameHz = 60;

constexpr bool IsFrameRateMode(int setting) {
    for (const int mode : kFrameRateModes) {
        if (mode == setting) {
            return true;
        }
    }
    return false;
}

constexpr bool IsUnlimited(int setting) {
    return setting == 0;
}

// Anything but a stored value exactly is the original 60, as in the launcher.
inline int ParseFrameRate(const std::string& value) {
    for (const int mode : kFrameRateModes) {
        if (value == std::to_string(mode)) {
            return mode;
        }
    }
    return kGameHz;
}

// The rate the setting asks for; 0 = uncapped.
constexpr int RenderFps(int setting) {
    if (IsUnlimited(setting)) {
        return 0;
    }
    return IsFrameRateMode(setting) ? setting : kGameHz;
}

constexpr bool IsHighRate(int setting) {
    return IsUnlimited(setting) || RenderFps(setting) > kGameHz;
}

// The display's refresh rounded to the rate its modes are named by (59.94 -> 60, 143.856 -> 144).
constexpr int NominalHz(double hz) {
    return static_cast<int>(hz + 0.5);
}

// The rate frames can actually reach the screen at; 0 = uncapped. A VSync present queue never goes past the display.
constexpr int EffectiveRenderHz(int setting, bool vsync = false, double display_hz = 0.0) {
    if (!IsHighRate(setting) || !vsync || display_hz <= 0.0) {
        return RenderFps(setting);
    }
    const int display = NominalHz(display_hz);
    return IsUnlimited(setting) ? display : std::min(RenderFps(setting), display);
}

// Presentations per second the host schedules, new frames and repeats together; 0 = only the game's own flips.
constexpr unsigned RepeatCadenceHz(int setting, bool vsync = false, double display_hz = 0.0) {
    if (!IsHighRate(setting)) {
        return 0u;
    }
    const int hz = EffectiveRenderHz(setting, vsync, display_hz);
    const int cadence = hz == 0 ? kUnlimitedCadenceHz : std::min(hz, kUnlimitedCadenceHz);
    return cadence > kGameHz ? static_cast<unsigned>(cadence) : 0u;
}

// 30 shows every second flip of a game that flips every vblank; every flip is still consumed. A game already flipping
// every second vblank (flip_rate 1 or more) is shown whole, so 30 never halves it again.
constexpr bool PresentsFlip(int setting, std::uint64_t flip_index, int flip_rate = 0) {
    return RenderFps(setting) != 30 || flip_rate > 0 || flip_index % 2 == 0;
}

// Releases one presentation per period on absolute slots of a monotonic clock, so a late wake does not shift later
// slots. Only a presentation more than one period late re-anchors to now + period: lateness is never repaid in a burst.
class FrameLimiter {
public:
    using Clock = std::chrono::steady_clock;

    FrameLimiter(unsigned fps, Clock::time_point start)
        : period_(fps ? std::chrono::nanoseconds(1'000'000'000LL / fps) : std::chrono::nanoseconds(0)),
          next_(start) {}

    bool Enabled() const { return period_.count() != 0; }
    Clock::time_point NextSlot() const { return next_; }
    std::chrono::nanoseconds Period() const { return period_; }

    // The next repeat if it falls before `deadline` (the next vblank); a repeat never delays a new frame.
    std::optional<Clock::time_point> SlotBefore(Clock::time_point deadline) const {
        if (!Enabled() || next_ >= deadline) {
            return std::nullopt;
        }
        return next_;
    }

    void Release(Clock::time_point now) {
        next_ = now - next_ > period_ ? now + period_ : next_ + period_;
    }

private:
    std::chrono::nanoseconds period_;
    Clock::time_point next_;
};

inline std::string ModeName(int setting) {
    return setting == 0 ? "Unlimited" : std::to_string(setting);
}

} // namespace AlnFramePacing
