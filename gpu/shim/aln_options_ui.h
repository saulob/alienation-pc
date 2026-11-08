// SPDX-License-Identifier: GPL-2.0-or-later
// The Options menu's window: drawn and read once per overlay frame. The overlay owns when it opens and closes.
#pragma once

#include <cstdint>

namespace AlnOptionsUi {

enum class Result { Stay, Close };

void Reset();
Result Frame();

// Controller events, for the L3 + R3 chord: true when the button completes the chord on that controller.
bool TrackChord(uint32_t pad, int sdl_button, bool down);
void ForgetPad(uint32_t pad);

} // namespace AlnOptionsUi
