// aln: GPU-side assertion failures stop the port with exit code 23.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/device_lost.h"
#include "common/gpu_trace.h"
#include "common/logging/log.h"
#include "common/shader_crash_context.h"

namespace {
std::atomic<void (*)(const char*, int)> fatal_stop{};
}

extern "C" void alngpu_set_fatal_stop(void (*stop)(const char* what, int status)) {
    fatal_stop.store(stop, std::memory_order_release);
}

void Common::StopProcess(const char* what, int status) {
    std::fflush(stdout);
    if (auto* stop = fatal_stop.load(std::memory_order_acquire)) {
        stop(what, status);
    }
    std::fprintf(stderr, "Runtime: %s; the process ends with status %d\n", what, status);
    std::fflush(stderr);
    std::_Exit(status);
}

void assert_fail_impl() {
    // Once the device is lost, the first device-lost report is the crash reason, not this assertion.
    if (Common::DeviceLost::IsLost()) {
        Common::DeviceLost::Global().Observe("a GPU assertion (see the log line above)");
    }
    Common::ShaderCrash::Report();
    Common::GpuTrace::Dump("GPU assertion failed");
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    Common::StopProcess("GPU library assertion failed", 23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
