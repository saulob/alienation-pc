// aln: see shader_crash_context.h.
#include <mutex>
#include <utility>
#include "common/gpu_trace.h"
#include "common/logging/log.h"
#include "common/shader_crash_context.h"

namespace Common::ShaderCrash {
namespace {
constexpr size_t MaxDetailBytes = 2048;

struct State {
    bool active{};
    std::string hw_stage;
    std::string sw_stage;
    u64 hash{};
    u64 pipeline{};
    const char* pass{"unknown"};
    std::string detail;
};

thread_local State state;
std::mutex identity_mutex;
std::string build_identity;
} // namespace

void SetBuildIdentity(std::string identity) {
    std::scoped_lock lock{identity_mutex};
    build_identity = std::move(identity);
}

Scope::Scope(std::string hw_stage, std::string sw_stage, u64 hash, u64 pipeline) {
    GpuTrace::Record(GpuTrace::Event::ShaderCompile, hash, GpuTrace::Tag(hw_stage.c_str()),
                     pipeline);
    state = State{true, std::move(hw_stage), std::move(sw_stage), hash, pipeline, "decode", {}};
}

Scope::~Scope() {
    state = State{};
}

bool Active() {
    return state.active;
}

void SetPass(const char* pass) {
    state.pass = pass;
}

void SetDetail(std::string detail) {
    if (detail.size() > MaxDetailBytes) {
        detail.resize(MaxDetailBytes);
        detail += "...";
    }
    state.detail = std::move(detail);
}

void Report() {
    std::string identity;
    {
        std::scoped_lock lock{identity_mutex};
        identity = build_identity;
    }
    if (!state.active) {
        LOG_CRITICAL(Debug, "Guest build: {}; no shader compilation active on this thread",
                     identity.empty() ? "unknown" : identity);
        return;
    }
    LOG_CRITICAL(Debug,
                 "Shader compile failure: guest build {}; stage {} (sw {}); shader hash {:#018x}; "
                 "pipeline {:#x}; pass {}",
                 identity.empty() ? "unknown" : identity, state.hw_stage, state.sw_stage,
                 state.hash, state.pipeline, state.pass);
    if (!state.detail.empty()) {
        LOG_CRITICAL(Debug, "Shader compile detail: {}", state.detail);
    }
}

} // namespace Common::ShaderCrash
