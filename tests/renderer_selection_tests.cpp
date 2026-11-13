// Renderer policy tests: Vulkan is the only renderer a release runs, nothing offers another, and the OpenGL backend's
// requirements. Pure logic plus the sources that must agree with it: no window, no OpenGL or Vulkan context, no game files.
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "aln_renderer.h"
#include "video_core/renderer_opengl/gl_capabilities.h"
#include "video_core/renderer_opengl/gl_primitives.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

std::string ReadAll(const std::string& relative) {
    std::ifstream file(std::string(ALN_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void TestSelectionIsVulkan() {
    using namespace AlnRenderer;
    for (const char* request : {"", "auto", "vulkan", "opengl", "d3d11", "angle", "OpenGL", "direct3d11"}) {
        CHECK(Select(request, false) == Backend::Vulkan);
        CHECK(Select(request) == Select(request, DevRenderersBuilt));
    }
    CHECK(!DevRenderersBuilt);
    CHECK(Active() == Backend::Vulkan);
    CHECK(std::string(BackendName(Backend::Vulkan)) == "Vulkan");
    CHECK(std::string(BackendName(Backend::Direct3D11)) == "Direct3D 11 via ANGLE");
}

void TestDevelopmentBackends() {
    using namespace AlnRenderer;
    CHECK(Select("opengl", true) == Backend::OpenGL);
    CHECK(Select("auto", true) == Backend::Vulkan);
    CHECK(Select("angle", true) == Backend::Vulkan);
    CHECK(Select("", true) == Backend::Vulkan);
#ifdef _WIN32
    CHECK(Select("d3d11", true) == Backend::Direct3D11);
#else
    CHECK(Select("d3d11", true) == Backend::Vulkan);
#endif

    const std::string header = ReadAll("gpu/shim/aln_renderer.h");
    CHECK(header.find("#ifdef ALN_DEV_RENDERERS") != std::string::npos);
    const std::string gpu = ReadAll("gpu/shim/alngpu.cpp");
    CHECK(gpu.find("AlnRenderer::Select(request ? request : \"\")") != std::string::npos);
    CHECK(gpu.find("aln_check_vulkan(cause") == std::string::npos);
    const std::string cmake = ReadAll("gpu/CMakeLists.txt");
    CHECK(cmake.find("option(ALN_DEV_RENDERERS") != std::string::npos);
    for (const char* script : {"tools/build.ps1", "tools/build.sh", "tools/package.ps1", "tools/package-linux.sh"}) {
        CHECK(ReadAll(script).find("ALN_DEV_RENDERER") == std::string::npos);
    }
}

void TestNoFallback() {
    const std::string loader = ReadAll("src/loader.c");
    CHECK(loader.find("AlnGraphicsCheck check_graphics=system_usable;") != std::string::npos);
    CHECK(loader.find("aln_check_vulkan(cause,cause_size,advice,advice_size)") != std::string::npos);
    CHECK(loader.find("aln_check_renderer") == std::string::npos);
    const std::string list = ReadAll("gpu/shim/aln_gpu_list.cpp");
    CHECK(list.find("aln_check_renderer") == std::string::npos);
    CHECK(list.find("OpenGL") == std::string::npos);
    const std::string header = ReadAll("gpu/shim/aln_renderer.h");
    CHECK(header.find("fallback") == std::string::npos);
    CHECK(header.find("Resolve(") == std::string::npos);
    CHECK(ReadAll("launcher/aln_launcher.h").find("aln_check_renderer") == std::string::npos);
}

void TestNoRendererSetting() {
    const std::string settings = ReadAll("launcher/settings.h");
    CHECK(settings.find("kRendererChoices") == std::string::npos);
    CHECK(settings.find("kDefaultRenderer") == std::string::npos);
    CHECK(settings.find("RendererChoice") == std::string::npos);
    CHECK(settings.find("\"OpenGL") == std::string::npos);
    CHECK(settings.find("\"Direct3D") == std::string::npos);
    CHECK(settings.find("kObsoleteRendererKey[] = \"renderer\"") != std::string::npos);

    const std::string launcher_settings = ReadAll("launcher/settings.cpp");
    CHECK(launcher_settings.find("{kRendererKey") == std::string::npos);
    CHECK(launcher_settings.find("{kObsoleteRendererKey") == std::string::npos);
    CHECK(launcher_settings.find("RemoveObsoleteSettings(file_);") != std::string::npos);
    CHECK(launcher_settings.find("key == kObsoleteRendererKey") != std::string::npos);

    const std::string shim = ReadAll("gpu/shim/aln_settings.cpp");
    CHECK(shim.find("key == \"renderer\" ") == std::string::npos);
    CHECK(shim.find("v.renderer") == std::string::npos);
    CHECK(shim.find("return ParseLine(line, key, value) && key == \"renderer\";") != std::string::npos);
    CHECK(ReadAll("gpu/shim/aln_settings.h").find("std::atomic<int> renderer") == std::string::npos);
}

void TestLauncherHasNoRendererRow() {
    const std::string launcher = ReadAll("launcher/launcher.cpp");
    CHECK(launcher.find("\"Renderer\", kRendererHelp") == std::string::npos);
    CHECK(launcher.find("kRendererHelp") == std::string::npos);
    CHECK(launcher.find("kRendererKey") == std::string::npos);
    CHECK(launcher.find("ui.Combo(\"Renderer\"") == std::string::npos);
    const size_t page = launcher.find("void PageDisplay(trg::Ui& ui)");
    const size_t end = launcher.find("const AlnAspectRatio& SavedAspectRatio()", page);
    CHECK(page != std::string::npos && end != std::string::npos);
    const std::string body = launcher.substr(page, end - page);
    CHECK(body.find("Renderer") == std::string::npos);
    CHECK(body.find("Vulkan") == std::string::npos);
    const size_t aspect = body.find("kAspectRatioKey,");
    const size_t card = body.find("ui.Row(\"Graphics card\"");
    CHECK(aspect != std::string::npos && card != std::string::npos && aspect < card);
    const std::string between = body.substr(aspect, card - aspect);
    CHECK(between.find("ui.Combo") == std::string::npos);
}

void TestOptionsMenuHasNoRenderer() {
    const std::string menu = ReadAll("gpu/shim/aln_options_menu.h");
    CHECK(menu.find("Renderer") == std::string::npos);
    CHECK(menu.find("renderer") == std::string::npos);
    CHECK(menu.find("kRenderers") == std::string::npos);
    CHECK(menu.find("OpenGL") == std::string::npos);
    CHECK(menu.find("Direct3D") == std::string::npos);
    CHECK(menu.find("ANGLE") == std::string::npos);
    CHECK(menu.find("\"Auto\"") == std::string::npos);
    const size_t graphics = menu.find("Page::Graphics, \"");
    CHECK(graphics != std::string::npos);
    CHECK(menu.find("Page::Graphics, \"render_resolution\", \"Render resolution\",", graphics) == graphics);
}

void TestDirect3D11Backend() {
    const std::string angle = ReadAll("gpu/shadps4/video_core/renderer_opengl/gl_angle.cpp");
    CHECK(angle.find("SDL_HINT_OPENGL_ES_DRIVER") != std::string::npos);
    CHECK(angle.find("SDL_HINT_VIDEO_EGL_ALLOW_GETDISPLAY_FALLBACK, \"0\"") != std::string::npos);
    CHECK(angle.find("PlatformAngleTypeD3D11") != std::string::npos);
    CHECK(angle.find("#ifdef _WIN32") != std::string::npos);
    CHECK(angle.find("LoadBesideExecutable(L\"libGLESv2.dll\")") != std::string::npos);
    CHECK(angle.find("LoadBesideExecutable(L\"libEGL.dll\")") != std::string::npos);

    // Native OpenGL keeps its own window path and never asks for ANGLE.
    const std::string window = ReadAll("gpu/shim/window.cpp");
    const size_t native = window.find("if (api == GraphicsApi::OpenGL) {");
    const size_t core = window.find("SDL_GL_CONTEXT_PROFILE_CORE", native);
    CHECK(native != std::string::npos && core != std::string::npos);
    CHECK(window.find("SDL_GL_CONTEXT_PROFILE_ES") == std::string::npos);
    CHECK(window.find("RequestAngleDirect3D11") < native);

    CHECK(ReadAll("gpu/angle-runtime.txt").find("clang64/bin/libGLESv2.dll") != std::string::npos);
}

// Non-indexed rectangles keep the attribute path that was validated at run time.
void TestRectPathsStaySeparate() {
    using OpenGL::RectListUsesTables;
    CHECK(!RectListUsesTables(true, false, true));
    CHECK(!RectListUsesTables(true, false, false));
    CHECK(!RectListUsesTables(true, true, false));
    CHECK(!RectListUsesTables(false, true, true));
    CHECK(RectListUsesTables(true, true, true));

    const std::string rasterizer = ReadAll("gpu/shadps4/video_core/renderer_opengl/gl_rasterizer.cpp");
    CHECK(rasterizer.find("RectListUsesTables(rect_list, is_indexed, indexed_rect_tables)") != std::string::npos);
    CHECK(rasterizer.find("rect_tables ? PrepareIndexedRectangles(") != std::string::npos);
    CHECK(rasterizer.find("BindVertexInputs(*program, instance_offset, moved_vertices, rect_list)") != std::string::npos);
    CHECK(rasterizer.find("if (rect_list && is_indexed && !rect_tables) {") < rasterizer.find("AttachedTargets targets{};\n    if (!BindTargets(*program, targets)) {"));
    CHECK(rasterizer.find("Report(\"indexed rectangle lists are not implemented\");") != std::string::npos);
    size_t calls = 0;
    for (size_t at = rasterizer.find("PrepareIndexedRectangles("); at != std::string::npos;
         at = rasterizer.find("PrepareIndexedRectangles(", at + 1)) {
        ++calls;
    }
    CHECK(calls == 2);
}

// The game code stays out of renderer choices: only the factory and the window know both.
void TestBoundary() {
    const std::string liverpool = ReadAll("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    CHECK(liverpool.find("Vulkan::Rasterizer") == std::string::npos);
    CHECK(liverpool.find("OpenGL") == std::string::npos);
    const std::string driver = ReadAll("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(driver.find("Vulkan::Presenter") == std::string::npos);
    CHECK(driver.find("OpenGL") == std::string::npos);
    const std::string factory = ReadAll("gpu/shadps4/video_core/renderer_factory.cpp");
    CHECK(factory.find("make_unique<Vulkan::Presenter>") != std::string::npos);
    CHECK(factory.find("make_unique<OpenGL::Presenter>") != std::string::npos);
    CHECK(factory.find("Active() == AlnRenderer::Backend::Vulkan") != std::string::npos);
}

void TestRequirements() {
    OpenGL::Capabilities caps;
    caps.major = 3;
    caps.minor = 3;
    caps.s3tc = true;
    caps.max_texture_buffer_texels = OpenGL::MinimumRingTexels;
    CHECK(OpenGL::MissingRequirement(caps).empty());

    auto old_context = caps;
    old_context.minor = 2;
    CHECK(OpenGL::MissingRequirement(old_context).find("3.3") != std::string::npos);
    auto no_s3tc = caps;
    no_s3tc.s3tc = false;
    CHECK(OpenGL::MissingRequirement(no_s3tc).find("s3tc") != std::string::npos);
    auto small_buffers = caps;
    small_buffers.max_texture_buffer_texels = 65536;
    CHECK(OpenGL::MissingRequirement(small_buffers).find("texture buffer") != std::string::npos);

    auto newer = caps;
    newer.major = 4;
    newer.minor = 0;
    CHECK(OpenGL::MissingRequirement(newer).empty());
}

void TestGlesRequirements() {
    OpenGL::Capabilities caps;
    caps.gles = true;
    caps.major = 3;
    caps.s3tc = true;
    caps.draw_buffers_blend = true;
    caps.max_texture_size = OpenGL::MinimumGlesTextureSize;
    CHECK(OpenGL::MissingRequirement(caps).empty());

    auto level_10_0 = caps;
    level_10_0.major = 2;
    CHECK(OpenGL::MissingRequirement(level_10_0).find("10_1") != std::string::npos);
    auto no_s3tc = caps;
    no_s3tc.s3tc = false;
    CHECK(OpenGL::MissingRequirement(no_s3tc).find("s3tc") != std::string::npos);
    auto no_blend = caps;
    no_blend.draw_buffers_blend = false;
    CHECK(OpenGL::MissingRequirement(no_blend).find("draw_buffers_indexed") != std::string::npos);
    auto small = caps;
    small.max_texture_size = 2048;
    CHECK(OpenGL::MissingRequirement(small).find("4096") != std::string::npos);

    using OpenGL::IsAngleDirect3D11;
    CHECK(IsAngleDirect3D11("ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 (0x00002786) Direct3D11 vs_5_0 ps_5_0, D3D11-32.0.15)"));
    CHECK(!IsAngleDirect3D11("ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 Direct3D9Ex vs_3_0 ps_3_0)"));
    CHECK(!IsAngleDirect3D11("ANGLE (NVIDIA, Vulkan 1.3.280, NVIDIA GeForce RTX 4070)"));
    CHECK(!IsAngleDirect3D11("NVIDIA GeForce RTX 4070/PCIe/SSE2"));
}

} // namespace

int main() {
    TestSelectionIsVulkan();
    TestDevelopmentBackends();
    TestNoFallback();
    TestNoRendererSetting();
    TestLauncherHasNoRendererRow();
    TestOptionsMenuHasNoRenderer();
    TestDirect3D11Backend();
    TestRectPathsStaySeparate();
    TestGlesRequirements();
    TestBoundary();
    TestRequirements();
    std::printf("renderer policy tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
