// Stale audio backlog protection: when the SDL audio stream queue is far above its target (the device stopped
// pulling), the queue is dropped and the pacing steering restarts. Pure helper tests plus source contracts on
// runtime_audio.c; no SDL, no audio device.
#include <climits>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "audio_backlog.h"

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

struct Format {
    int channels, sample_bytes, frames;
    size_t bytes() const { return size_t(channels) * size_t(sample_bytes) * size_t(frames); }
    int low() const { return 2 * int(bytes()); }
    int slack() const { return int(bytes()) / frames * AUDIO_BACKLOG_SLACK_FRAMES; }
    int threshold() const { return low() + slack(); }
    uint64_t period_ns() const { return uint64_t(frames) * 1000000000u / 48000u; }
};

constexpr Format Stereo256{2, 4, 256}, Stereo512{2, 4, 512}, Surround2048{8, 4, 2048}, Mono256{1, 2, 256};

bool Stale(const Format& f, int queued) {
    return audio_backlog_stale(queued, f.low(), f.bytes(), f.frames) != 0;
}

void BelowAndAtThresholdAreNormal() {
    for (const Format& f : {Stereo256, Stereo512, Surround2048, Mono256}) {
        CHECK(!Stale(f, 0));
        CHECK(!Stale(f, f.low()));
        CHECK(!Stale(f, f.threshold() - 1));
        CHECK(!Stale(f, f.threshold()));
        CHECK(Stale(f, f.threshold() + 1));
    }
}

void LargeQueueIsStale() {
    CHECK(Stale(Stereo256, 1 << 20));
    CHECK(Stale(Stereo256, INT_MAX));
    CHECK(Stale(Surround2048, 48000 * 8 * 4));
}

void ThresholdsPerFormat() {
    CHECK(Stereo256.threshold() == 4096 + 38400);
    CHECK(Stereo512.threshold() == 8192 + 38400);
    CHECK(Surround2048.threshold() == 131072 + 153600);
    CHECK(Mono256.threshold() == 1024 + 9600);
    CHECK(!Stale(Stereo256, 42496) && Stale(Stereo256, 42497));
    CHECK(!Stale(Stereo512, 46592) && Stale(Stereo512, 46593));
    CHECK(!Stale(Surround2048, 284672) && Stale(Surround2048, 284673));
    CHECK(!Stale(Mono256, 10624) && Stale(Mono256, 10625));
}

void InvalidInputIsIgnored() {
    const Format& f = Stereo256;
    CHECK(!audio_backlog_stale(1 << 20, f.low(), f.bytes(), 0));
    CHECK(!audio_backlog_stale(1 << 20, f.low(), f.bytes(), -256));
    CHECK(!audio_backlog_stale(1 << 20, f.low(), 0, f.frames));
    CHECK(!audio_backlog_stale(-1, f.low(), f.bytes(), f.frames));
    CHECK(!audio_backlog_stale(INT_MIN, f.low(), f.bytes(), f.frames));
    int queued = 1 << 20, window = 7;
    int64_t adjust = 5;
    CHECK(audio_backlog_drop(&queued, f.low(), 0, f.frames, &window, &adjust) == 0);
    CHECK(audio_backlog_drop(&queued, f.low(), f.bytes(), 0, &window, &adjust) == 0);
    queued = -5;
    CHECK(audio_backlog_drop(&queued, f.low(), f.bytes(), f.frames, &window, &adjust) == 0);
    CHECK(queued == -5 && window == 7 && adjust == 5);
}

void ArithmeticDoesNotWrap() {
    CHECK(!audio_backlog_stale(INT_MAX, INT_MAX, 1, 1));
    CHECK(!audio_backlog_stale(INT_MAX, INT_MAX, size_t(1) << 31, 1));
    CHECK(!audio_backlog_stale(1, INT_MAX, SIZE_MAX, 1));
    CHECK(audio_backlog_stale(INT_MAX, INT_MAX / 2 + INT_MAX / 4, 1 << 20, 4800 * 2));
    CHECK(!audio_backlog_stale(INT_MAX, INT_MAX - 1, 4, 1));
    CHECK(audio_backlog_stale(INT_MAX, 0, 1000, 1000));
    CHECK(!audio_backlog_stale(0, INT_MAX, 4, 1));
}

void DropResetsSteeringOnly() {
    const Format& f = Stereo256;
    int queued = 300000, window = 19;
    int64_t adjust = -int64_t(f.period_ns() / 32);
    const int dropped = audio_backlog_drop(&queued, f.low(), f.bytes(), f.frames, &window, &adjust);
    CHECK(dropped == 300000 - f.low());
    CHECK(queued == 0);
    CHECK(window == 0);
    CHECK(adjust == 0);
}

void NormalQueueKeepsSteering() {
    const Format& f = Stereo256;
    for (int level : {0, f.low(), f.threshold()}) {
        int queued = level, window = 19;
        int64_t adjust = int64_t(f.period_ns() / 32);
        CHECK(audio_backlog_drop(&queued, f.low(), f.bytes(), f.frames, &window, &adjust) == 0);
        CHECK(queued == level && window == 19 && adjust == int64_t(f.period_ns() / 32));
    }
}

void MillisecondsConversion() {
    const Format& f = Stereo256;
    CHECK(f.period_ns() == 5333333);
    const double one_buffer = audio_backlog_ms(int(f.bytes()), f.period_ns(), f.bytes());
    CHECK(one_buffer > 5.33 && one_buffer < 5.34);
    const double hundred = audio_backlog_ms(f.slack(), f.period_ns(), f.bytes());
    CHECK(hundred > 99.99 && hundred < 100.01);
    const double two_seconds = audio_backlog_ms(f.slack() * 20, f.period_ns(), f.bytes());
    CHECK(two_seconds > 1999.9 && two_seconds < 2000.1);
    const double surround = audio_backlog_ms(Surround2048.slack(), Surround2048.period_ns(), Surround2048.bytes());
    CHECK(surround > 99.9 && surround < 100.1);
}

std::string ReadSource(const char* path) {
    std::ifstream in(std::string(ALN_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

size_t Find(const std::string& text, const std::string& needle, size_t from = 0) {
    return text.find(needle, from);
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

void RuntimeAudioContracts() {
    const std::string audio = ReadSource("src/runtime_audio.c");
    CHECK(!audio.empty());
    CHECK(audio.find("#include \"audio_backlog.h\"") != std::string::npos);

    const size_t steering = Find(audio, "int low=2*(int)bytes, queued=SDL_GetAudioStreamQueued(p->stream);");
    const size_t drop = Find(audio, "audio_backlog_drop(&queued,low,bytes,p->frames,&p->window_count,&p->adjust_ns)");
    const size_t clear = Find(audio, "SDL_ClearAudioStream(p->stream);");
    const size_t starvation = Find(audio, "if (queued<(int)bytes) {");
    const size_t window = Find(audio, "if (!p->window_count || queued<p->window_min) p->window_min=queued;");
    CHECK(steering != std::string::npos && drop != std::string::npos && clear != std::string::npos);
    CHECK(steering < drop && drop < clear && clear < starvation && starvation < window);
    CHECK(Count(audio, "SDL_ClearAudioStream(") == 1);
    const size_t guard = Find(audio, "if (dropped) {");
    CHECK(guard != std::string::npos && drop < guard && guard < clear && clear - guard < 250);

    CHECK(audio.find("Runtime: audio port %d dropped %.0f ms of backlog (device stalled)") != std::string::npos);
    CHECK(Count(audio, "dropped %.0f ms of backlog") == 1);

    CHECK(audio.find("SDL_PutAudioStreamData(p->stream,silence,low-queued<(int)bytes ? low-queued : (int)bytes);") !=
          std::string::npos);
    CHECK(audio.find("p->adjust_ns=p->window_min>low+2*(int)bytes ? (int64_t)(period/32) : p->window_min<low ? "
                     "-(int64_t)(period/32) : 0;") != std::string::npos);
    CHECK(audio.find("if (++p->window_count==32) {") != std::string::npos);
    CHECK(audio.find("if (queued==0) ++p->stat_starved;") != std::string::npos);
    CHECK(audio.find("if (queued<p->stat_min_queued) p->stat_min_queued=queued;") != std::string::npos);
    CHECK(audio.find("DEBUG_" "SAULO") == std::string::npos);
    CHECK(ReadSource("src/audio_backlog.h").find("DEBUG_" "SAULO") == std::string::npos);
}

} // namespace

int main() {
    BelowAndAtThresholdAreNormal();
    LargeQueueIsStale();
    ThresholdsPerFormat();
    InvalidInputIsIgnored();
    ArithmeticDoesNotWrap();
    DropResetsSteeringOnly();
    NormalQueueKeepsSteering();
    MillisecondsConversion();
    RuntimeAudioContracts();
    std::printf("audio backlog tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
