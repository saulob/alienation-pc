// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/types.h"

namespace Shader {
struct Info;
}

namespace Shader::IR {
struct Program;
}

namespace Shader::Optimization {

/// Particle spawn loops append to a GDS counter once per particle and write only while the
/// returned index is below a fixed budget, so a huge emitter count keeps a dispatch appending to
/// a spent budget for seconds. Once a lane's append returns an index at or above the budget, its
/// next append adds everything the lane has left and the loop ends: accepted particles and the
/// final counter value are those of the original loop. Returns how many loops were bounded.
u32 BoundSpawnAppendLoops(IR::Program& program);

/// The line a normal log carries for each shader translation that bounded a loop.
std::string SpawnLoopBoundNotice(u64 pgm_hash, u32 loops);

/// Nonzero for shaders the pass may change (compute shaders that use GDS): SPIR-V they cached
/// before the pass must not be reused.
u32 SpawnLoopBoundRevision(const Info& info);

} // namespace Shader::Optimization
