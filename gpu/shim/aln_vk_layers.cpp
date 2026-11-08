// aln: see aln_vk_layers.h.
#include "aln_vk_layers.h"

#ifdef _WIN32
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>
#include <windows.h>

namespace AlnVkLayers {
namespace {

struct Layer {
    std::string name;
    std::string library;
    std::string disable_env;
};

// The value of `"key": "value"` at or after `from`, or empty.
std::string JsonString(const std::string& text, const char* key, size_t from, size_t* end) {
    const std::string quoted = std::string{"\""} + key + "\"";
    const size_t key_pos = text.find(quoted, from);
    if (key_pos == std::string::npos) {
        return {};
    }
    const size_t open = text.find('"', text.find(':', key_pos + quoted.size()) + 1);
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    if (end) {
        *end = close + 1;
    }
    return text.substr(open + 1, close - open - 1);
}

// disable_environment is an object whose first key names the variable.
std::string DisableVariable(const std::string& text, size_t from, size_t to) {
    const size_t key_pos = text.find("\"disable_environment\"", from);
    if (key_pos == std::string::npos || key_pos > to) {
        return {};
    }
    const size_t open = text.find('"', text.find('{', key_pos) + 1);
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    return close == std::string::npos ? std::string{} : text.substr(open + 1, close - open - 1);
}

std::vector<Layer> ParseManifest(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{file}, {}};
    std::vector<Layer> layers;
    size_t pos = 0;
    while (true) {
        size_t name_end = 0;
        std::string name = JsonString(text, "name", pos, &name_end);
        if (name.empty()) {
            break;
        }
        size_t library_end = 0;
        std::string library = JsonString(text, "library_path", name_end, &library_end);
        const size_t next = text.find("\"name\"", name_end);
        if (library.empty() || (next != std::string::npos && library_end > next)) {
            pos = name_end;
            continue;
        }
        layers.push_back({std::move(name), std::move(library),
                          DisableVariable(text, name_end,
                                          next == std::string::npos ? text.size() : next)});
        pos = library_end;
    }
    return layers;
}

std::vector<std::filesystem::path> EnabledManifests(HKEY root) {
    std::vector<std::filesystem::path> manifests;
    HKEY key{};
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return manifests;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t name[1024];
        DWORD name_size = DWORD(std::size(name));
        DWORD type = 0;
        DWORD value = 1;
        DWORD value_size = sizeof(value);
        const LONG result = RegEnumValueW(key, index, name, &name_size, nullptr, &type,
                                          reinterpret_cast<BYTE*>(&value), &value_size);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result == ERROR_SUCCESS && type == REG_DWORD && value == 0) {
            manifests.emplace_back(name);
        }
    }
    RegCloseKey(key);
    return manifests;
}

} // namespace

std::string DescribeImplicitLayers() {
    std::ostringstream out;
    size_t count = 0;
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (const auto& manifest : EnabledManifests(root)) {
            for (const Layer& layer : ParseManifest(manifest)) {
                std::filesystem::path library = std::filesystem::path{layer.library};
                if (library.is_relative() && library.has_parent_path()) {
                    library = manifest.parent_path() / library;
                }
                const bool loaded = GetModuleHandleW(library.filename().c_str()) != nullptr;
                const bool disabled =
                    !layer.disable_env.empty() && std::getenv(layer.disable_env.c_str()) != nullptr;
                out << (count++ ? "; " : "") << layer.name << " (" << library.filename().string()
                    << (loaded ? ", loaded" : ", not loaded")
                    << (disabled ? ", disabled by " + layer.disable_env : "") << ")";
            }
        }
    }
    return count ? out.str() : "none registered";
}

std::string ModuleOf(const void* address) {
    HMODULE module{};
    wchar_t path[MAX_PATH];
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH)) {
        return "unknown";
    }
    return std::filesystem::path{path}.filename().string();
}

} // namespace AlnVkLayers

#else

namespace AlnVkLayers {
std::string DescribeImplicitLayers() {
    return "not inspected on this platform";
}
std::string ModuleOf(const void*) {
    return "unknown";
}
} // namespace AlnVkLayers

#endif
