// Decoded NV12 frames as the guest receives them (ALN-VUK9PE, shadPS4 #4525 green video): rows at the 64-byte
// pitch whatever the decoder's row size, and the rows below the picture filled from its last row.
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "core/libraries/videodec/video_utils.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}

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

u8 LumaAt(u32 x, u32 y) {
    return u8(16 + (x * 7 + y * 13) % 200);
}

u8 ChromaAt(u32 x, u32 y) {
    return u8(64 + (x * 3 + y * 11) % 120);
}

void TestFrame(u32 width, u32 height, u32 linesize) {
    std::vector<u8> luma(linesize * height), chroma(linesize * (height / 2));
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) luma[y * linesize + x] = LumaAt(x, y);
    for (u32 y = 0; y < height / 2; ++y)
        for (u32 x = 0; x < width; ++x) chroma[y * linesize + x] = ChromaAt(x, y);

    AVFrame frame{};
    frame.width = int(width);
    frame.height = int(height);
    frame.data[0] = luma.data();
    frame.data[1] = chroma.data();
    frame.linesize[0] = int(linesize);
    frame.linesize[1] = int(linesize);

    const u32 pitch = (width + 63) & ~63u;
    const u32 padded = (height + 15) & ~15u;
    std::vector<u8> out(pitch * padded * 3 / 2, 0);
    Libraries::Videodec::CopyNV12Data(out.data(), out.size(), frame);

    bool luma_ok = true, chroma_ok = true;
    for (u32 y = 0; y < padded; ++y) {
        const u32 source_row = y < height ? y : height - 1;
        for (u32 x = 0; x < width; ++x) luma_ok &= out[y * pitch + x] == LumaAt(x, source_row);
    }
    const u8* uv = out.data() + pitch * padded;
    for (u32 y = 0; y < padded / 2; ++y) {
        const u32 source_row = y < height / 2 ? y : height / 2 - 1;
        for (u32 x = 0; x < width; ++x) chroma_ok &= uv[y * pitch + x] == ChromaAt(x, source_row);
    }
    CHECK(luma_ok);
    CHECK(chroma_ok);
}

} // namespace

int main() {
    // Decoder rows wider than the picture, at sizes on and off the guest pitch.
    TestFrame(70, 20, 94);
    TestFrame(1920, 1080, 1984);
    TestFrame(128, 64, 160);
    // Rows exactly at the guest pitch, the bulk copy.
    TestFrame(1920, 1080, 1920);
    TestFrame(70, 20, 128);
    std::printf("video frame tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
