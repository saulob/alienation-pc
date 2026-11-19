// SPDX-License-Identifier: GPL-2.0-or-later
// aln: the Vulkan-independent rules of the persistent pipeline cache: blob integrity, cache
// identity, the warm-up mode and what a failed pipeline creation leads to.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace PipelineCachePolicy {

/// Largest blob the cache writes or reads; bounds every allocation made from a stored size.
constexpr std::size_t MaxBlobBytes = 64u * 1024 * 1024;

inline std::uint64_t Fnv1a(std::span<const std::uint8_t> bytes,
                           std::uint64_t hash = 0xcbf29ce484222325ull) {
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ull;
    }
    return hash;
}

namespace Detail {
template <typename T>
void Append(std::vector<std::uint8_t>& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}
template <typename T>
T At(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
} // namespace Detail

constexpr std::uint32_t SealMagic = 0x424e4c41; // "ALNB"
constexpr std::size_t SealSize = 16;

/// Appends magic, payload size and checksum: a truncated, zero-filled, partially overwritten or
/// unsealed (older) file never passes Unseal.
inline void Seal(std::vector<std::uint8_t>& bytes) {
    const auto size = std::uint32_t(bytes.size());
    const auto hash = Fnv1a(bytes);
    Detail::Append(bytes, SealMagic);
    Detail::Append(bytes, size);
    Detail::Append(bytes, hash);
}

/// Strips the seal; false (bytes untouched) when the blob is not intact.
inline bool Unseal(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < SealSize || bytes.size() - SealSize > MaxBlobBytes) {
        return false;
    }
    const std::size_t payload = bytes.size() - SealSize;
    const std::span<const std::uint8_t> all{bytes};
    if (Detail::At<std::uint32_t>(all, payload) != SealMagic ||
        Detail::At<std::uint32_t>(all, payload + 4) != payload ||
        Detail::At<std::uint64_t>(all, payload + 8) != Fnv1a(all.first(payload))) {
        return false;
    }
    bytes.resize(payload);
    return true;
}

/// One store per game build: 1.00 and 1.06 of a title never share a folder.
inline std::string CacheFolderName(std::string_view serial, std::string_view version) {
    std::string name{serial};
    if (!version.empty()) {
        name += '-';
        name += version;
    }
    for (auto& c : name) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.';
        if (!plain) {
            c = '_';
        }
    }
    return name;
}

inline std::uint64_t GameIdentity(std::string_view serial, std::string_view version) {
    const auto bytes = [](std::string_view text) {
        return std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    };
    const std::uint8_t separator = 0;
    return Fnv1a(bytes(version), Fnv1a({&separator, 1}, Fnv1a(bytes(serial))));
}

/// The GPU and driver a pipeline failed on.
struct GpuIdentity {
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_id{};
    std::uint32_t driver_version{};
    std::uint32_t api_version{};
    bool operator==(const GpuIdentity&) const = default;
};

/// What the shader and pipeline store is a function of besides the shader profile: the game
/// build and the three serialization layouts. The driver and the GPU are not in it: whatever in
/// them changes the translation is in the profile, and the rest does not touch SPIR-V.
struct StoreIdentity {
    std::uint64_t game{};
    std::uint32_t shader_binary_version{};
    std::uint32_t shader_meta_version{};
    std::uint32_t pipeline_key_version{};
    std::uint32_t profile_size{};
    bool operator==(const StoreIdentity&) const = default;
};

constexpr std::uint32_t StoreIdentityMagic = 0x49424c41; // "ALBI"
constexpr std::uint32_t WarmupPolicyMagic = 0x57424c41;  // "ALBW"
constexpr std::uint32_t RecordVersion = 1;

namespace Detail {
template <typename T>
std::vector<std::uint8_t> EncodeRecord(std::uint32_t magic, const T& value) {
    static_assert(std::has_unique_object_representations_v<T>);
    std::vector<std::uint8_t> bytes;
    Append(bytes, magic);
    Append(bytes, RecordVersion);
    Append(bytes, std::uint32_t(sizeof(T)));
    Append(bytes, value);
    return bytes;
}
template <typename T>
std::optional<T> DecodeRecord(std::uint32_t magic, std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 12 + sizeof(T) || At<std::uint32_t>(bytes, 0) != magic ||
        At<std::uint32_t>(bytes, 4) != RecordVersion || At<std::uint32_t>(bytes, 8) != sizeof(T)) {
        return std::nullopt;
    }
    return At<T>(bytes, 12);
}
} // namespace Detail

inline std::vector<std::uint8_t> EncodeStoreIdentity(const StoreIdentity& identity) {
    return Detail::EncodeRecord(StoreIdentityMagic, identity);
}
inline std::optional<StoreIdentity> DecodeStoreIdentity(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<StoreIdentity>(StoreIdentityMagic, bytes);
}

inline StoreIdentity CurrentStoreIdentity(std::string_view serial, std::string_view version,
                                          std::uint32_t shader_binary_version,
                                          std::uint32_t shader_meta_version,
                                          std::uint32_t pipeline_key_version,
                                          std::size_t profile_size) {
    return {
        .game = GameIdentity(serial, version),
        .shader_binary_version = shader_binary_version,
        .shader_meta_version = shader_meta_version,
        .pipeline_key_version = pipeline_key_version,
        .profile_size = std::uint32_t(profile_size),
    };
}

enum class StoreVerdict { Reuse, Missing, Corrupt, Incompatible };

inline StoreVerdict CheckStore(std::span<const std::uint8_t> stored, const StoreIdentity& current) {
    if (stored.empty()) {
        return StoreVerdict::Missing;
    }
    const auto identity = DecodeStoreIdentity(stored);
    if (!identity) {
        return StoreVerdict::Corrupt;
    }
    return *identity == current ? StoreVerdict::Reuse : StoreVerdict::Incompatible;
}

enum class StoreAction { Reuse, Create, Rebuild };

/// What to do with the store folder. A store that is not provably this build's and this GPU's
/// is cleared; an empty one is only filled.
constexpr StoreAction DecideStoreAction(StoreVerdict verdict, bool profile_present,
                                        bool profile_match, bool store_empty) {
    if (verdict == StoreVerdict::Reuse && profile_match) {
        return StoreAction::Reuse;
    }
    return verdict == StoreVerdict::Missing && !profile_present && store_empty
               ? StoreAction::Create
               : StoreAction::Rebuild;
}

inline std::string_view ToString(StoreVerdict verdict) {
    switch (verdict) {
    case StoreVerdict::Reuse:
        return "compatible";
    case StoreVerdict::Missing:
        return "no identity record";
    case StoreVerdict::Corrupt:
        return "corrupt identity record";
    case StoreVerdict::Incompatible:
        return "written by another game build or cache format";
    }
    return "unknown";
}

/// Driver-dependent: recorded after a pipeline failed while the warm-up built in parallel.
struct WarmupPolicy {
    GpuIdentity gpu{};
    std::uint32_t serial_builds{};
};

inline std::vector<std::uint8_t> EncodeWarmupPolicy(const WarmupPolicy& policy) {
    return Detail::EncodeRecord(WarmupPolicyMagic, policy);
}
inline std::optional<WarmupPolicy> DecodeWarmupPolicy(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<WarmupPolicy>(WarmupPolicyMagic, bytes);
}

enum class WarmupMode { Parallel, Serial };

/// Serial only on the exact GPU and driver that failed; a driver update gets parallel back.
inline WarmupMode ChooseWarmupMode(std::span<const std::uint8_t> stored, const GpuIdentity& gpu) {
    const auto policy = DecodeWarmupPolicy(stored);
    return policy && policy->serial_builds && policy->gpu == gpu ? WarmupMode::Serial
                                                                 : WarmupMode::Parallel;
}

/// Pipelines build in parallel until EnableSerial, then one at a time; the game's own builds go
/// ahead of the warm-up's.
class BuildGate {
public:
    enum class Kind { Warmup, Game };

private:
    struct Ticket {
        bool parallel;
        bool peers;
        std::uint64_t entries;
    };

public:
    class Slot {
    public:
        Slot(BuildGate& gate_, Kind kind) : gate{gate_}, ticket{gate_.Enter(kind)} {}
        ~Slot() {
            gate.Leave(ticket.parallel);
        }
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        bool Parallel() const {
            return ticket.parallel;
        }
        /// Another build ran at the same time as this one.
        bool Overlapped() const {
            return ticket.parallel && (ticket.peers || gate.entries.load() != ticket.entries);
        }

    private:
        BuildGate& gate;
        const Ticket ticket;
    };

    /// True for the call that switched to serial builds.
    bool EnableSerial() {
        std::scoped_lock lock{mutex};
        return !std::exchange(serial, true);
    }
    bool Serial() const {
        std::scoped_lock lock{mutex};
        return serial;
    }
    /// False when builds still run in parallel after `limit`.
    bool WaitForParallelBuilds(std::chrono::milliseconds limit) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, limit, [this] { return parallel == 0; });
    }

private:
    Ticket Enter(Kind kind) {
        std::unique_lock lock{mutex};
        if (!serial) {
            const bool peers = parallel > 0;
            ++parallel;
            return Ticket{true, peers, entries.fetch_add(1) + 1};
        }
        if (kind == Kind::Game) {
            ++games_waiting;
        }
        cv.wait(lock, [&] {
            return parallel == 0 && !serial_running && (kind == Kind::Game || games_waiting == 0);
        });
        if (kind == Kind::Game) {
            --games_waiting;
        }
        serial_running = true;
        return Ticket{false, false, entries.load()};
    }
    void Leave(bool was_parallel) {
        {
            std::scoped_lock lock{mutex};
            if (was_parallel) {
                --parallel;
            } else {
                serial_running = false;
            }
        }
        cv.notify_all();
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<std::uint64_t> entries{0};
    unsigned parallel{};
    unsigned games_waiting{};
    bool serial{};
    bool serial_running{};
};

/// The driver refused a pipeline even without the shared pipeline cache and binaries.
struct BuildFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class PipelineOrigin { Preload, Game };
enum class FailureAction { DropCachedEntry, RetryWithWarmUpPaused, Fatal };

/// After a creation already retried without the driver pipeline cache failed again. A preloaded
/// pipeline is only an optimisation; the game's own gets one more try with the warm-up paused.
constexpr FailureAction OnPipelineFailure(PipelineOrigin origin, bool game_retried) {
    if (origin == PipelineOrigin::Preload) {
        return FailureAction::DropCachedEntry;
    }
    return game_retried ? FailureAction::Fatal : FailureAction::RetryWithWarmUpPaused;
}

/// What a failed warm-up build leads to. The cached entry is dropped and left to the game; the
/// build mode turns serial only when the failed build was not alone.
struct WarmupFailure {
    bool drop_entry;
    bool record_serial;
};

constexpr WarmupFailure OnWarmupBuildFailure(bool build_overlapped) {
    return {true, build_overlapped};
}

/// Builds a pipeline the game asked for: one more try after `pause_warmup`, and `fatal` (which
/// does not return in the game) when that fails too. Builds hold a game slot of `gate`.
template <typename Make, typename Pause, typename Fatal>
auto BuildGamePipeline(BuildGate& gate, Make&& make, Pause&& pause_warmup, Fatal&& fatal) {
    for (bool retried = false;; retried = true) {
        try {
            const BuildGate::Slot slot{gate, BuildGate::Kind::Game};
            return make();
        } catch (const BuildFailed& error) {
            if (OnPipelineFailure(PipelineOrigin::Game, retried) == FailureAction::Fatal) {
                fatal(error);
                throw;
            }
            pause_warmup(error);
        }
    }
}

enum class PipelineStartup { Background, OnDemand };

/// ALN_PIPELINE_ON_DEMAND: any non-empty value but "0" skips the background warm-up.
inline bool PipelineOnDemandRequested(const char* env_value) {
    return env_value && *env_value && std::strcmp(env_value, "0") != 0;
}

/// `start_background` runs only when on-demand was not requested and returns whether it started.
template <typename StartBackground>
PipelineStartup ChoosePipelineStartup(const char* env_value, StartBackground&& start_background) {
    if (PipelineOnDemandRequested(env_value) || !start_background()) {
        return PipelineStartup::OnDemand;
    }
    return PipelineStartup::Background;
}

} // namespace PipelineCachePolicy
