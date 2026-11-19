// Focused tests of the persistent pipeline cache rules: blob seal, corrupt-data recovery, atomic
// writes, the IO queue, cache identity, the warm-up mode and the failure policy. No Vulkan.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/serdes.h"
#include "video_core/cache_files.h"
#include "video_core/cache_request_queue.h"
#include "video_core/pipeline_cache_policy.h"

namespace {

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

using namespace PipelineCachePolicy;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path{fs::temp_directory_path() / name} {
        std::error_code error;
        fs::remove_all(path, error);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

Bytes Pattern(std::size_t size, std::uint8_t seed = 1) {
    Bytes bytes(size);
    std::uint32_t state = 0x9e3779b9u * seed;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = std::uint8_t(state >> 24);
    }
    return bytes;
}

Bytes Sealed(Bytes bytes) {
    Seal(bytes);
    return bytes;
}

Bytes ReadAll(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

unsigned CountFiles(const fs::path& directory) {
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        count += entry.is_regular_file();
    }
    return count;
}

unsigned CountTemps(const fs::path& directory) {
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        count += PipelineCacheFiles::IsTempFile(entry.path());
    }
    return count;
}

// What the store does with a blob it reads: unseal, and the caller rebuilds on failure.
std::optional<Bytes> LoadBlob(const fs::path& file) {
    auto bytes = ReadAll(file);
    if (!Unseal(bytes)) {
        return std::nullopt;
    }
    return bytes;
}

// ---------------------------------------------------------------------------------------------
// Blob integrity

void TestSealRoundTrips() {
    for (const std::size_t size : {0, 1, 15, 16, 17, 255, 4096, 70000}) {
        const auto payload = Pattern(size, std::uint8_t(size));
        auto blob = Sealed(payload);
        CHECK(blob.size() == payload.size() + SealSize);
        CHECK(Unseal(blob));
        CHECK(blob == payload);
    }
}

void TestTruncatedBlobsAreRejected() {
    const auto blob = Sealed(Pattern(300));
    for (std::size_t cut = 0; cut < blob.size(); ++cut) {
        Bytes shorter(blob.begin(), blob.begin() + std::ptrdiff_t(cut));
        const auto before = shorter;
        CHECK(!Unseal(shorter));
        CHECK(shorter == before);
    }
    Bytes without_first(blob.begin() + 1, blob.end());
    CHECK(!Unseal(without_first));
}

void TestEmptyAndZeroFilledBlobsAreRejected() {
    Bytes empty;
    CHECK(!Unseal(empty));
    for (std::size_t size = 0; size <= 96; ++size) {
        Bytes zeros(size, 0);
        CHECK(!Unseal(zeros));
    }
    Bytes zeros(4096, 0);
    CHECK(!Unseal(zeros));
}

void TestEverySingleBitFlipIsRejected() {
    const auto blob = Sealed(Pattern(40));
    for (std::size_t bit = 0; bit < blob.size() * 8; ++bit) {
        auto flipped = blob;
        flipped[bit / 8] ^= std::uint8_t(1u << (bit % 8));
        CHECK(!Unseal(flipped));
    }
}

void TestTrailingGarbageIsRejected() {
    auto blob = Sealed(Pattern(100));
    for (const std::size_t extra : {1, 2, 15, 16, 17, 100}) {
        auto longer = blob;
        longer.insert(longer.end(), extra, std::uint8_t(0xa5));
        CHECK(!Unseal(longer));
    }
    // Two sealed blobs back to back are not one blob either.
    auto doubled = blob;
    doubled.insert(doubled.end(), blob.begin(), blob.end());
    CHECK(!Unseal(doubled));
}

void TestOldUnsealedBlobsRebuildInsteadOfAsserting() {
    // What the previous format stored: the payload alone, whatever it was.
    const Bytes old_profile = Pattern(200, 3);
    const Bytes old_identity(24, 0x11);
    for (auto old : {old_profile, old_identity, Bytes(7, 1)}) {
        CHECK(!Unseal(old));
    }
    // The store sees an unreadable identity and profile in a folder that has files: rebuild.
    CHECK(DecideStoreAction(StoreVerdict::Missing, true, false, false) == StoreAction::Rebuild);
    CHECK(DecideStoreAction(StoreVerdict::Missing, false, false, false) == StoreAction::Rebuild);
    CHECK(DecideStoreAction(StoreVerdict::Corrupt, true, true, false) == StoreAction::Rebuild);

    // An old entry cut anywhere is corrupt data, never a process-ending assertion.
    Serialization::Archive writer;
    Serialization::Writer out{writer};
    out.Write(std::uint32_t{5});
    out.Write(std::vector<std::uint32_t>{1, 2, 3, 4});
    out.Write(std::string{"entry"});
    const auto entry = writer.TakeOff();
    for (std::size_t cut = 0; cut < entry.size(); ++cut) {
        Serialization::Archive ar{Bytes(entry.begin(), entry.begin() + std::ptrdiff_t(cut))};
        Serialization::Reader in{ar};
        bool corrupt = false;
        try {
            std::uint32_t version{};
            std::vector<std::uint32_t> list;
            std::string text;
            in.Read(version);
            in.Read(list);
            in.Read(text);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
    }
    Serialization::Archive whole{Bytes(entry)};
    Serialization::Reader in{whole};
    std::uint32_t version{};
    std::vector<std::uint32_t> list;
    std::string text;
    in.Read(version);
    in.Read(list);
    in.Read(text);
    CHECK(version == 5 && list.size() == 4 && list[3] == 4 && text == "entry" && whole.IsEoS());
}

// ---------------------------------------------------------------------------------------------
// Corrupt dynamic lengths

Bytes WithCount(std::uint64_t count, std::size_t payload_bytes) {
    Bytes bytes(sizeof(count) + payload_bytes, 0x7f);
    std::memcpy(bytes.data(), &count, sizeof(count));
    return bytes;
}

template <typename Container>
bool ReadThrowsCorrupt(Bytes bytes, Container& out) {
    Serialization::Archive ar{std::move(bytes)};
    Serialization::Reader in{ar};
    try {
        in.Read(out);
    } catch (const Serialization::CorruptData&) {
        return true;
    }
    return false;
}

void TestCorruptVectorCountsAreRejectedBeforeAllocating() {
    for (const std::uint64_t count : {std::uint64_t{0xffffffffffffffff}, std::uint64_t{1} << 62,
                                      std::uint64_t{1} << 40, std::uint64_t{0x7fffffff}}) {
        std::vector<std::uint32_t> list;
        CHECK(ReadThrowsCorrupt(WithCount(count, 16), list));
        CHECK(list.empty() && list.capacity() == 0);
        std::vector<std::vector<std::uint8_t>> nested;
        CHECK(ReadThrowsCorrupt(WithCount(count, 16), nested));
        CHECK(nested.empty() && nested.capacity() == 0);
    }
    // One more element than the bytes hold; and exactly what they hold.
    std::vector<std::uint32_t> list;
    CHECK(ReadThrowsCorrupt(WithCount(5, 16), list));
    CHECK(list.capacity() == 0);
    std::vector<std::uint32_t> exact;
    CHECK(!ReadThrowsCorrupt(WithCount(4, 16), exact));
    CHECK(exact.size() == 4);
    std::vector<std::uint8_t> bytes_list;
    CHECK(ReadThrowsCorrupt(WithCount(17, 16), bytes_list));
    CHECK(bytes_list.empty());
    // A count that wraps a 32-bit or 64-bit sum with the offset.
    std::vector<std::uint16_t> wrapped;
    CHECK(ReadThrowsCorrupt(WithCount(0x8000000000000001ull, 32), wrapped));
    CHECK(wrapped.empty());
}

void TestCorruptStringLengthsAreRejectedBeforeAllocating() {
    for (const std::uint64_t length : {std::uint64_t{0xffffffffffffffff}, std::uint64_t{1} << 40,
                                       std::uint64_t{0xfffffffffffffff8}, std::uint64_t{17}}) {
        std::string text;
        CHECK(ReadThrowsCorrupt(WithCount(length, 16), text));
        CHECK(text.empty() && text.capacity() < 1024);
    }
    std::string exact;
    CHECK(!ReadThrowsCorrupt(WithCount(16, 16), exact));
    CHECK(exact.size() == 16);
    std::vector<std::string> names;
    CHECK(ReadThrowsCorrupt(WithCount(1000000, 64), names));
    CHECK(names.capacity() == 0);
}

void TestReadsNeverPassTheEnd() {
    const Bytes ten(10, 1);
    {
        Serialization::Archive ar{Bytes(ten)};
        Serialization::Reader in{ar};
        std::uint32_t value{};
        in.Read(value);
        in.Read(value);
        bool corrupt = false;
        try {
            in.Read(value);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
    }
    for (const std::size_t size : {std::size_t(11), ~std::size_t(0), ~std::size_t(0) - 3}) {
        Serialization::Archive ar{Bytes(ten)};
        Serialization::Reader in{ar};
        std::uint32_t value{};
        in.Read(value);
        bool corrupt = false;
        try {
            in.Borrow(size);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
        corrupt = false;
        try {
            char sink[4];
            in.Read(sink, size);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
        corrupt = false;
        try {
            ar.Advance(size);
        } catch (const Serialization::CorruptData&) {
            corrupt = true;
        }
        CHECK(corrupt);
    }
    Serialization::Archive ar{Bytes(ten)};
    Serialization::Reader in{ar};
    CHECK(in.Borrow(10) != nullptr && ar.IsEoS());
    CHECK(in.Borrow(0) != nullptr);
}

// ---------------------------------------------------------------------------------------------
// Atomic writes

bool WriteHalfThenFail(const fs::path& file, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size() / 2));
    return false;
}

bool FailToReplace(const fs::path&, const fs::path&) {
    return false;
}

void TestFailedWritesKeepThePreviousFile() {
    TempDir dir{"aln-pipeline-cache-atomic-fail"};
    const auto file = dir.path / "g_0x1.key";
    const auto previous = Sealed(Pattern(500, 1));
    CHECK(PipelineCacheFiles::WriteFileAtomically(file, previous));

    const auto next = Sealed(Pattern(900, 2));
    CHECK(!PipelineCacheFiles::WriteFileAtomically(file, next, {.write = &WriteHalfThenFail}));
    CHECK(ReadAll(file) == previous);
    CHECK(LoadBlob(file).has_value());
    CHECK(!PipelineCacheFiles::WriteFileAtomically(file, next, {.replace = &FailToReplace}));
    CHECK(ReadAll(file) == previous);
    CHECK(CountTemps(dir.path) == 0 && CountFiles(dir.path) == 1);

    // No previous file: a failed write leaves nothing a reader could load.
    const auto fresh = dir.path / "g_0x2.key";
    CHECK(!PipelineCacheFiles::WriteFileAtomically(fresh, next, {.write = &WriteHalfThenFail}));
    CHECK(!fs::exists(fresh) && CountTemps(dir.path) == 0);

    // A directory that does not exist is a failed write too, not a crash.
    CHECK(!PipelineCacheFiles::WriteFileAtomically(dir.path / "missing" / "x.key", next));
}

void TestSuccessfulWritesReplaceExactly() {
    TempDir dir{"aln-pipeline-cache-atomic-ok"};
    const auto file = dir.path / "0x1.meta";
    for (const std::size_t size : {600, 40, 0, 5000, 5000}) {
        const auto bytes = Pattern(size, std::uint8_t(size));
        CHECK(PipelineCacheFiles::WriteFileAtomically(file, bytes));
        CHECK(ReadAll(file) == bytes);
        CHECK(CountFiles(dir.path) == 1);
    }
    const auto sealed = Sealed(Pattern(321));
    CHECK(PipelineCacheFiles::WriteFileAtomically(file, sealed));
    const auto loaded = LoadBlob(file);
    CHECK(loaded && *loaded == Pattern(321));
}

void TestReplacingAFileThatIsOpenForReading() {
    TempDir dir{"aln-pipeline-cache-atomic-open"};
    const auto file = dir.path / "profile.bin";
    CHECK(PipelineCacheFiles::WriteFileAtomically(file, Pattern(100, 1)));
    const auto next = Pattern(200, 2);
    // A reader that lets go within the retry window does not lose the write.
    std::thread reader([&] {
        std::ifstream hold(file, std::ios::binary);
        std::this_thread::sleep_for(8ms);
    });
    std::this_thread::sleep_for(2ms);
    const bool written = PipelineCacheFiles::WriteFileAtomically(file, next);
    reader.join();
    const auto content = ReadAll(file);
    CHECK(written ? content == next : content == Pattern(100, 1));
    CHECK(CountTemps(dir.path) == 0);
}

void TestTemporaryFilesAreCleanedAtOpen() {
    TempDir dir{"aln-pipeline-cache-orphans"};
    const auto keep = dir.path / "g_0x1.key";
    CHECK(PipelineCacheFiles::WriteFileAtomically(keep, Sealed(Pattern(64))));
    const auto old_temp = dir.path / "g_0x2.key.4242.7.tmp";
    const auto older_style = dir.path / "g_0x3.key.tmp";
    const auto fresh_temp = dir.path / "g_0x4.key.4243.1.tmp";
    for (const auto& temp : {old_temp, older_style, fresh_temp}) {
        std::ofstream(temp, std::ios::binary) << "partial";
    }
    const auto long_ago = fs::file_time_type::clock::now() - 2h;
    fs::last_write_time(old_temp, long_ago);
    fs::last_write_time(older_style, long_ago);

    // A write another process is making right now is not an orphan.
    CHECK(PipelineCacheFiles::RemoveLeftoverTemps(dir.path) == 2);
    CHECK(!fs::exists(old_temp) && !fs::exists(older_style));
    CHECK(fs::exists(fresh_temp) && fs::exists(keep));
    CHECK(PipelineCacheFiles::RemoveLeftoverTemps(dir.path, 0s) == 1);
    CHECK(!fs::exists(fresh_temp) && fs::exists(keep) && CountFiles(dir.path) == 1);
    CHECK(PipelineCacheFiles::RemoveLeftoverTemps(dir.path / "absent", 0s) == 0);
}

void TestConcurrentWritersDoNotShareTemporaryFiles() {
    std::set<std::string> names;
    std::mutex names_mutex;
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 500; ++i) {
                    const auto temp = PipelineCacheFiles::TempPathFor("dir/g_0x1.key").string();
                    std::scoped_lock lock{names_mutex};
                    names.insert(temp);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    CHECK(names.size() == 2000);
    CHECK(std::ranges::all_of(names, [](const std::string& name) {
        return name.starts_with("dir/g_0x1.key.") && name.ends_with(".tmp");
    }));

    TempDir dir{"aln-pipeline-cache-concurrent"};
    const auto file = dir.path / "g_0x1.key";
    std::atomic<int> failed{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 8; ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; i < 40; ++i) {
                const Bytes payload(2000 + std::size_t(t) * 37, std::uint8_t(t + 1));
                failed += !PipelineCacheFiles::WriteFileAtomically(file, payload);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    const auto content = ReadAll(file);
    bool one_writer = !content.empty();
    for (const auto byte : content) {
        one_writer = one_writer && byte == content.front();
    }
    CHECK(one_writer && content.size() == 2000 + std::size_t(content.front() - 1) * 37);
    CHECK(CountTemps(dir.path) == 0 && CountFiles(dir.path) == 1);
    CHECK(failed.load() < 8 * 40);
}

void TestStoreFilesAreSelectedAndFoldersStay() {
    CHECK(PipelineCacheFiles::IsStoreFile("a/g_0x1.key"));
    CHECK(PipelineCacheFiles::IsStoreFile("a/0x1.meta"));
    CHECK(PipelineCacheFiles::IsStoreFile("a/0x1_0.spv"));
    CHECK(PipelineCacheFiles::IsStoreFile("a/profile.bin"));
    CHECK(PipelineCacheFiles::IsStoreFile("a/x.key.12.3.tmp"));
    CHECK(!PipelineCacheFiles::IsStoreFile("a/notes.txt") && !PipelineCacheFiles::IsStoreFile("a/key"));

    // The driver pipeline binaries are not the store: rebuilding the store never touches them.
    TempDir dir{"aln-pipeline-cache-folders"};
    fs::create_directories(dir.path / "pipeline-binaries");
    const auto binary = dir.path / "pipeline-binaries" / "ab12.bin";
    const auto binary_bytes = Pattern(300, 9);
    CHECK(PipelineCacheFiles::WriteFileAtomically(binary, binary_bytes));
    for (const char* name : {"g_0x1.key", "0x1.meta", "0x1_0.spv", "profile.bin", "readme.txt"}) {
        CHECK(PipelineCacheFiles::WriteFileAtomically(dir.path / name, Sealed(Pattern(20))));
    }
    CHECK(PipelineCacheFiles::RemoveFiles(dir.path, PipelineCacheFiles::IsStoreFile) == 4);
    CHECK(fs::exists(dir.path / "readme.txt") && ReadAll(binary) == binary_bytes);
    CHECK(PipelineCacheFiles::RemoveFiles(dir.path, [](const auto&) { return true; }) == 1);
    CHECK(CountFiles(dir.path) == 0 && ReadAll(binary) == binary_bytes);
}

// ---------------------------------------------------------------------------------------------
// The IO queue

void TestQueueCountStaysCorrectUnderConcurrentProducers() {
    constexpr int Producers = 8;
    constexpr int PerProducer = 500;
    auto queue = std::make_shared<Storage::RequestQueue>();
    std::atomic<int> ran{0};
    std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
    std::vector<std::thread> producers;
    std::atomic<bool> consistent{true};
    for (int p = 0; p < Producers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < PerProducer; ++i) {
                if (i % 50 == 7) {
                    queue->Submit([] { throw 1; });
                } else {
                    queue->Submit([&] { ++ran; });
                }
                if (queue->Completed() + queue->Failed() > queue->Submitted()) {
                    consistent = false;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (queue->Completed() + queue->Failed() < std::uint64_t(Producers) * PerProducer &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    constexpr int Throwing = Producers * (PerProducer / 50);
    CHECK(consistent.load());
    CHECK(queue->Submitted() == std::uint64_t(Producers) * PerProducer);
    CHECK(queue->Pending() == 0 && queue->Dropped() == 0);
    CHECK(queue->Failed() == Throwing);
    CHECK(queue->Completed() == std::uint64_t(Producers) * PerProducer - Throwing);
    CHECK(ran.load() == Producers * PerProducer - Throwing);
}

void TestWorkerKeepsTheQueueAliveUntilItEnds() {
    std::atomic<bool> destroyed{false}, started{false}, release{false};
    {
        std::shared_ptr<Storage::RequestQueue> queue{new Storage::RequestQueue,
                                                     [&](Storage::RequestQueue* freed) {
                                                         destroyed = true;
                                                         delete freed;
                                                     }};
        queue->Submit([&] {
            started = true;
            while (!release) {
                std::this_thread::yield();
            }
        });
        queue->Submit([] {});
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        while (!started) {
            std::this_thread::yield();
        }
        queue.reset(); // the owner goes away while the worker is in a task
        std::this_thread::sleep_for(20ms);
        CHECK(!destroyed.load());
        worker.request_stop();
        release = true;
    } // joins the worker, which then lets go of the queue
    CHECK(destroyed.load());
}

void TestCloseFinishesWhatFitsAndDropsTheRest() {
    {
        auto queue = std::make_shared<Storage::RequestQueue>();
        std::atomic<int> ran{0};
        for (int i = 0; i < 4; ++i) {
            queue->Submit([&] {
                std::this_thread::sleep_for(10ms);
                ++ran;
            });
        }
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        queue->Close(10s);
        worker.join();
        CHECK(ran.load() == 4 && queue->Dropped() == 0 && queue->Pending() == 0);
        CHECK(!queue->Submit([] {}));
    }
    {
        auto queue = std::make_shared<Storage::RequestQueue>();
        std::atomic<int> ran{0};
        for (int i = 0; i < 10; ++i) {
            queue->Submit([&] {
                std::this_thread::sleep_for(40ms);
                ++ran;
            });
        }
        const auto started = std::chrono::steady_clock::now();
        std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
        queue->Close(60ms);
        worker.join();
        const auto took = std::chrono::steady_clock::now() - started;
        CHECK(ran.load() >= 1 && ran.load() < 10);
        CHECK(queue->Completed() + queue->Dropped() == 10 && queue->Dropped() > 0);
        CHECK(took < 2s);
    }
}

void TestStopAbandonsTheQueueWithoutHanging() {
    auto queue = std::make_shared<Storage::RequestQueue>();
    std::atomic<bool> started{false}, release{false};
    queue->Submit([&] {
        started = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 3; ++i) {
        queue->Submit([] {});
    }
    std::jthread worker{[queue](std::stop_token stoken) { queue->Run(stoken); }};
    while (!started) {
        std::this_thread::yield();
    }
    worker.request_stop();
    release = true;
    worker.join();
    CHECK(queue->Completed() == 1 && queue->Dropped() == 3 && queue->Pending() == 0);

    // An idle worker wakes for a stop request.
    auto idle = std::make_shared<Storage::RequestQueue>();
    std::jthread idle_worker{[idle](std::stop_token stoken) { idle->Run(stoken); }};
    std::this_thread::sleep_for(10ms);
    idle_worker.request_stop();
    idle_worker.join();
    CHECK(idle->Dropped() == 0);
}

// ---------------------------------------------------------------------------------------------
// Cache identity

constexpr std::uint32_t BinaryVersion = 8, MetaVersion = 7, KeyVersion = 5;
constexpr std::size_t ProfileSize = 80;

StoreIdentity Identity(std::string_view serial, std::string_view version) {
    return CurrentStoreIdentity(serial, version, BinaryVersion, MetaVersion, KeyVersion,
                                ProfileSize);
}

void TestCompatibleIdentityIsReused() {
    const auto identity = Identity("CUSA00062", "1.06");
    const auto stored = EncodeStoreIdentity(identity);
    CHECK(CheckStore(stored, identity) == StoreVerdict::Reuse);
    CHECK(DecideStoreAction(StoreVerdict::Reuse, true, true, false) == StoreAction::Reuse);
    // What the store does: the record is sealed on disk and unsealed on load.
    auto on_disk = Sealed(stored);
    CHECK(Unseal(on_disk) && CheckStore(on_disk, identity) == StoreVerdict::Reuse);
    CHECK(DecodeStoreIdentity(stored) == identity);
}

void TestEachBuildAndFormatHasItsOwnIdentity() {
    const StoreIdentity builds[] = {Identity("CUSA00062", "1.00"), Identity("CUSA00062", "1.06"),
                                    Identity("CUSA04430", "1.00"), Identity("CUSA00062", "")};
    for (const auto& stored : builds) {
        for (const auto& current : builds) {
            const auto verdict = CheckStore(EncodeStoreIdentity(stored), current);
            CHECK((verdict == StoreVerdict::Reuse) == (&stored == &current));
        }
    }
    const auto serial_only = GameIdentity("CUSA00062", "");
    CHECK(serial_only != GameIdentity("CUSA00062", "1.00"));
    CHECK(GameIdentity("AB", "C") != GameIdentity("A", "BC"));

    const std::string folders[] = {CacheFolderName("CUSA00062", "1.00"),
                                   CacheFolderName("CUSA00062", "1.06"),
                                   CacheFolderName("CUSA04430", "1.00"),
                                   CacheFolderName("CUSA00062", "")};
    CHECK(std::set<std::string>(std::begin(folders), std::end(folders)).size() == 4);
    CHECK(folders[1] == "CUSA00062-1.06" && folders[3] == "CUSA00062");
    CHECK(CacheFolderName("../x", "a/b\\c:d") == ".._x-a_b_c_d");

    const auto stored = EncodeStoreIdentity(Identity("CUSA00062", "1.06"));
    const auto current = Identity("CUSA00062", "1.06");
    for (const auto& changed :
         {CurrentStoreIdentity("CUSA00062", "1.06", BinaryVersion + 1, MetaVersion, KeyVersion,
                               ProfileSize),
          CurrentStoreIdentity("CUSA00062", "1.06", BinaryVersion, MetaVersion + 1, KeyVersion,
                               ProfileSize),
          CurrentStoreIdentity("CUSA00062", "1.06", BinaryVersion, MetaVersion, KeyVersion + 1,
                               ProfileSize),
          CurrentStoreIdentity("CUSA00062", "1.06", BinaryVersion, MetaVersion, KeyVersion,
                               ProfileSize + 4)}) {
        CHECK(CheckStore(stored, changed) == StoreVerdict::Incompatible);
        CHECK(DecideStoreAction(StoreVerdict::Incompatible, true, true, false) ==
              StoreAction::Rebuild);
    }
    CHECK(CheckStore(stored, current) == StoreVerdict::Reuse);
}

void TestCorruptIdentityRebuilds() {
    const auto current = Identity("CUSA00062", "1.06");
    const auto stored = EncodeStoreIdentity(current);
    CHECK(CheckStore({}, current) == StoreVerdict::Missing);
    CHECK(CheckStore(Bytes(stored.size(), 0), current) == StoreVerdict::Corrupt);
    for (std::size_t cut = 1; cut < stored.size(); ++cut) {
        CHECK(CheckStore(std::span{stored}.first(cut), current) == StoreVerdict::Corrupt);
    }
    auto longer = stored;
    longer.push_back(0);
    CHECK(CheckStore(longer, current) == StoreVerdict::Corrupt);
    for (std::size_t byte = 0; byte < 12; ++byte) {
        auto changed = stored;
        changed[byte] ^= 1;
        CHECK(CheckStore(changed, current) == StoreVerdict::Corrupt);
    }
    CHECK(DecideStoreAction(StoreVerdict::Corrupt, true, true, false) == StoreAction::Rebuild);

    // A record whose seal broke loads as nothing, beside a profile that is there.
    auto sealed = Sealed(stored);
    sealed[sealed.size() / 2] ^= 0x10;
    CHECK(!Unseal(sealed));
    CHECK(DecideStoreAction(StoreVerdict::Missing, true, true, false) == StoreAction::Rebuild);
}

void TestDriverChangeKeepsTheStore() {
    // The identity has no driver in it: two sessions on two drivers of one GPU compute the same.
    const auto session_a = Identity("CUSA00062", "1.00");
    const auto session_b = Identity("CUSA00062", "1.00");
    CHECK(session_a == session_b);
    CHECK(CheckStore(EncodeStoreIdentity(session_a), session_b) == StoreVerdict::Reuse);
    // What the driver does change is in the shader profile, which decides on its own.
    CHECK(DecideStoreAction(StoreVerdict::Reuse, true, true, false) == StoreAction::Reuse);
    CHECK(DecideStoreAction(StoreVerdict::Reuse, true, false, false) == StoreAction::Rebuild);
    // A new store is filled, not cleared; a folder with files and no identity is rebuilt.
    CHECK(DecideStoreAction(StoreVerdict::Missing, false, false, true) == StoreAction::Create);
    CHECK(DecideStoreAction(StoreVerdict::Missing, false, false, false) == StoreAction::Rebuild);
    CHECK(DecideStoreAction(StoreVerdict::Missing, true, true, true) == StoreAction::Rebuild);
}

// ---------------------------------------------------------------------------------------------
// Warm-up mode and failures

const GpuIdentity Gpu{0x10de, 0x2204, 4, 0x8c9a2000, 0x403150};

void TestParallelFailureRecordsSerialOnlyForThatGpuAndDriver() {
    const auto policy = EncodeWarmupPolicy({Gpu, 1});
    CHECK(ChooseWarmupMode(policy, Gpu) == WarmupMode::Serial);
    // The same GPU on an updated driver, another GPU, another vendor: parallel.
    auto updated = Gpu;
    updated.driver_version += 1;
    CHECK(ChooseWarmupMode(policy, updated) == WarmupMode::Parallel);
    auto other = Gpu;
    other.device_id = 0x2484;
    CHECK(ChooseWarmupMode(policy, other) == WarmupMode::Parallel);
    other = Gpu;
    other.vendor_id = 0x1002;
    other.driver_id = 1;
    CHECK(ChooseWarmupMode(policy, other) == WarmupMode::Parallel);
    other = Gpu;
    other.api_version += 1;
    CHECK(ChooseWarmupMode(policy, other) == WarmupMode::Parallel);
    other = Gpu;
    other.driver_id += 1;
    CHECK(ChooseWarmupMode(policy, other) == WarmupMode::Parallel);
    // Nothing recorded, a cleared record, a broken record: parallel.
    CHECK(ChooseWarmupMode({}, Gpu) == WarmupMode::Parallel);
    CHECK(ChooseWarmupMode(EncodeWarmupPolicy({Gpu, 0}), Gpu) == WarmupMode::Parallel);
    for (std::size_t cut = 0; cut < policy.size(); ++cut) {
        CHECK(ChooseWarmupMode(std::span{policy}.first(cut), Gpu) == WarmupMode::Parallel);
    }
    auto flipped = policy;
    flipped[2] ^= 1;
    CHECK(ChooseWarmupMode(flipped, Gpu) == WarmupMode::Parallel);
    CHECK(DecodeWarmupPolicy(policy) && DecodeWarmupPolicy(policy)->gpu == Gpu);
}

void TestSerialModeFollowsTheFailureAcrossRuns() {
    // Run 1: two warm-up builds overlap and one fails.
    Bytes stored;
    {
        BuildGate gate;
        BuildGate::Slot failing{gate, BuildGate::Kind::Warmup};
        BuildGate::Slot other{gate, BuildGate::Kind::Warmup};
        CHECK(failing.Parallel() && other.Parallel());
        const auto failure = OnWarmupBuildFailure(failing.Overlapped());
        CHECK(failure.drop_entry && failure.record_serial);
        CHECK(gate.EnableSerial());
        stored = EncodeWarmupPolicy({Gpu, 1});
    }
    // Run 2: same GPU and driver, serial from the start.
    {
        BuildGate gate;
        if (ChooseWarmupMode(stored, Gpu) == WarmupMode::Serial) {
            gate.EnableSerial();
        }
        CHECK(gate.Serial());
        BuildGate::Slot slot{gate, BuildGate::Kind::Warmup};
        CHECK(!slot.Parallel());
    }
    // Run 3: the driver was updated.
    {
        BuildGate gate;
        auto updated = Gpu;
        updated.driver_version += 1;
        if (ChooseWarmupMode(stored, updated) == WarmupMode::Serial) {
            gate.EnableSerial();
        }
        CHECK(!gate.Serial());
        if (!gate.Serial()) {
            BuildGate::Slot first{gate, BuildGate::Kind::Warmup};
            BuildGate::Slot second{gate, BuildGate::Kind::Warmup};
            CHECK(first.Parallel() && second.Parallel());
        }
    }
    // A build that ran alone says nothing about parallel builds.
    BuildGate gate;
    {
        BuildGate::Slot alone{gate, BuildGate::Kind::Warmup};
        CHECK(!alone.Overlapped() && !OnWarmupBuildFailure(alone.Overlapped()).record_serial);
    }
    BuildGate::Slot first{gate, BuildGate::Kind::Warmup};
    CHECK(!first.Overlapped());
    {
        BuildGate::Slot second{gate, BuildGate::Kind::Game};
        CHECK(first.Overlapped() && second.Overlapped());
    }
    CHECK(first.Overlapped());
    CHECK(gate.EnableSerial() && !gate.EnableSerial());
}

void TestWarmupFailureIsNeverFatal() {
    for (const bool retried : {false, true}) {
        CHECK(OnPipelineFailure(PipelineOrigin::Preload, retried) == FailureAction::DropCachedEntry);
    }
    CHECK(OnWarmupBuildFailure(false).drop_entry && OnWarmupBuildFailure(true).drop_entry);
    CHECK(!OnWarmupBuildFailure(false).record_serial && OnWarmupBuildFailure(true).record_serial);
}

struct Fatal {};

void TestGamePipelineGetsOneCleanRetry() {
    {
        BuildGate gate;
        int made = 0, paused = 0, fatal = 0;
        const auto value = BuildGamePipeline(
            gate,
            [&] {
                ++made;
                return 7;
            },
            [&](const BuildFailed&) { ++paused; }, [&](const BuildFailed&) { ++fatal; });
        CHECK(value == 7 && made == 1 && paused == 0 && fatal == 0);
    }
    {
        BuildGate gate;
        int made = 0, paused = 0, fatal = 0;
        const auto value = BuildGamePipeline(
            gate,
            [&] {
                if (++made == 1) {
                    throw BuildFailed{"refused"};
                }
                return std::string{"built"};
            },
            [&](const BuildFailed& error) {
                ++paused;
                CHECK(std::string{error.what()} == "refused");
                CHECK(gate.WaitForParallelBuilds(0ms)); // the failed build's slot is released
            },
            [&](const BuildFailed&) { ++fatal; });
        CHECK(value == "built" && made == 2 && paused == 1 && fatal == 0);
    }
    {
        BuildGate gate;
        int made = 0, paused = 0, fatal = 0;
        bool reached_fatal = false;
        try {
            BuildGamePipeline(
                gate,
                [&]() -> int {
                    ++made;
                    throw BuildFailed{"refused"};
                },
                [&](const BuildFailed&) { ++paused; },
                [&](const BuildFailed&) {
                    ++fatal;
                    throw Fatal{};
                });
        } catch (const Fatal&) {
            reached_fatal = true;
        }
        CHECK(reached_fatal && made == 2 && paused == 1 && fatal == 1);
    }
    {
        BuildGate gate;
        int made = 0;
        bool other = false;
        try {
            BuildGamePipeline(
                gate,
                [&]() -> int {
                    ++made;
                    throw std::logic_error{"not a driver refusal"};
                },
                [](const BuildFailed&) {}, [](const BuildFailed&) {});
        } catch (const std::logic_error&) {
            other = true;
        }
        CHECK(other && made == 1);
    }
    {
        BuildGate gate;
        BuildGamePipeline(
            gate, [&] { return gate.WaitForParallelBuilds(0ms); }, [](const BuildFailed&) {},
            [](const BuildFailed&) {});
        CHECK(gate.WaitForParallelBuilds(0ms));
        BuildGamePipeline(
            gate,
            [&] {
                CHECK(!gate.WaitForParallelBuilds(0ms)); // the build holds its slot
                return 0;
            },
            [](const BuildFailed&) {}, [](const BuildFailed&) {});
    }
}

void TestPipelineFailureTable() {
    CHECK(OnPipelineFailure(PipelineOrigin::Game, false) == FailureAction::RetryWithWarmUpPaused);
    CHECK(OnPipelineFailure(PipelineOrigin::Game, true) == FailureAction::Fatal);
}

void TestSerialBuildsRunOneAtATimeAndGameBuildsGoFirst() {
    BuildGate gate;
    CHECK(gate.EnableSerial());
    std::atomic<int> running{0}, peak{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                BuildGate::Slot slot{gate, t % 2 ? BuildGate::Kind::Game : BuildGate::Kind::Warmup};
                const int now = ++running;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::yield();
                --running;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(peak.load() == 1);

    std::mutex order_mutex;
    std::vector<char> order;
    auto holder = std::make_unique<BuildGate::Slot>(gate, BuildGate::Kind::Warmup);
    std::thread warmup([&] {
        BuildGate::Slot slot{gate, BuildGate::Kind::Warmup};
        std::scoped_lock lock{order_mutex};
        order.push_back('w');
    });
    std::this_thread::sleep_for(60ms);
    std::thread game([&] {
        BuildGate::Slot slot{gate, BuildGate::Kind::Game};
        std::scoped_lock lock{order_mutex};
        order.push_back('g');
    });
    std::this_thread::sleep_for(60ms);
    {
        std::scoped_lock lock{order_mutex};
        CHECK(order.empty()); // both wait for the running build
    }
    holder.reset();
    warmup.join();
    game.join();
    CHECK((order == std::vector<char>{'g', 'w'}));
}

void TestPauseWaitsForParallelBuildsAndTimesOut() {
    BuildGate gate;
    std::atomic<bool> release{false};
    std::thread build([&] {
        BuildGate::Slot slot{gate, BuildGate::Kind::Warmup};
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(20ms);
    CHECK(gate.EnableSerial());
    CHECK(!gate.WaitForParallelBuilds(30ms));
    release = true;
    build.join();
    CHECK(gate.WaitForParallelBuilds(5s));
}

// ---------------------------------------------------------------------------------------------
// The sources keep the contracts the helpers rely on

std::string ReadSource(const char* relative) {
    std::ifstream stream(fs::path{ALN_SOURCE_DIR} / relative, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::size_t Count(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

std::size_t Pos(const std::string& text, std::string_view needle) {
    const auto at = text.find(needle);
    CHECK(at != std::string::npos);
    return at;
}

void TestBinaryCacheFallbacksAreUntouched() {
    const auto serialization =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const auto graphics =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    const auto compute = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_compute_pipeline.cpp");
    const auto cache = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    CHECK(!serialization.empty() && !graphics.empty() && !compute.empty() && !cache.empty());

    // A rejected driver binary still ends in a normal compile, and a failed compile in a plain create.
    const auto rejected = Pos(serialization, "++binary_rejected;");
    const auto compile = Pos(serialization, "auto result = create(nullptr, captured_info, false);");
    CHECK(rejected < compile);
    CHECK(Count(serialization, "return create(cache, info, false);") == 3);
    CHECK(Pos(serialization, "XXH3_64bits(bytes.data(), bytes.size())") <
          Pos(serialization, "std::optional<std::vector<Binary>> Decode"));
    CHECK(Count(serialization, "XXH3_64bits(bytes.data(), bytes.size())") == 2);
    CHECK(Pos(serialization, "return PipelineCacheFiles::WriteFileAtomically(file, bytes);") >
          Pos(serialization, "bool Save(const std::filesystem::path& file"));

    // The clean retry comes after the cached creation, without the shared cache, once.
    CHECK(Pos(graphics, "CreateCachedGraphicsPipeline(device, pipeline_cache") <
          Pos(graphics, "device.createGraphicsPipelineUnique(nullptr, info)"));
    CHECK(Count(graphics, "createGraphicsPipelineUnique(nullptr") == 1);
    CHECK(Count(graphics, "throw PipelineCreationFailed") == 1);
    CHECK(Count(graphics, "Failed to create graphics pipeline: {}") == 0);
    CHECK(Pos(compute, "createComputePipelineUnique(pipeline_cache") <
          Pos(compute, "createComputePipelineUnique(nullptr"));
    CHECK(Count(compute, "throw PipelineCreationFailed") == 1);

    // The game's pipelines keep their fatal end; both kinds go through the retry.
    CHECK(Count(cache, "ASSERT_MSG(false, \"Failed to create {} pipeline: {}\"") == 1);
    CHECK(Count(cache, "CreateGamePipeline(\"graphics\"") == 1);
    CHECK(Count(cache, "CreateGamePipeline(\"compute\"") == 1);
    CHECK(Count(cache, "std::make_unique<GraphicsPipeline>") == 2);
    CHECK(Count(cache, "std::make_unique<ComputePipeline>") == 1);
}

void TestEveryCacheReaderRecoversFromCorruptData() {
    const auto serialization =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const auto serdes = ReadSource("gpu/shadps4/common/serdes.h");
    const auto storage = ReadSource("gpu/shadps4/video_core/cache_storage.cpp");
    CHECK(!serialization.empty() && !serdes.empty() && !storage.empty());
    // Where entries are read: the shader entry, the foreground walk and the background reader.
    CHECK(Count(serialization, "catch (const Serialization::CorruptData&)") == 3);
    CHECK(Count(serialization, "ReportCorrupt(") == 3);
    // Only reading throws it, and nothing reads past the end on its own.
    CHECK(serdes.find("common/assert.h") == std::string::npos && serdes.find("ASSERT") == std::string::npos);
    CHECK(Count(serdes, "throw CorruptData{}") == 5);
    CHECK(Count(serdes, "throw std::length_error") == 1);
    // Blobs are sealed on the way out and unsealed on the way in, whichever store holds them.
    CHECK(Count(storage, "Seal(bytes)") == 1 && Count(storage, "Unseal(") == 4);
    CHECK(Count(storage, "WriteFileAtomically(path, bytes)") == 1);
}

} // namespace

int main() {
    TestSealRoundTrips();
    TestTruncatedBlobsAreRejected();
    TestEmptyAndZeroFilledBlobsAreRejected();
    TestEverySingleBitFlipIsRejected();
    TestTrailingGarbageIsRejected();
    TestOldUnsealedBlobsRebuildInsteadOfAsserting();
    TestCorruptVectorCountsAreRejectedBeforeAllocating();
    TestCorruptStringLengthsAreRejectedBeforeAllocating();
    TestReadsNeverPassTheEnd();
    TestFailedWritesKeepThePreviousFile();
    TestSuccessfulWritesReplaceExactly();
    TestReplacingAFileThatIsOpenForReading();
    TestTemporaryFilesAreCleanedAtOpen();
    TestConcurrentWritersDoNotShareTemporaryFiles();
    TestStoreFilesAreSelectedAndFoldersStay();
    TestQueueCountStaysCorrectUnderConcurrentProducers();
    TestWorkerKeepsTheQueueAliveUntilItEnds();
    TestCloseFinishesWhatFitsAndDropsTheRest();
    TestStopAbandonsTheQueueWithoutHanging();
    TestCompatibleIdentityIsReused();
    TestEachBuildAndFormatHasItsOwnIdentity();
    TestCorruptIdentityRebuilds();
    TestDriverChangeKeepsTheStore();
    TestParallelFailureRecordsSerialOnlyForThatGpuAndDriver();
    TestSerialModeFollowsTheFailureAcrossRuns();
    TestWarmupFailureIsNeverFatal();
    TestGamePipelineGetsOneCleanRetry();
    TestPipelineFailureTable();
    TestSerialBuildsRunOneAtATimeAndGameBuildsGoFirst();
    TestPauseWaitsForParallelBuildsAndTimesOut();
    TestBinaryCacheFallbacksAreUntouched();
    TestEveryCacheReaderRecoversFromCorruptData();
    std::printf("pipeline cache tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
