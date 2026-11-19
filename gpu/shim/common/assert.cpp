// aln: GPU-side assertion failures stop the port with exit code 23.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/gpu_trace.h"
#include "common/logging/log.h"
#include "common/shader_crash_context.h"

namespace {
std::atomic<void (*)(const char*, int)> fatal_stop{};
}

extern "C" void alngpu_set_fatal_stop(void (*stop)(const char* what, int status)) {
    fatal_stop.store(stop, std::memory_order_release);
}

void assert_fail_impl() {
    Common::ShaderCrash::Report();
    Common::GpuTrace::Dump("GPU assertion failed");
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    if (auto* stop = fatal_stop.load(std::memory_order_acquire)) {
        stop("GPU library assertion failed", 23);
    }
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
