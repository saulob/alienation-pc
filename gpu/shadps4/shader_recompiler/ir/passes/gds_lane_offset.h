// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Optimization {

// Drops the per-lane rank added to or subtracted from a DataAppend/DataConsume result; the
// lowered per-lane atomic already returns a distinct value for every active lane.
void RemoveAppendBufferLaneOffset(IR::Inst& vx);

} // namespace Shader::Optimization
