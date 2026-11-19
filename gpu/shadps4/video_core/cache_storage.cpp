// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_files.h"
#include "video_core/cache_storage.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>

namespace {

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

constexpr auto CloseDrainLimit = std::chrono::milliseconds(1500);

std::vector<u8> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const auto size = stream.tellg();
    if (size <= 0 ||
        size > std::streamoff(PipelineCachePolicy::MaxBlobBytes + PipelineCachePolicy::SealSize)) {
        return {};
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) {
        return {};
    }
    return bytes;
}

} // namespace

namespace Storage {

std::filesystem::path PipelineBinaryDirectory() {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           Common::ElfInfo::Instance().GameSerial() / "pipeline-binaries";
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();
    const auto folder =
        PipelineCachePolicy::CacheFolderName(game_info.GameSerial(), game_info.AppVer());

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) / std::filesystem::path{folder}.concat(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / folder;
        std::error_code error;
        std::filesystem::create_directories(cache_path, error);

        const auto unfinished = PipelineCacheFiles::RemoveLeftoverTemps(cache_path) +
                                PipelineCacheFiles::RemoveLeftoverTemps(PipelineBinaryDirectory());
        if (unfinished) {
            LOG_INFO(Render, "Pipeline cache: removed {} unfinished writes", unfinished);
        }

        // Earlier versions kept the store in the serial's own folder, unsealed and unreadable now.
        const auto older = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (older != cache_path) {
            const auto removed =
                PipelineCacheFiles::RemoveFiles(older, PipelineCacheFiles::IsStoreFile);
            if (removed) {
                LOG_INFO(Render, "Pipeline cache: removed {} files of an older cache layout",
                         removed);
            }
        }
    }

    queue = std::make_shared<RequestQueue>();
    io_worker = std::jthread{[worker_queue = queue](std::stop_token stoken) {
        Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
        worker_queue->Run(stoken);
    }};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    queue->Close(CloseDrainLimit);
    io_worker.join();
    if (const auto dropped = queue->Dropped()) {
        LOG_INFO(Render, "Pipeline cache: {} queued writes were not finished", dropped);
    }

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

namespace {

template <typename T>
std::vector<u8> SealedBytes(std::vector<T>&& v) {
    std::vector<u8> bytes;
    if constexpr (std::is_same_v<T, u8>) {
        bytes = std::move(v);
    } else {
        bytes.resize(v.size() * sizeof(T));
        std::memcpy(bytes.data(), v.data(), bytes.size());
    }
    PipelineCachePolicy::Seal(bytes);
    return bytes;
}

void WriteBlob(BlobType type, std::filesystem::path path, const std::vector<u8>& bytes) {
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        ASSERT_MSG(!ar_is_read_only,
                   "The archive is read-only. Did you forget to call `FinishPreload`?");
        if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), bytes.data(), bytes.size(),
                                   MZ_BEST_COMPRESSION)) {
            LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
        }
    } else if (!PipelineCacheFiles::WriteFileAtomically(path, bytes)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            LOG_WARNING(Render, "Pipeline cache: could not write {}; further failures are silent",
                        path.filename().string());
        }
    }
}

template <typename T>
bool WriteVector(RequestQueue& queue, const BlobType type, std::filesystem::path&& path,
                 std::vector<T>&& v) {
    if (v.size() * sizeof(T) > PipelineCachePolicy::MaxBlobBytes) {
        return false;
    }
    return queue.Submit([type, path = std::move(path), bytes = SealedBytes(std::move(v))] {
        WriteBlob(type, path, bytes);
    });
}

} // namespace

template <typename T>
void DataBase::LoadBlob(BlobType type, const std::string& name, std::vector<T>& v) {
    v.clear();
    if (!opened) {
        return;
    }
    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    std::vector<u8> bytes;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const int index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip_ar, index, &stat) ||
            stat.m_uncomp_size > PipelineCachePolicy::MaxBlobBytes + PipelineCachePolicy::SealSize) {
            ReportCorrupt("archive entry");
            return;
        }
        bytes.resize(stat.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip_ar, index, bytes.data(), bytes.size(), 0)) {
            bytes.clear();
        }
    } else {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            return;
        }
        bytes = ReadFileBytes(path);
    }
    if (!PipelineCachePolicy::Unseal(bytes) || bytes.size() % sizeof(T) != 0) {
        ReportCorrupt("blob");
        DiscardIfCorrupt(path);
        return;
    }
    v.resize(bytes.size() / sizeof(T));
    std::memcpy(v.data(), bytes.data(), bytes.size());
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(*queue, type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(*queue, type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    LoadBlob(type, name, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    LoadBlob(type, name, data);
}

void DataBase::Remove(BlobType type, const std::string& name) {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    auto path = cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    queue->Submit([path] {
        std::error_code error;
        std::filesystem::remove(path, error);
    });
}

void DataBase::DiscardIfCorrupt(const std::filesystem::path& path) {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    // Checked again where the queue runs: a writer may have replaced the file meanwhile.
    queue->Submit([path] {
        auto bytes = ReadFileBytes(path);
        if (!PipelineCachePolicy::Unseal(bytes)) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    });
}

void DataBase::ReportCorrupt(std::string_view what) {
    if (corrupt_entries.fetch_add(1) == 0) {
        LOG_WARNING(Render, "Pipeline cache: discarded a corrupt {}; it is rebuilt when needed",
                    what);
    }
}

size_t DataBase::CountBlobs(BlobType type) {
    const auto& ext = GetBlobFileExtension(type);
    size_t count = 0;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            count += std::string{file_name.data()}.ends_with(ext);
        }
        return count;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it{cache_path, error}, end; !error && it != end;
         it.increment(error)) {
        count += it->path().extension().string().ends_with(ext);
    }
    return count;
}

bool DataBase::ConcurrentReads() const {
    return !EmulatorSettings.IsPipelineCacheArchived();
}

void DataBase::ForEachBlob(BlobType type, const BlobVisitor& func,
                           const std::function<bool()>& stop) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            const std::filesystem::path entry{file_name.data()};
            if (!entry.extension().string().ends_with(ext)) {
                continue;
            }
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip_ar, index, &stat) ||
                stat.m_uncomp_size >
                    PipelineCachePolicy::MaxBlobBytes + PipelineCachePolicy::SealSize) {
                ReportCorrupt("archive entry");
                continue;
            }
            std::vector<u8> data(stat.m_uncomp_size);
            if (mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0) &&
                PipelineCachePolicy::Unseal(data)) {
                func(std::move(data), entry.stem().string());
            } else {
                ReportCorrupt("archive entry");
            }
        }
        return;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it{cache_path, error}, end; !error && it != end;
         it.increment(error)) {
        if (stop && stop()) {
            return;
        }
        const auto path = it->path();
        if (!path.extension().string().ends_with(ext)) {
            continue;
        }
        auto data = ReadFileBytes(path);
        if (PipelineCachePolicy::Unseal(data)) {
            func(std::move(data), path.stem().string());
        } else {
            ReportCorrupt("blob");
            DiscardIfCorrupt(path);
        }
    }
}

void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    const auto removed = PipelineCacheFiles::RemoveFiles(cache_path, [](const auto&) { return true; });
    if (removed) {
        LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt", removed);
    }
}

bool DataBase::IsEmpty() const {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        return mz_zip_reader_get_num_files(&zip_ar) == 0;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it{cache_path, error}, end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error)) {
            return false;
        }
    }
    return true;
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
    if (const auto corrupt = corrupt_entries.exchange(0); corrupt > 1) {
        LOG_WARNING(Render, "Pipeline cache: {} corrupt entries discarded in all", corrupt);
    }
}

} // namespace Storage
