// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"
#include "video_core/cache_request_queue.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

/// aln: the driver pipeline binaries of the running game; one folder for every build of a title.
std::filesystem::path PipelineBinaryDirectory();

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    /// Finishes the queued writes for a bounded time, then drops the rest: they are optional.
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// aln: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();
    /// aln: true when the store folder holds no file.
    [[nodiscard]] bool IsEmpty() const;

    /// aln: blobs are sealed (size and checksum) when saved; Load and ForEachBlob hand out
    /// only blobs that pass, and discard the others.
    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);
    /// aln: queued behind the pending saves, so it removes what they wrote too.
    void Remove(BlobType type, const std::string& name);

    using BlobVisitor = std::function<void(std::vector<u8>&& data, const std::string& name)>;
    /// aln: `stop` returning true ends the walk early (background warm-up at exit).
    void ForEachBlob(BlobType type, const BlobVisitor& func,
                     const std::function<bool()>& stop = {});
    /// aln: blobs are sealed files written beside and renamed into place, so other threads
    /// may read them while the IO thread saves (the zip archive cannot).
    [[nodiscard]] bool ConcurrentReads() const;
    /// aln: number of blobs ForEachBlob visits (the loading screen's total).
    size_t CountBlobs(BlobType type);

    /// aln: a stored entry that cannot be used (failed its seal or its format). One warning, the rest counted.
    void ReportCorrupt(std::string_view what);

private:
    template <typename T>
    void LoadBlob(BlobType type, const std::string& name, std::vector<T>& data);
    void DiscardIfCorrupt(const std::filesystem::path& path);

    std::jthread io_worker{};
    std::shared_ptr<RequestQueue> queue{};
    std::filesystem::path cache_path{};
    std::atomic<u32> corrupt_entries{0};
    bool opened{};
};

} // namespace Storage
