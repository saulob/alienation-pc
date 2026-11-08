// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/gpu_trace.h"
#include "common/logging/log.h"
#include "shader_recompiler/backend/spirv/geometry_output.h"

namespace Shader::Backend::SPIRV {

u32 GeometryOutputComponents(const Info& info) {
    u32 components = 4 + info.gs_copy_data.num_attrs * 4;
    if (info.stores.GetAny(IR::Attribute::ClipDistance)) {
        components += 8;
    }
    if (info.stores.GetAny(IR::Attribute::CullDistance)) {
        components += 8;
    }
    for (const auto attr : {IR::Attribute::PointSize, IR::Attribute::RenderTargetIndex,
                            IR::Attribute::ViewportIndex}) {
        components += info.stores.GetAny(attr) ? 1 : 0;
    }
    return components;
}

// The GSVS ring stores component c of vertex v at (c * ring_vertices + v), so a GS never emits
// more than ring_vertices vertices even when VGT_GS_MAX_VERT_OUT is larger.
u32 GeometryOutputVertices(const Info& info, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    const u32 max_vert_out = runtime_info.hw.gs.output_vertices;
    const u32 ring_vertices = info.gs_copy_data.output_vertices;
    u32 vertices = ring_vertices != 0 ? std::min(max_vert_out, ring_vertices) : max_vert_out;
    const u32 components = GeometryOutputComponents(info);
    if (profile.max_geometry_output_vertices != 0) {
        vertices = std::min(vertices, profile.max_geometry_output_vertices);
    }
    if (profile.max_geometry_total_output_components != 0 &&
        vertices * components > profile.max_geometry_total_output_components) {
        const u32 limited = std::max(1u, profile.max_geometry_total_output_components / components);
        LOG_WARNING(Render_Vulkan,
                    "GS {:#x}: {} output vertices of {} components exceed the device limit of {} "
                    "components; declaring {}",
                    info.pgm_hash, vertices, components,
                    profile.max_geometry_total_output_components, limited);
        vertices = limited;
    }
    if (vertices != max_vert_out) {
        Common::GpuTrace::Record(Common::GpuTrace::Event::GeometryOutputClamp, info.pgm_hash,
                                 max_vert_out, vertices, components);
    }
    return vertices;
}

} // namespace Shader::Backend::SPIRV
