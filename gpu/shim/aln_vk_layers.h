// aln: the Vulkan implicit layers registered on this machine and whether each one's library is
// loaded into the process. The loader enables implicit layers without the game requesting them.
#pragma once
#include <string>

namespace AlnVkLayers {

// One line, e.g. "VK_LAYER_RTSS (RTSSVkLayer64.dll, loaded); VK_LAYER_X (x.dll, not loaded)".
std::string DescribeImplicitLayers();

// File name of the module holding `address`, e.g. "nvoglv64.dll", or "unknown".
std::string ModuleOf(const void* address);

} // namespace AlnVkLayers
