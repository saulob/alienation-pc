// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

// Scalar output components per emitted vertex, built-ins included.
u32 GeometryOutputComponents(const Info& info);

// The OutputVertices execution mode: the GSVS ring bound, within the device limits.
u32 GeometryOutputVertices(const Info& info, const RuntimeInfo& runtime_info,
                           const Profile& profile);

} // namespace Shader::Backend::SPIRV
