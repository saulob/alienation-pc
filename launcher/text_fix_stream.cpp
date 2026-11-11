#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "runtime.h"
#include "text_fix.h"
#include "utf8.h"

namespace {

constexpr uint32_t kLocalizationType = 0x52415744u;  // "DWAR"
constexpr uint32_t kUnit = 64;

struct Storage {
    std::vector<unsigned char> payload;
    uint32_t zsize = 0;
};

std::vector<Storage> g_storage;

int Fail(char *detail, size_t size, const std::string &text) {
    if (detail && size) std::snprintf(detail, size, "%s", text.c_str());
    return -1;
}

bool ReadAt(std::ifstream &file, uint64_t offset, void *data, size_t size) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    return file && file.gcount() == static_cast<std::streamsize>(size);
}

bool Deflate(const std::vector<unsigned char> &data, std::vector<unsigned char> &out) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    out.resize(size);
    if (compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), 9) != Z_OK) return false;
    out.resize(size);
    return true;
}

}  // namespace

extern "C" int aln_text_fix_install(const char *app0, const char *build_key, char *detail, size_t detail_size) {
    size_t count = 0;
    const AlnTextFix *fixes = aln_text_fixes(build_key, &count);
    if (!count) return 0;
    if (!app0 || !*app0) return Fail(detail, detail_size, "no game folder");
    const std::filesystem::path archive = aln::PathFromUtf8(app0) / "stream.dat";
    std::error_code ec;
    const uint64_t stream_size = std::filesystem::file_size(archive, ec);
    if (ec) return Fail(detail, detail_size, "stream.dat cannot be read");
    std::ifstream file(archive, std::ios::binary);

    std::vector<Storage> storage(count);
    std::vector<RuntimeFilePatch> patches;
    patches.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        const AlnTextFix &fix = fixes[i];
        const uint64_t capacity = static_cast<uint64_t>(fix.units) * kUnit - ALN_TEXT_FIX_PAYLOAD;
        uint32_t header[6];
        if (fix.block + static_cast<uint64_t>(fix.units) * kUnit > stream_size ||
            !ReadAt(file, fix.block, header, sizeof(header)))
            return Fail(detail, detail_size, "the localization block cannot be read");
        const uint32_t id = static_cast<uint32_t>(fix.block / kUnit);
        if (header[0] != id || header[4] != id || header[5] != kLocalizationType || header[1] != fix.zsize ||
            header[2] != fix.dsize || header[3] != fix.units)
            return Fail(detail, detail_size, "unexpected localization block");
        std::vector<unsigned char> compressed(fix.zsize);
        if (!ReadAt(file, fix.block + ALN_TEXT_FIX_PAYLOAD, compressed.data(), compressed.size()) ||
            crc32(0L, compressed.data(), static_cast<uInt>(compressed.size())) != fix.crc)
            return Fail(detail, detail_size, "localization identity mismatch");
        std::vector<unsigned char> text(fix.dsize);
        uLongf inflated = fix.dsize;
        if (uncompress(text.data(), &inflated, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK ||
            inflated != fix.dsize)
            return Fail(detail, detail_size, "the localization block does not inflate");
        if (!aln_text_fix_apply(text.data(), text.size(), &fix))
            return Fail(detail, detail_size, "the localization text differs");
        if (!Deflate(text, storage[i].payload) || storage[i].payload.size() > capacity)
            return Fail(detail, detail_size, "the fixed localization block does not fit");
        storage[i].zsize = static_cast<uint32_t>(storage[i].payload.size());
    }
    g_storage = std::move(storage);
    for (size_t i = 0; i < count; ++i) {
        patches.push_back({fixes[i].block + 4, 4, &g_storage[i].zsize});
        patches.push_back({fixes[i].block + ALN_TEXT_FIX_PAYLOAD, g_storage[i].zsize, g_storage[i].payload.data()});
    }
    if (runtime_file_overlay("/app0/stream.dat", stream_size, patches.data(), patches.size()) != 0)
        return Fail(detail, detail_size, "the file overlay was refused");
    if (detail && detail_size) std::snprintf(detail, detail_size, "%zu Portuguese voice language texts", count);
    return static_cast<int>(count);
}
