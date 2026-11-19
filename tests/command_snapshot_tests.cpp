// Graphics and compute command memory: the command processor reads command words in place, when it reaches them,
// as the GPU does. The game (behind a label wait) and earlier packets may write words it has yet to read; a copy
// taken at submission would miss those writes. Indirect buffer ranges are validated before they are read. The
// source contracts pin how liverpool.cpp uses the helpers. No GPU device, no Vulkan.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "video_core/amdgpu/command_memory.h"

namespace {

int checks = 0;
int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(condition)) {                                                                                            \
            ++failures;                                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);                                                   \
        }                                                                                                              \
    } while (0)

using AmdGpu::FirstUnreadWordWritten;
using AmdGpu::GuestCommandWords;
using Words = std::vector<u32>;

constexpr u32 Nop = 0x10, WriteData = 0x37, IndirectBuffer = 0x3f, DispatchDirect = 0x15;

u32 Type3(u32 opcode, u32 words) {
    return 0xc0000000u | ((words - 2) << 16) | (opcode << 8);
}

u64 Address(const u32* word) {
    return reinterpret_cast<u64>(word);
}

struct GuestMemory {
    explicit GuestMemory(std::vector<Words*> regions) : regions{std::move(regions)} {}

    u64 operator()(u64 address, u64 bytes) {
        ++calls;
        for (Words* region : regions) {
            const u64 begin = Address(region->data());
            const u64 end = begin + region->size() * sizeof(u32);
            if (address >= begin && address < end) {
                return std::min(bytes, end - address);
            }
        }
        return 0;
    }

    std::vector<Words*> regions;
    int calls = 0;
};

// A command processor reading in place: NOP payloads are recorded, a write packet stores its data at a guest address
// (production runs WriteData and CPU-path DmaData as a memcpy), an indirect buffer is validated and read in place.
struct Processor {
    explicit Processor(GuestMemory& memory) : memory{memory} {}

    GuestMemory& memory;
    Words read;
    std::vector<size_t> self_writes;
    bool rejected = false;

    void Run(std::span<const u32> stream) {
        while (!stream.empty()) {
            const u32 header = stream[0];
            const u32 opcode = (header >> 8) & 0xff;
            const size_t words = std::min<size_t>(((header >> 16) & 0x3fff) + 2, stream.size());
            const auto unread = stream.subspan(words);
            if (opcode == Nop) {
                read.push_back(stream[1]);
            } else if (opcode == WriteData) {
                const u64 address = stream[1] | (u64(stream[2]) << 32);
                const u64 bytes = (words - 3) * sizeof(u32);
                if (const auto word = FirstUnreadWordWritten(unread, address, bytes)) {
                    self_writes.push_back(*word);
                }
                std::memcpy(reinterpret_cast<void*>(address), stream.data() + 3, bytes);
            } else if (opcode == IndirectBuffer) {
                const auto target =
                    GuestCommandWords(stream[1] | (u64(stream[2] & 0xffff) << 32), stream[3] & 0xfffff, memory);
                if (!target) {
                    rejected = true;
                    return;
                }
                if ((stream[3] >> 20) & 1) {
                    stream = *target;
                    continue;
                }
                Run(*target);
            } else if (opcode == DispatchDirect) {
                read.insert(read.end(), stream.begin() + 1, stream.begin() + 4);
            }
            stream = stream.subspan(words);
        }
    }
};

void Mark(Words& words, u32 value) {
    words.push_back(Type3(Nop, 2));
    words.push_back(value);
}

void Write(Words& words, const u32* target, std::initializer_list<u32> data) {
    words.push_back(Type3(WriteData, static_cast<u32>(data.size()) + 3));
    words.push_back(static_cast<u32>(Address(target)));
    words.push_back(static_cast<u32>(Address(target) >> 32));
    words.insert(words.end(), data);
}

void Ib(Words& words, const Words& target, bool chain = false) {
    words.push_back(Type3(IndirectBuffer, 4));
    words.push_back(static_cast<u32>(Address(target.data())));
    words.push_back(static_cast<u32>(Address(target.data()) >> 32) & 0xffff);
    words.push_back(static_cast<u32>(target.size()) | (chain ? 1u << 20 : 0));
}

void Dispatch(Words& words, u32 x, u32 y, u32 z) {
    words.push_back(Type3(DispatchDirect, 5));
    words.insert(words.end(), {x, y, z, 1});
}

// The game builds a frame, submits it, then fills in late values before releasing the wait in front of them.
void CopyAtSubmissionMissesLatePatches() {
    Words dcb;
    Mark(dcb, 1);
    Mark(dcb, 0);
    const Words copy_at_submit = dcb;
    dcb[3] = 42;
    CHECK(copy_at_submit[3] == 0);
    GuestMemory memory{{&dcb}};
    Processor cp{memory};
    cp.Run(dcb);
    CHECK((cp.read == Words{1, 42}));
}

void SelfModifyingStreamReadsTheWrite() {
    Words dcb(64);
    dcb.clear();
    Mark(dcb, 1);
    const size_t write_at = dcb.size();
    Write(dcb, nullptr, {77});
    const size_t target = dcb.size();
    Mark(dcb, 2);
    const u32* patched = dcb.data() + target + 1;
    dcb[write_at + 1] = static_cast<u32>(Address(patched));
    dcb[write_at + 2] = static_cast<u32>(Address(patched) >> 32);
    GuestMemory memory{{&dcb}};
    Processor cp{memory};
    cp.Run(dcb);
    CHECK(dcb[target + 1] == 77);
    CHECK((cp.read == Words{1, 77}));
    CHECK((cp.self_writes == std::vector<size_t>{1}));
}

void SelfModifyingNestedBufferReadsTheWrite() {
    Words ib, top;
    ib.reserve(32);
    Write(ib, nullptr, {55});
    const size_t target = ib.size();
    Mark(ib, 3);
    Mark(ib, 4);
    ib[1] = static_cast<u32>(Address(ib.data() + target + 1));
    ib[2] = static_cast<u32>(Address(ib.data() + target + 1) >> 32);
    Ib(top, ib);
    Mark(top, 9);
    GuestMemory memory{{&ib}};
    ib[target + 3] = 5;
    Processor cp{memory};
    cp.Run(top);
    CHECK((cp.read == Words{55, 5, 9}));
    CHECK((cp.self_writes == std::vector<size_t>{1}));
}

// A patches a word of B before chaining to it; B patches a later word of its own, then chains to C.
void ChainedBuffersReadEarlierWrites() {
    Words a, b, c;
    b.reserve(32);
    Mark(c, 30);
    Write(b, nullptr, {21});
    Mark(b, 20);
    Mark(b, 22);
    Ib(b, c, true);
    b[1] = static_cast<u32>(Address(b.data() + 7));
    b[2] = static_cast<u32>(Address(b.data() + 7) >> 32);
    Write(a, b.data() + 5, {25});
    Ib(a, b, true);
    GuestMemory memory{{&b, &c}};
    Processor cp{memory};
    cp.Run(a);
    CHECK((cp.read == Words{25, 21, 30}));
    CHECK((cp.self_writes == std::vector<size_t>{3}));
    CHECK(c[1] == 30);
}

void DispatchDimensionsReadAfterTheirWrite() {
    for (const auto dims : {std::array<u32, 3>{0, 0, 0}, std::array<u32, 3>{8, 2, 8}}) {
        Words ring, ib, chained;
        ib.reserve(32);
        chained.reserve(32);
        Write(chained, nullptr, {dims[0], dims[1], dims[2]});
        const size_t chained_dispatch = chained.size();
        Dispatch(chained, 0xdead, 0xdead, 0xdead);
        chained[1] = static_cast<u32>(Address(chained.data() + chained_dispatch + 1));
        chained[2] = static_cast<u32>(Address(chained.data() + chained_dispatch + 1) >> 32);
        Write(ib, nullptr, {dims[0], dims[1], dims[2]});
        const size_t dispatch = ib.size();
        Dispatch(ib, 0xdead, 0xdead, 0xdead);
        Ib(ib, chained, true);
        ib[1] = static_cast<u32>(Address(ib.data() + dispatch + 1));
        ib[2] = static_cast<u32>(Address(ib.data() + dispatch + 1) >> 32);
        ring.reserve(32);
        Write(ring, nullptr, {dims[0], dims[1], dims[2]});
        const size_t ring_dispatch = ring.size();
        Dispatch(ring, 0xdead, 0xdead, 0xdead);
        Ib(ring, ib);
        ring[1] = static_cast<u32>(Address(ring.data() + ring_dispatch + 1));
        ring[2] = static_cast<u32>(Address(ring.data() + ring_dispatch + 1) >> 32);

        GuestMemory memory{{&ib, &chained}};
        Processor cp{memory};
        cp.Run(ring);
        const Words expected = {dims[0], dims[1], dims[2], dims[0], dims[1], dims[2], dims[0], dims[1], dims[2]};
        CHECK(cp.read == expected);
        CHECK((cp.self_writes == std::vector<size_t>{1, 1, 1}));
        CHECK(ring[ring_dispatch + 1] == dims[0] && ring[ring_dispatch + 3] == dims[2]);
    }
}

void PartialWritesFindTheFirstChangedWord() {
    Words stream(8, 0);
    const u64 begin = Address(stream.data());
    CHECK(FirstUnreadWordWritten(stream, begin, 1) == 0);
    CHECK(FirstUnreadWordWritten(stream, begin + 3, 2) == 0);
    CHECK(FirstUnreadWordWritten(stream, begin + 13, 1) == 3);
    CHECK(FirstUnreadWordWritten(stream, begin + 15, 2) == 3);
    CHECK(FirstUnreadWordWritten(stream, begin + 31, 1) == 7);
    CHECK(FirstUnreadWordWritten(stream, begin + 28, 4) == 7);
    CHECK(FirstUnreadWordWritten(stream, begin + 32, 4) == std::nullopt);
    CHECK(FirstUnreadWordWritten(stream, begin - 4, 4) == std::nullopt);
    CHECK(FirstUnreadWordWritten(stream, begin - 4, 5) == 0);
    CHECK(FirstUnreadWordWritten(stream, begin - 1, 100) == 0);
    CHECK(FirstUnreadWordWritten(stream, begin + 8, 0) == std::nullopt);
    CHECK(FirstUnreadWordWritten(stream, UINT64_MAX - 3, UINT64_MAX) == std::nullopt);
    CHECK(FirstUnreadWordWritten(stream, 0, UINT64_MAX) == 0);
    CHECK(FirstUnreadWordWritten({}, begin, 4) == std::nullopt);
    CHECK(FirstUnreadWordWritten(std::span<const u32>{stream}.subspan(8), begin + 28, 8) == std::nullopt);
}

void WritesAreAttributedToTheOverlappedStreamOnly() {
    Words graphics(100, 0), nested(40, 0), compute(60, 0);
    const u64 target = Address(nested.data() + 17);
    CHECK(FirstUnreadWordWritten(graphics, target, 8) == std::nullopt);
    CHECK(FirstUnreadWordWritten(nested, target, 8) == 17);
    CHECK(FirstUnreadWordWritten(compute, target, 8) == std::nullopt);
    CHECK(FirstUnreadWordWritten(std::span<const u32>{nested}.subspan(18), target, 8) == 0);
    CHECK(FirstUnreadWordWritten(std::span<const u32>{nested}.subspan(19), target, 8) == std::nullopt);
}

void IndirectBufferRangesAreValidated() {
    Words region(64, 7);
    GuestMemory memory{{&region}};
    const u64 address = Address(region.data());
    constexpr u64 max_words = static_cast<u64>(INTPTR_MAX) / sizeof(u32);

    CHECK(!GuestCommandWords(UINT64_MAX - 7, 4, memory));
    CHECK(!GuestCommandWords(UINT64_MAX - 3, 1, memory));
    CHECK(!GuestCommandWords(0, 4, memory));
    CHECK(!GuestCommandWords(address, max_words + 1, memory));
    CHECK(!GuestCommandWords(address, UINT64_MAX, memory));
    CHECK(memory.calls == 0);

    CHECK(!GuestCommandWords(address + 64 * 4, 1, memory));
    CHECK(!GuestCommandWords(address + 60 * 4, 5, memory));
    CHECK(!GuestCommandWords(0x1000, 4, memory));
    CHECK(memory.calls == 3);

    const auto empty = GuestCommandWords(address, 0, memory);
    CHECK(empty && empty->empty());
    CHECK(memory.calls == 3);
    const auto whole = GuestCommandWords(address, 64, memory);
    CHECK(whole && whole->data() == region.data() && whole->size() == 64);

    constexpr u32 max_ib_words = (1u << 20) - 1;
    Words big(max_ib_words, 1);
    GuestMemory big_memory{{&big}};
    CHECK(GuestCommandWords(Address(big.data()), max_ib_words, big_memory).has_value());
    CHECK(!GuestCommandWords(Address(big.data()) + 4, max_ib_words, big_memory));

    Words unmapped(8, 0), top;
    Mark(top, 5);
    Ib(top, unmapped);
    Mark(top, 6);
    Processor cp{memory};
    cp.Run(top);
    CHECK(cp.rejected);
    CHECK((cp.read == Words{5}));
}

std::string ReadText(const char* relative) {
    std::ifstream in(std::string(ALN_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Between(const std::string& text, const char* from, const char* to) {
    const size_t begin = text.find(from);
    const size_t end = text.find(to, begin);
    return begin == std::string::npos || end == std::string::npos ? std::string{} : text.substr(begin, end - begin);
}

bool Before(const std::string& text, const char* first, const char* second) {
    const size_t a = text.find(first), b = text.find(second, a == std::string::npos ? 0 : a);
    return a != std::string::npos && b != std::string::npos;
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

void LiverpoolReadsCommandMemoryInPlace() {
    const std::string liverpool = ReadText("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    const std::string header = ReadText("gpu/shadps4/video_core/amdgpu/liverpool.h");
    const std::string gnm = ReadText("gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp");
    const std::string settings = ReadText("gpu/shim/core/emulator_settings.h");
    CHECK(!liverpool.empty() && !header.empty() && !gnm.empty() && !settings.empty());
    CHECK(!std::ifstream(std::string(ALN_SOURCE_DIR) + "/gpu/shadps4/video_core/amdgpu/command_snapshot.h"));

    for (const std::string* text : {&liverpool, &header, &gnm, &settings}) {
        for (const char* gone : {"CommandSnapshot", "command_snapshots", "CommandStream", "GuestRest", "TakeGuest",
                                 "ReportRewrittenCommands", "CopyCmdBuffers", "ReserveCopyBufferSpace",
                                 "IsCopyGpuBuffers", "dcb_buffer", "ccb_buffer", "DEBUG_" "SAULO"}) {
            CHECK(text->find(gone) == std::string::npos);
        }
    }

    const std::string submit = Between(liverpool, "void Liverpool::SubmitGfx", "void Liverpool::SubmitAsc");
    CHECK(submit.find("draw_prep->Build(dcb)") != std::string::npos);
    CHECK(submit.find("auto task = ProcessGraphics(dcb, ccb, seq);") != std::string::npos);

    CHECK(Count(liverpool, "IndirectBufferWords(*indirect_buffer)") == 5);
    CHECK(liverpool.find("indirect_buffer->Address") == std::string::npos);
    const std::string validate = Between(liverpool, "std::span<const u32> IndirectBufferWords(", "bool WritesUnreadCommands(");
    CHECK(Before(validate, "GuestCommandWords(address, words,", "ClampRangeSize(at, bytes)"));
    CHECK(Before(validate, "if (!guest) {", "UNREACHABLE_MSG("));

    const std::string graphics = Between(liverpool, "Liverpool::Task Liverpool::ProcessGraphics", "template <bool is_indirect>");
    const std::string dma = Between(graphics, "case PM4ItOpcode::DmaData: {", "case PM4ItOpcode::WriteData: {");
    CHECK(Before(dma, "WritesUnreadCommands(\"gfx\", opcode, unread, dma_data->DstAddress<VAddr>(),",
                 "rasterizer->DrainDrawPipe(static_cast<u32>(opcode));"));
    const std::string write = Between(graphics, "case PM4ItOpcode::WriteData: {", "case PM4ItOpcode::CopyData: {");
    CHECK(Before(write, "WritesUnreadCommands(\"gfx\", opcode, unread, write_data->Address<u64>(),",
                 "rasterizer->DrainDrawPipe(static_cast<u32>(opcode));"));
    CHECK(Count(graphics, "const auto unread = dcb.subspan(std::min<size_t>(count + 1, dcb.size()));") == 2);

    const std::string compute = Between(liverpool, "Liverpool::Task Liverpool::ProcessCompute", "void Liverpool::SubmitGfx");
    CHECK(compute.find("dst_addr >= base_addr && dst_addr < base_addr + acb_size") != std::string::npos);
    CHECK(Count(compute, "WritesUnreadCommands(\"compute\", opcode,") == 2);
    CHECK(Before(compute, "WritesUnreadCommands(\"compute\", opcode,", "rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);"));
}

} // namespace

int main() {
    CopyAtSubmissionMissesLatePatches();
    SelfModifyingStreamReadsTheWrite();
    SelfModifyingNestedBufferReadsTheWrite();
    ChainedBuffersReadEarlierWrites();
    DispatchDimensionsReadAfterTheirWrite();
    PartialWritesFindTheFirstChangedWord();
    WritesAreAttributedToTheOverlappedStreamOnly();
    IndirectBufferRangesAreValidated();
    LiverpoolReadsCommandMemoryInPlace();
    std::printf("command memory tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
