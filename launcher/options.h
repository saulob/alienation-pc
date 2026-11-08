// What the Display page offers and how it labels it.
#pragma once

#include <string>
#include <vector>

#include "aln_displays.h"
#include "aln_keys.h"
#include "aln_resolution.h"
#include "trg/settings.h"

namespace aln {

struct WindowSize {
    int width = 0;
    int height = 0;
    bool operator==(const WindowSize&) const = default;
};

// In pixels; the game's video output is 1920 x 1080.
inline constexpr int kWindowHeights[] = {720, 900, 1080, 1440, 2160};
inline constexpr WindowSize kGamePictureSize{1920, 1080};

WindowSize WindowSizeOf(int height, const AlnAspectRatio& aspect);

std::vector<WindowSize> WindowSizesFor(const AlnAspectRatio& aspect);

enum class PageId { kPlay, kDisplay, kGraphics, kGameplay, kControls, kOnline, kTrophies, kBugReport, kAbout };

inline constexpr PageId kPageOrder[] = {PageId::kPlay,   PageId::kDisplay,  PageId::kGraphics, PageId::kGameplay,
                                        PageId::kOnline, PageId::kControls, PageId::kTrophies, PageId::kBugReport,
                                        PageId::kAbout};

const char* PageName(PageId page);

// The window a requested size becomes on this display: unchanged when it fits, else shrunk to fit, keeping its shape.
WindowSize FitGameWindow(WindowSize requested, const Display& display);

// Whether the size fits the display, so the game window is not larger than the screen.
bool WindowSizeFits(WindowSize size, const Display& display);

// "Native — 1920 × 1080".
std::string ResolutionLabel(const AlnResolution& resolution);

// "1920 × 1080", with a note on the game's own picture size.
std::string WindowSizeLabel(WindowSize size);

// "Display 2 (primary)   2560 × 1440   Name". Monitor settings are 1-based.
std::string MonitorLabel(int monitor, const std::vector<Display>& displays);

// The Controls page's bindings, stored one key name per action in the settings (aln_keys.h).
// "Space", "Left Shift", "Enter", "F10".
std::string KeyDisplayName(int scancode);
// The saved key of an action; the default when the setting is missing or names no usable key.
int SavedKey(const trg::SettingsStore& settings, AlnKeyAction action);
void SetKey(trg::SettingsStore& settings, AlnKeyAction action, int scancode);
// Puts every keyboard control and port hotkey back to its default and nothing else.
void ResetKeys(trg::SettingsStore& settings);

} // namespace aln
