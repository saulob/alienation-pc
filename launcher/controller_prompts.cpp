#include "controller_prompts.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "runtime.h"
#include "utf8.h"

namespace {

struct BlockHeader {
    uint32_t id0;
    uint32_t zsize;
    uint32_t dsize;
    uint32_t units;
    uint32_t id1;
    uint32_t type;
};

struct GlyphPatchStorage {
    std::vector<unsigned char> payload;
    uint32_t zsize = 0;
};

constexpr size_t kAtlasSize = 2048;
constexpr size_t kAtlasPixels = kAtlasSize * kAtlasSize;
constexpr size_t kAtlasPrefix = 16 + 0x100;
constexpr size_t kAtlasTilesPerRow = kAtlasSize / 8;
constexpr uint32_t kSceneType = 0x53434E45u;  // "ENCS"
constexpr uint32_t kScriptType = 0x53435249u;  // "IRCS"

std::vector<GlyphPatchStorage> g_glyph_storage;

uint32_t Crc32(const unsigned char *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool ReadAt(std::ifstream &file, uint64_t offset, void *data, size_t size) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    file.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    return file.gcount() == static_cast<std::streamsize>(size);
}

void SetDetail(char *detail, size_t size, const std::string &text) {
    if (detail && size) {
        std::snprintf(detail, size, "%s", text.c_str());
    }
}

uint32_t ReadLe32(const unsigned char *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

size_t MicroIndex(size_t x, size_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// The game's font atlases are 8x8-tiled R8 surfaces.
void UntileAtlas(const unsigned char *source, unsigned char *target) {
    for (size_t y = 0; y < kAtlasSize; ++y) {
        for (size_t x = 0; x < kAtlasSize; ++x) {
            const size_t tile = (y / 8) * kAtlasTilesPerRow + x / 8;
            target[y * kAtlasSize + x] = source[tile * 64 + MicroIndex(x & 7, y & 7)];
        }
    }
}

void TileAtlas(const unsigned char *source, unsigned char *target) {
    for (size_t y = 0; y < kAtlasSize; ++y) {
        for (size_t x = 0; x < kAtlasSize; ++x) {
            const size_t tile = (y / 8) * kAtlasTilesPerRow + x / 8;
            target[tile * 64 + MicroIndex(x & 7, y & 7)] = source[y * kAtlasSize + x];
        }
    }
}

bool BuildGlyphPatches(std::ifstream &file, const char *build_key, uint64_t stream_size,
                       std::vector<RuntimeFilePatch> &patches, std::string &detail) {
    size_t count = 0;
    const AlnPromptGlyphAtlas *atlases = AlnPromptGlyphAtlases(&count);
    g_glyph_storage.clear();
    g_glyph_storage.reserve(count);
    std::vector<RuntimeFilePatch> glyph_patches;
    glyph_patches.reserve(count * 2);
    size_t applied = 0;

    for (size_t index = 0; index < count; ++index) {
        const AlnPromptGlyphAtlas &atlas = atlases[index];
        if (std::strcmp(atlas.build, build_key) != 0) {
            continue;
        }
        if (atlas.offset > stream_size || atlas.offset + 40 + atlas.original_zsize > stream_size) {
            detail = std::string("glyph atlas outside the archive: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        BlockHeader header{};
        if (!ReadAt(file, atlas.offset, &header, sizeof(header))) {
            detail = std::string("cannot read the glyph atlas: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        if (header.id0 != header.id1 || header.type != 0x54455820u ||
            header.zsize != atlas.original_zsize || header.dsize != atlas.original_dsize ||
            header.units != atlas.block_units) {
            detail = std::string("unexpected glyph atlas: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        std::vector<unsigned char> compressed(atlas.original_zsize);
        if (!ReadAt(file, atlas.offset + 40, compressed.data(), compressed.size())) {
            detail = std::string("cannot read the glyph payload: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        if (Crc32(compressed.data(), compressed.size()) != atlas.original_crc32) {
            detail = std::string("glyph atlas identity mismatch: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        std::vector<unsigned char> payload(atlas.original_dsize);
        uLongf payload_size = static_cast<uLongf>(payload.size());
        if (uncompress(payload.data(), &payload_size, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK ||
            payload_size != payload.size() || payload.size() != kAtlasPrefix + kAtlasPixels) {
            detail = std::string("glyph atlas decompression failed: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        // GNF descriptor: format in word1[25:20], dimensions in word2[13:0] and word2[27:14].
        const unsigned char *gnf = payload.data() + 16;
        const uint32_t format = (ReadLe32(gnf + 0x14) >> 20) & 0x3F;
        const uint32_t dimensions = ReadLe32(gnf + 0x18);
        const uint32_t width = (dimensions & 0x3FFF) + 1;
        const uint32_t height = ((dimensions >> 14) & 0x3FFF) + 1;
        if (std::memcmp(payload.data() + 16, "GNF ", 4) != 0 || format != 1 || width != kAtlasSize ||
            height != kAtlasSize) {
            detail = std::string("unexpected glyph atlas format: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }

        std::vector<unsigned char> pixels(kAtlasPixels);
        UntileAtlas(payload.data() + kAtlasPrefix, pixels.data());
        for (size_t mask_index = 0; mask_index < atlas.mask_count; ++mask_index) {
            const AlnPromptGlyphMask &mask = atlas.masks[mask_index];
            if (mask.x + mask.width > kAtlasSize || mask.y + mask.height > kAtlasSize ||
                mask.dsize != static_cast<uint32_t>(mask.width) * mask.height) {
                detail = std::string("invalid glyph mask: ") + atlas.name;
                g_glyph_storage.clear();
                return false;
            }
            std::vector<unsigned char> glyph(mask.dsize);
            uLongf glyph_size = static_cast<uLongf>(glyph.size());
            if (uncompress(glyph.data(), &glyph_size, mask.data, mask.zsize) != Z_OK ||
                glyph_size != glyph.size()) {
                detail = std::string("glyph mask decompression failed: ") + atlas.name;
                g_glyph_storage.clear();
                return false;
            }
            for (size_t row = 0; row < mask.height; ++row) {
                std::memcpy(pixels.data() + (mask.y + row) * kAtlasSize + mask.x,
                            glyph.data() + row * mask.width, mask.width);
            }
        }

        GlyphPatchStorage storage;
        std::vector<unsigned char> plain(kAtlasPrefix + kAtlasPixels);
        std::memcpy(plain.data(), payload.data(), kAtlasPrefix);
        TileAtlas(pixels.data(), plain.data() + kAtlasPrefix);
        std::vector<unsigned char> packed(compressBound(static_cast<uLong>(plain.size())));
        uLongf packed_size = static_cast<uLongf>(packed.size());
        if (compress2(packed.data(), &packed_size, plain.data(), static_cast<uLong>(plain.size()), 9) != Z_OK) {
            detail = std::string("glyph atlas compression failed: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        packed.resize(packed_size);
        storage.payload = std::move(packed);
        if (static_cast<uint64_t>(40) + storage.payload.size() > static_cast<uint64_t>(atlas.block_units) * 64) {
            detail = std::string("glyph atlas does not fit: ") + atlas.name;
            g_glyph_storage.clear();
            return false;
        }
        storage.zsize = static_cast<uint32_t>(storage.payload.size());
        g_glyph_storage.push_back(std::move(storage));
        GlyphPatchStorage &stored = g_glyph_storage.back();
        glyph_patches.push_back({atlas.offset + 4, 4, &stored.zsize});
        glyph_patches.push_back({atlas.offset + 40, stored.payload.size(), stored.payload.data()});
        ++applied;
    }

    if (!applied) {
        detail = "no glyph atlases for the build";
        return false;
    }
    patches.insert(patches.end(), glyph_patches.begin(), glyph_patches.end());
    detail = std::to_string(applied) + " glyph atlases";
    return true;
}

} // namespace

extern "C" int aln_controller_prompts_install(const char *settings_file, const char *app0, const char *build_key,
                                               char *detail, size_t detail_size) {
    const int style = aln_controller_prompts_style(settings_file);
    if (style != ALN_PROMPTS_XBOX) {
        SetDetail(detail, detail_size, "PlayStation (Original)");
        return 0;
    }
    if (!build_key || !*build_key) {
        SetDetail(detail, detail_size, "unsupported game build");
        return -1;
    }
    if (!app0 || !*app0) {
        SetDetail(detail, detail_size, "no game folder");
        return -1;
    }
    const std::filesystem::path archive = aln::PathFromUtf8(app0) / "stream.dat";
    std::error_code ec;
    const uint64_t stream_size = std::filesystem::file_size(archive, ec);
    if (ec) {
        SetDetail(detail, detail_size, "stream.dat cannot be read");
        return -1;
    }

    size_t count = 0;
    const AlnControllerPromptPatch *table = AlnControllerPromptPatches(&count);
    std::vector<const AlnControllerPromptPatch *> selected;
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(table[i].build, build_key) == 0) {
            selected.push_back(&table[i]);
        }
    }
    if (selected.empty()) {
        SetDetail(detail, detail_size, std::string("no Xbox assets for ") + build_key);
        return -1;
    }

    size_t scene_count = 0;
    const AlnControllerScenePatch *scene_table = AlnControllerScenePatches(&scene_count);
    std::vector<const AlnControllerScenePatch *> selected_scenes;
    for (size_t i = 0; i < scene_count; ++i) {
        if (std::strcmp(scene_table[i].build, build_key) == 0) {
            selected_scenes.push_back(&scene_table[i]);
        }
    }

    std::ifstream file(archive, std::ios::binary);
    if (!file) {
        SetDetail(detail, detail_size, "stream.dat cannot be opened");
        return -1;
    }
    for (const AlnControllerPromptPatch *patch : selected) {
        if (!AlnPromptPatchFits(patch, stream_size)) {
            SetDetail(detail, detail_size, std::string("bounds do not fit: ") + patch->name);
            return -1;
        }
        BlockHeader header{};
        if (!ReadAt(file, patch->offset, &header, sizeof(header))) {
            SetDetail(detail, detail_size, std::string("cannot read the block: ") + patch->name);
            return -1;
        }
        if (header.id0 != header.id1 || header.type != 0x54455820u ||
            header.zsize != patch->original_zsize || header.dsize != patch->original_dsize ||
            header.units != patch->block_units) {
            SetDetail(detail, detail_size, std::string("unexpected block: ") + patch->name);
            return -1;
        }
        std::vector<unsigned char> original(patch->original_zsize);
        if (!ReadAt(file, patch->offset + 40, original.data(), original.size())) {
            SetDetail(detail, detail_size, std::string("cannot read the payload: ") + patch->name);
            return -1;
        }
        if (Crc32(original.data(), original.size()) != patch->original_crc32) {
            SetDetail(detail, detail_size, std::string("asset identity mismatch: ") + patch->name);
            return -1;
        }
    }
    for (const AlnControllerScenePatch *patch : selected_scenes) {
        if (!AlnScenePatchFits(patch, stream_size)) {
            SetDetail(detail, detail_size, std::string("scene bounds do not fit: ") + patch->name);
            return -1;
        }
        BlockHeader header{};
        if (!ReadAt(file, patch->offset, &header, sizeof(header))) {
            SetDetail(detail, detail_size, std::string("cannot read the scene: ") + patch->name);
            return -1;
        }
        if (header.id0 != header.id1 || (header.type != kSceneType && header.type != kScriptType) ||
            header.zsize != patch->original_zsize || header.dsize != patch->original_dsize ||
            header.units != patch->block_units) {
            SetDetail(detail, detail_size, std::string("unexpected scene: ") + patch->name);
            return -1;
        }
        std::vector<unsigned char> original(patch->original_zsize);
        if (!ReadAt(file, patch->offset + patch->payload_offset, original.data(), original.size())) {
            SetDetail(detail, detail_size, std::string("cannot read the scene payload: ") + patch->name);
            return -1;
        }
        if (Crc32(original.data(), original.size()) != patch->original_crc32) {
            SetDetail(detail, detail_size, std::string("scene identity mismatch: ") + patch->name);
            return -1;
        }
    }

    std::vector<RuntimeFilePatch> patches;
    patches.reserve(selected.size() * 3 + selected_scenes.size() * 2 + 16);
    for (const AlnControllerPromptPatch *patch : selected) {
        patches.push_back({patch->offset + 4, 4, &patch->zsize});
        patches.push_back({patch->offset + 8, 4, &patch->dsize});
        patches.push_back({patch->offset + 40, patch->zsize, patch->data});
    }
    for (const AlnControllerScenePatch *patch : selected_scenes) {
        patches.push_back({patch->offset + 4, 4, &patch->zsize});
        patches.push_back({patch->offset + patch->payload_offset, patch->zsize, patch->data});
    }
    std::string glyph_detail;
    const bool glyph_patched = BuildGlyphPatches(file, build_key, stream_size, patches, glyph_detail);
    if (runtime_file_overlay("/app0/stream.dat", stream_size, patches.data(), patches.size()) != 0) {
        SetDetail(detail, detail_size, "the file overlay was refused");
        return -1;
    }
    std::string message = "Xbox (" + std::to_string(selected.size()) + " resources";
    if (!selected_scenes.empty()) {
        message += ", " + std::to_string(selected_scenes.size()) +
                   (selected_scenes.size() == 1 ? " callout scene" : " callout scenes");
    }
    message += glyph_patched ? ", " + glyph_detail : ", glyph atlases unchanged: " + glyph_detail;
    message += ")";
    SetDetail(detail, detail_size, message);
    return 0;
}
