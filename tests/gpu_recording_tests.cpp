// Deferred Vulkan command recording tests: chunk order, command lifetime and owned captures, replay-side dynamic
// rendering pairing and the recording owner handoff. No Vulkan device: the commands record nothing.
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/vk_recording_state.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// Normally the loader's: this test links the video core without it.
extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

namespace {

using Vulkan::RecordChunk;
using Vulkan::RecordingOwner;
using Vulkan::RenderingScope;

int g_checks = 0;
int g_failures = 0;
std::vector<int> executed;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

void CommandsRunInRecordedOrder() {
    auto chunk = std::make_unique<RecordChunk>();
    std::vector<int> order;
    for (int i = 0; i < 100; ++i) {
        CHECK(chunk->Push([&order, i](vk::CommandBuffer) { order.push_back(i); },
                          std::source_location::current()));
    }
    chunk->Execute(vk::CommandBuffer{});
    bool ordered = order.size() == 100;
    for (int i = 0; ordered && i < 100; ++i) {
        ordered = order[i] == i;
    }
    CHECK(ordered);
    CHECK(chunk->Empty());
    CHECK(chunk->Size() == 0);
}

void CapturesLiveUntilTheCommandRuns() {
    auto chunk = std::make_unique<RecordChunk>();
    std::weak_ptr<int> watch;
    int seen = 0;
    {
        auto owned = std::make_shared<int>(42);
        watch = owned;
        CHECK(chunk->Push([owned, &seen](vk::CommandBuffer) { seen = *owned; },
                          std::source_location::current()));
    }
    CHECK(!watch.expired());
    chunk->Execute(vk::CommandBuffer{});
    CHECK(seen == 42);
    CHECK(watch.expired());
}

void DataSharesTheChunkWithItsCommand() {
    auto chunk = std::make_unique<RecordChunk>();
    auto* data = static_cast<u32*>(chunk->Allocate(64 * sizeof(u32), alignof(u32)));
    CHECK(data != nullptr);
    for (u32 i = 0; i < 64; ++i) {
        data[i] = i * 3;
    }
    u32 sum = 0;
    CHECK(chunk->Push(
        [data, &sum](vk::CommandBuffer) {
            for (u32 i = 0; i < 64; ++i) {
                sum += data[i];
            }
        },
        std::source_location::current()));
    chunk->Execute(vk::CommandBuffer{});
    CHECK(sum == 3 * (63 * 64 / 2));
}

void FullChunkRefusesAndKeepsItsCommands() {
    auto chunk = std::make_unique<RecordChunk>();
    int runs = 0;
    CHECK(chunk->Allocate(RecordChunk::Capacity - 64, 8) != nullptr);
    struct Big {
        char bytes[256];
    };
    const Big big{};
    CHECK(!chunk->Push([big, &runs](vk::CommandBuffer) { runs += big.bytes[0] + 1; },
                       std::source_location::current()));
    chunk->Execute(vk::CommandBuffer{});
    CHECK(runs == 0);
    CHECK(chunk->Push([&runs](vk::CommandBuffer) { ++runs; }, std::source_location::current()));
    chunk->Execute(vk::CommandBuffer{});
    CHECK(runs == 1);
}

void RenderingScopePairing() {
    RenderingScope scope;
    CHECK(!scope.End());
    CHECK(!scope.Begin());
    CHECK(scope.IsOpen());
    CHECK(scope.Begin());
    CHECK(scope.End());
    CHECK(!scope.IsOpen());
    CHECK(!scope.End());
    CHECK(!scope.Begin());
    scope.Reset();
    CHECK(!scope.IsOpen());
    CHECK(!scope.End());
}

void RecordingOwnerHandoff() {
    RecordingOwner owner;
    CHECK(owner.Owner() == 0);
    CHECK(owner.Allows(10));
    CHECK(owner.Owner() == 10);
    CHECK(owner.Allows(10));
    CHECK(!owner.Allows(20));
    owner.Claim(20);
    CHECK(owner.Allows(20));
    CHECK(!owner.Allows(10));
    CHECK(owner.Owner() == 20);
}

// Stage A and stage B alternate as the only producer, handing over through an atomic position like the draw
// pipe's published/consumed counters. Every command must replay once, in order, and the owner must never
// refuse the thread whose turn it is.
void TwoProducersHandOffInTurns() {
    constexpr int Rounds = 2000;
    constexpr int PerTurn = 7;
    RecordingOwner owner;
    auto chunk = std::make_unique<RecordChunk>();
    std::atomic<int> turn{0};
    std::atomic<int> refused{0};
    int next = 0;
    const auto produce = [&](u32 tid, int who) {
        for (int round = 0; round < Rounds; ++round) {
            while (turn.load(std::memory_order_acquire) != who + 2 * round) {
                std::this_thread::yield();
            }
            owner.Claim(tid);
            for (int i = 0; i < PerTurn; ++i) {
                if (!owner.Allows(tid)) {
                    refused.fetch_add(1);
                }
                const int value = next++;
                if (!chunk->Push([&order = executed, value](vk::CommandBuffer) { order.push_back(value); },
                                 std::source_location::current())) {
                    chunk->Execute(vk::CommandBuffer{});
                    CHECK(chunk->Push([&order = executed, value](vk::CommandBuffer) { order.push_back(value); },
                                      std::source_location::current()));
                }
            }
            turn.fetch_add(1, std::memory_order_release);
        }
    };
    std::thread a{produce, 1u, 0};
    std::thread b{produce, 2u, 1};
    a.join();
    b.join();
    chunk->Execute(vk::CommandBuffer{});
    CHECK(refused.load() == 0);
    CHECK(executed.size() == size_t(2 * Rounds * PerTurn));
    bool ordered = true;
    for (size_t i = 0; ordered && i < executed.size(); ++i) {
        ordered = executed[i] == int(i);
    }
    CHECK(ordered);
    CHECK(owner.Changes() >= u64(2 * Rounds - 1));
}

// A thread that does not hold the turn is refused: this is what turns a cross-thread record into a failure.
void ForeignThreadIsRefusedWhileOwnerRecords() {
    RecordingOwner owner;
    owner.Claim(1);
    std::atomic<bool> refused{false};
    std::thread other{[&] { refused = !owner.Allows(2); }};
    other.join();
    CHECK(refused.load());
    CHECK(owner.Owner() == 1);
    CHECK(owner.Allows(1));
}

} // namespace

int main() {
    CommandsRunInRecordedOrder();
    CapturesLiveUntilTheCommandRuns();
    DataSharesTheChunkWithItsCommand();
    FullChunkRefusesAndKeepsItsCommands();
    RenderingScopePairing();
    RecordingOwnerHandoff();
    TwoProducersHandOffInTurns();
    ForeignThreadIsRefusedWhileOwnerRecords();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
