// GPU crash trace and geometry output tests: the OutputVertices bound of a translated geometry shader, and the
// bounded GPU operation trace printed on device loss. No Vulkan device, window or game files.
#include <cstdio>
#include <string>

#include "common/gpu_trace.h"
#include "shader_recompiler/backend/spirv/geometry_output.h"
#include "video_core/texture_cache/image.h"

namespace {

using namespace Shader;
using Shader::Backend::SPIRV::GeometryOutputComponents;
using Shader::Backend::SPIRV::GeometryOutputVertices;

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

struct Geometry {
    Info info{};
    RuntimeInfo runtime{};
    Profile profile{};

    Geometry(u32 max_vert_out, u32 ring_vertices, u32 num_attrs) {
        runtime.Initialize(HwStage::Geometry, SwStage::Geometry);
        runtime.hw.gs.output_vertices = max_vert_out;
        info.gs_copy_data.output_vertices = ring_vertices;
        info.gs_copy_data.num_attrs = num_attrs;
    }
};

void RingBoundsMaxVertOut() {
    Geometry gs{48, 16, 2};
    CHECK(GeometryOutputVertices(gs.info, gs.runtime, gs.profile) == 16);
}

void MaxVertOutWhenRingIsLarger() {
    Geometry gs{4, 16, 2};
    CHECK(GeometryOutputVertices(gs.info, gs.runtime, gs.profile) == 4);
}

void MaxVertOutWithoutRingLayout() {
    Geometry gs{48, 0, 1};
    CHECK(GeometryOutputVertices(gs.info, gs.runtime, gs.profile) == 48);
}

void ComponentsCountBuiltIns() {
    Geometry gs{48, 16, 7};
    CHECK(GeometryOutputComponents(gs.info) == 32);
    gs.info.stores.Set(IR::Attribute::ClipDistance, 0);
    gs.info.stores.Set(IR::Attribute::PointSize, 0);
    CHECK(GeometryOutputComponents(gs.info) == 41);
}

void TotalComponentLimit() {
    // 48 vertices of 32 components exceed 1024; 16 do not.
    Geometry wide{48, 0, 7};
    wide.profile.max_geometry_total_output_components = 1024;
    wide.profile.max_geometry_output_vertices = 1024;
    CHECK(GeometryOutputVertices(wide.info, wide.runtime, wide.profile) == 32);

    Geometry ring{48, 16, 7};
    ring.profile = wide.profile;
    CHECK(GeometryOutputVertices(ring.info, ring.runtime, ring.profile) == 16);
}

void VertexLimit() {
    Geometry gs{300, 0, 0};
    gs.profile.max_geometry_output_vertices = 256;
    CHECK(GeometryOutputVertices(gs.info, gs.runtime, gs.profile) == 256);
}

std::string ReadFile(const char* path) {
    std::string text;
    if (FILE* file = std::fopen(path, "rb")) {
        char buffer[4096];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, read);
        }
        std::fclose(file);
    }
    return text;
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

void TraceIsBoundedAndPrintedOnce(const char* path) {
    using namespace Common::GpuTrace;
    SetDeviceIdentity("Test GPU (driver 1.0, Vulkan 1.3)");
    SetLayerSummary("requested none; implicit VK_LAYER_TEST (test.dll, loaded)");
    Record(Event::ShaderCompile, 0x73f13959, Tag("gs"), 0);
    Record(Event::PipelineFirstUse, u64(PipelineKind::Graphics), 0x94ece6df, 0x73f13959, 0x1234);
    for (u64 tick = 1; tick <= 2000; ++tick) {
        Record(Event::Submit, tick, 1, tick - 1, 0);
    }
    Record(Event::RenderingMismatch, 1);
    Record(Event::NonUniformSubgroupOp, 0x1234, Tag("Ballot"));
    Record(Event::AllocationFallback, 1920, 1080, 2);
    Record(Event::ForeignRecording, CurrentThreadId(), 7, reinterpret_cast<u64>("vk_rasterizer.cpp"),
           99);
    SetSubmittedTick(2000);
    SetCompletedTick(1990);
    SetLastDispatch(0xabcd, 8, 4, 1);
    SetPendingWork(3, 2);
    SetDispatchSummary("vkQueueSubmit test.dll, vkCmdDraw driver.dll");
    SetReplaySite("vk_rasterizer.cpp", 1234);

    std::fflush(stderr);
    if (!std::freopen(path, "w", stderr)) {
        CHECK(false);
        return;
    }
    VulkanFailure("semwait", -4, 1991);
    Dump("second dump is ignored");
    std::fflush(stderr);

    const std::string text = ReadFile(path);
    CHECK(Count(text, "GPU trace: Vulkan device lost") == 1);
    CHECK(text.find("second dump") == std::string::npos);
    CHECK(text.find("Test GPU (driver 1.0, Vulkan 1.3)") != std::string::npos);
    CHECK(text.find("VK_LAYER_TEST") != std::string::npos);
    CHECK(text.find("last submitted tick 2000, last completed tick 1990") != std::string::npos);
    CHECK(text.find("cs 0x0000abcd groups 8x4x1") != std::string::npos);
    CHECK(text.find("DEVICE LOST at semwait, result -4, tick 1991") != std::string::npos);
    // The ring keeps the newest 512 events: the early shader compile is gone, the last submits stay.
    CHECK(text.find("replay was inside a command recorded at vk_rasterizer.cpp:1234") != std::string::npos);
    CHECK(text.find("Vulkan dispatch vkQueueSubmit test.dll, vkCmdDraw driver.dll") != std::string::npos);
    CHECK(text.find("3 operations waited for GPU ticks, 2 recorded chunks were queued") != std::string::npos);
    CHECK(text.find("unpaired dynamic rendering (kind 1), corrected") != std::string::npos);
    CHECK(text.find("shader 0x0000000000001234: Ballot in non-uniform control flow") != std::string::npos);
    CHECK(text.find("image 1920x1080 allocated in system memory") != std::string::npos);
    CHECK(text.find("recorded while thread 7 owns recording, at vk_rasterizer.cpp:99") !=
          std::string::npos);
    CHECK(text.find("GPU trace: thread " + std::to_string(CurrentThreadId()) + " is ") !=
          std::string::npos);
    CHECK(text.find("detected on thread " + std::to_string(CurrentThreadId()) + " (") !=
          std::string::npos);
    CHECK(Count(text, " submit tick ") == 507);
    CHECK(text.find("submit tick 2000,") != std::string::npos);
    CHECK(text.find("compile gs") == std::string::npos);
}

void ImageBarrierStages() {
    using VideoCore::Image;
    using Stage = vk::PipelineStageFlagBits2;
    const auto read = vk::AccessFlagBits2::eShaderRead;
    const auto layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    const Image::State fragment_read{Stage::eFragmentShader, read, layout};
    CHECK(!Image::NeedsBarrier(fragment_read, layout, read, Stage::eFragmentShader));
    CHECK(Image::NeedsBarrier(fragment_read, layout, read, Stage::eComputeShader));
    CHECK(Image::NeedsBarrier(fragment_read, layout, read,
                              Stage::eFragmentShader | Stage::eComputeShader));
    const Image::State all_read{Stage::eAllCommands, read, layout};
    CHECK(!Image::NeedsBarrier(all_read, layout, read, Stage::eComputeShader));
    const Image::State compute_write{Stage::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                                     vk::ImageLayout::eGeneral};
    CHECK(Image::NeedsBarrier(compute_write, vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderWrite, Stage::eComputeShader));
    CHECK(Image::NeedsBarrier(fragment_read, vk::ImageLayout::eGeneral, read, Stage::eFragmentShader));
}

} // namespace

int main(int argc, char** argv) {
    RingBoundsMaxVertOut();
    MaxVertOutWhenRingIsLarger();
    MaxVertOutWithoutRingLayout();
    ComponentsCountBuiltIns();
    TotalComponentLimit();
    VertexLimit();
    ImageBarrierStages();
    const std::string trace_path = std::string{argc > 0 ? argv[0] : "gpu_crash_trace_tests"} + ".trace.txt";
    TraceIsBoundedAndPrintedOnce(trace_path.c_str());
    std::remove(trace_path.c_str());
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
