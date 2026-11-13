// SPDX-License-Identifier: GPL-2.0-or-later
// Which renderer runs the game. Release builds always run Vulkan; the OpenGL and Direct3D 11 backends are
// development-only and are reachable only in a build made with ALN_DEV_RENDERERS.
#pragma once

#include <atomic>
#include <string_view>

namespace AlnRenderer {

enum class Backend { Vulkan, OpenGL, Direct3D11 };

inline const char* BackendName(Backend backend) {
    switch (backend) {
    case Backend::OpenGL:
        return "OpenGL";
    case Backend::Direct3D11:
        return "Direct3D 11 via ANGLE";
    default:
        return "Vulkan";
    }
}

#ifdef ALN_DEV_RENDERERS
inline constexpr bool DevRenderersBuilt = true;
#else
inline constexpr bool DevRenderersBuilt = false;
#endif

/// `dev_request` is the ALN_DEV_RENDERER environment value; it counts only in a development build.
inline Backend Select(std::string_view dev_request, bool dev_renderers_built = DevRenderersBuilt) {
    if (dev_renderers_built) {
        if (dev_request == "opengl") {
            return Backend::OpenGL;
        }
#ifdef _WIN32
        if (dev_request == "d3d11") {
            return Backend::Direct3D11;
        }
#endif
    }
    return Backend::Vulkan;
}

inline std::atomic<Backend> active_backend{Backend::Vulkan};

inline Backend Active() {
    return active_backend.load(std::memory_order_relaxed);
}

inline void SetActive(Backend backend) {
    active_backend.store(backend, std::memory_order_relaxed);
}

} // namespace AlnRenderer
