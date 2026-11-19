// Interface agreement between the stage that writes fragment shader inputs and the fragment shader
// (ALN-VUK9PE): both sides take the NVIDIA clip-distance emulation decision from the production rule.
#include <cstdio>
#include <utility>

#include "shader_recompiler/runtime_info.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace {

using namespace Shader;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                      \
        }                                                                                          \
    } while (0)

// The stage whose outputs the fragment shader reads, as the recompiler compiles it.
std::pair<HwStage, SwStage> FragmentInputWriter(bool tessellation, bool geometry) {
    if (geometry) {
        return {HwStage::Geometry, SwStage::Geometry};
    }
    return {HwStage::Vertex, tessellation ? SwStage::TessellationEval : SwStage::Vertex};
}

bool FragmentReads(bool tessellation, bool geometry, bool needs_emulation) {
    return EmulatesClipDistances(HwStage::Vertex, HardwareVsProgram(tessellation, geometry), needs_emulation);
}

void TestEveryStageConfigurationAgrees() {
    for (const bool needs_emulation : {false, true}) {
        for (const bool tessellation : {false, true}) {
            for (const bool geometry : {false, true}) {
                const auto [hw, sw] = FragmentInputWriter(tessellation, geometry);
                CHECK(FragmentReads(tessellation, geometry, needs_emulation) ==
                      EmulatesClipDistances(hw, sw, needs_emulation));
            }
        }
    }
}

// The decal pipeline shot impacts draw on NVIDIA: ES + GS, with clip_distance_enable left set by an
// earlier draw. The geometry shader writes its parameters from location 0 and the real ClipDistance
// built-in; a fragment shader shifted by one location read the wrong parameters, discarded on them
// and read a location nothing wrote.
void TestGeometryPipelinesDoNotShiftFragmentInputs() {
    CHECK(!FragmentReads(false, true, true));
    CHECK(!FragmentReads(true, true, true));
    CHECK(!EmulatesClipDistances(HwStage::Geometry, SwStage::Geometry, true));
    CHECK(!EmulatesClipDistances(HwStage::Export, SwStage::Vertex, true));
}

void TestTessellationDoesNotShiftFragmentInputs() {
    CHECK(!FragmentReads(true, false, true));
    CHECK(!EmulatesClipDistances(HwStage::Vertex, SwStage::TessellationEval, true));
}

void TestVertexPipelinesStillEmulateOnlyWhereNeeded() {
    CHECK(FragmentReads(false, false, true));
    CHECK(EmulatesClipDistances(HwStage::Vertex, SwStage::Vertex, true));
    CHECK(!FragmentReads(false, false, false));
    CHECK(!EmulatesClipDistances(HwStage::Vertex, SwStage::Vertex, false));
}

// Pipelines without a pixel shader decide their discard-only fragment shader from their own registers
// under the same rule; before, they reused whatever fragment state the previous draw had left.
void TestFragmentDecisionFollowsOwnRegisters() {
    for (const bool tessellation : {false, true}) {
        for (const bool geometry : {false, true}) {
            const bool vertex_writes = FragmentReads(tessellation, geometry, true);
            for (const bool clip : {false, true}) {
                for (const bool user_planes : {false, true}) {
                    CHECK(FragmentClipDistanceEmulation(tessellation, geometry, clip, user_planes, true) ==
                          (vertex_writes && (clip || user_planes)));
                    CHECK(!FragmentClipDistanceEmulation(tessellation, geometry, clip, user_planes, false));
                }
            }
        }
    }
    CHECK(FragmentClipDistanceEmulation(false, false, true, false, true));
    CHECK(!FragmentClipDistanceEmulation(false, true, true, false, true));
    CHECK(!FragmentClipDistanceEmulation(true, false, false, true, true));
}

void TestHardwareVsProgram() {
    CHECK(HardwareVsProgram(false, false) == SwStage::Vertex);
    CHECK(HardwareVsProgram(true, false) == SwStage::TessellationEval);
    CHECK(HardwareVsProgram(false, true) == SwStage::Geometry);
    CHECK(HardwareVsProgram(true, true) == SwStage::Geometry);
}

} // namespace

int main() {
    TestEveryStageConfigurationAgrees();
    TestGeometryPipelinesDoNotShiftFragmentInputs();
    TestTessellationDoesNotShiftFragmentInputs();
    TestVertexPipelinesStillEmulateOnlyWhereNeeded();
    TestFragmentDecisionFollowsOwnRegisters();
    TestHardwareVsProgram();
    std::printf("shader interface tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
