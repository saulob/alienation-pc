#include "exit_game_stream.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "exit_game_script.h"
#include "runtime.h"
#include "utf8.h"

namespace {

constexpr uint32_t kBytecodeType = 0x53544144u;  // "DATS"
constexpr uint32_t kUnit = 64;

struct Storage {
    std::vector<unsigned char> payload;
    uint32_t zsize = 0;
    uint32_t dsize = 0;
};

Storage g_storage;

void SetDetail(char *detail, size_t size, const std::string &text) {
    if (detail && size) std::snprintf(detail, size, "%s", text.c_str());
}

int Fail(char *detail, size_t size, const std::string &text) {
    SetDetail(detail, size, text);
    return -1;
}

bool ReadAt(std::ifstream &file, uint64_t offset, void *data, size_t size) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    return file && file.gcount() == static_cast<std::streamsize>(size);
}

// Level 9 with zlib's default memory level keeps the patched block about 1.3 KiB within the original's units; memory
// level 9 compresses this bytecode worse and does not fit.
bool Deflate(const std::vector<unsigned char> &data, std::vector<unsigned char> &out) {
    z_stream stream{};
    if (deflateInit2(&stream, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.resize(deflateBound(&stream, static_cast<uLong>(data.size())));
    stream.next_in = const_cast<Bytef *>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return result == Z_STREAM_END;
}

}  // namespace

extern "C" int aln_exit_game_stream_install(const char *app0, const char *build_key, char *detail, size_t detail_size) {
    const auto started = std::chrono::steady_clock::now();
    const ExitGameScriptSite *site = exit_game_script_site(build_key);
    if (!site) return Fail(detail, detail_size, "not a supported build");
    if (!app0 || !*app0) return Fail(detail, detail_size, "no game folder");
    const std::filesystem::path archive = aln::PathFromUtf8(app0) / "stream.dat";
    std::error_code ec;
    const uint64_t stream_size = std::filesystem::file_size(archive, ec);
    if (ec) return Fail(detail, detail_size, "stream.dat cannot be read");
    const uint64_t block_end = site->block + static_cast<uint64_t>(site->units) * kUnit;
    if (block_end > stream_size) return Fail(detail, detail_size, "stream.dat is shorter than the build's");

    std::ifstream file(archive, std::ios::binary);
    uint32_t header[6];
    if (!file || !ReadAt(file, site->block, header, sizeof(header)))
        return Fail(detail, detail_size, "the bytecode block cannot be read");
    const uint32_t id = static_cast<uint32_t>(site->block / kUnit);
    if (header[0] != id || header[4] != id || header[5] != kBytecodeType || header[1] != site->zsize ||
        header[2] != site->dsize || header[3] != site->units)
        return Fail(detail, detail_size, "unexpected bytecode block");
    std::vector<unsigned char> compressed(site->zsize);
    if (!ReadAt(file, site->block + EXIT_GAME_BLOCK_HEADER, compressed.data(), compressed.size()))
        return Fail(detail, detail_size, "the bytecode cannot be read");
    if (crc32(0L, compressed.data(), static_cast<uInt>(compressed.size())) != site->crc)
        return Fail(detail, detail_size, "bytecode identity mismatch");

    std::vector<unsigned char> bytecode(site->dsize + exit_game_script_extra(site));
    uLongf inflated = site->dsize;
    if (uncompress(bytecode.data(), &inflated, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK ||
        inflated != site->dsize)
        return Fail(detail, detail_size, "the bytecode does not inflate");
    const char *why = "";
    const size_t patched = exit_game_script_patch(bytecode.data(), bytecode.size(), site, &why);
    if (!patched) return Fail(detail, detail_size, why);
    bytecode.resize(patched);

    Storage storage;
    if (!Deflate(bytecode, storage.payload)) return Fail(detail, detail_size, "the patched bytecode does not deflate");
    if (storage.payload.size() > static_cast<uint64_t>(site->units) * kUnit - EXIT_GAME_BLOCK_HEADER)
        return Fail(detail, detail_size, "the patched bytecode does not fit its block");
    storage.zsize = static_cast<uint32_t>(storage.payload.size());
    storage.dsize = static_cast<uint32_t>(patched);
    g_storage = std::move(storage);

    const RuntimeFilePatch patches[] = {
        {site->block + 4, 4, &g_storage.zsize},
        {site->block + 8, 4, &g_storage.dsize},
        {site->block + EXIT_GAME_BLOCK_HEADER, g_storage.zsize, g_storage.payload.data()},
    };
    if (runtime_file_overlay("/app0/stream.dat", stream_size, patches, sizeof(patches) / sizeof(*patches)) != 0)
        return Fail(detail, detail_size, "the file overlay was refused");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    SetDetail(detail, detail_size,
              "Options and Select Campaign rows added (" + std::to_string(g_storage.zsize) + " of " +
                  std::to_string(static_cast<uint64_t>(site->units) * kUnit - EXIT_GAME_BLOCK_HEADER) + " bytes, " +
                  std::to_string(elapsed) + " ms)");
    return 0;
}
