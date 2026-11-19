// SPDX-License-Identifier: GPL-2.0-or-later
// aln: how the persistent pipeline cache puts files on disk: written beside the target and moved
// into place, so a reader sees the previous complete file or the new one, never a partial one.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace PipelineCacheFiles {

namespace fs = std::filesystem;

/// A temporary file this old belongs to a writer that no longer exists.
constexpr std::chrono::seconds OrphanAge{60};

inline bool WriteWhole(const fs::path& file, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    stream.close();
    return !stream.fail();
}

/// Replaces `to` if it exists. Windows refuses while a reader or a scanner has `to` open, which
/// lasts moments, so those errors are retried.
inline bool ReplaceFile(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        const auto error = GetLastError();
        const bool transient = error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
                               error == ERROR_LOCK_VIOLATION;
        if (!transient || attempt == 3) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5 << attempt));
    }
#else
    std::error_code error;
    fs::rename(from, to, error);
    return !error;
#endif
}

struct FileOps {
    bool (*write)(const fs::path&, std::span<const std::uint8_t>) = &WriteWhole;
    bool (*replace)(const fs::path&, const fs::path&) = &ReplaceFile;
};

inline bool IsTempFile(const fs::path& path) {
    return path.extension() == ".tmp";
}

/// Unique per process and per call, so writers of one target never share a temporary file.
inline fs::path TempPathFor(const fs::path& file) {
    static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
    const auto process = std::uint64_t(GetCurrentProcessId());
#else
    const auto process = std::uint64_t(getpid());
#endif
    auto temp = file;
    temp += "." + std::to_string(process) + "." + std::to_string(sequence++) + ".tmp";
    return temp;
}

/// True when `file` now holds exactly `bytes`. Otherwise `file` is as it was and no temporary
/// file is left behind.
inline bool WriteFileAtomically(const fs::path& file, std::span<const std::uint8_t> bytes,
                                const FileOps& ops = {}) {
    const auto temp = TempPathFor(file);
    if (ops.write(temp, bytes) && ops.replace(temp, file)) {
        return true;
    }
    std::error_code error;
    fs::remove(temp, error);
    return false;
}

inline bool IsStoreFile(const fs::path& path) {
    const auto extension = path.extension();
    return extension == ".meta" || extension == ".spv" || extension == ".key" ||
           extension == ".bin" || IsTempFile(path);
}

/// Removes the regular files of `directory` that `selected` accepts; folders, and so the driver
/// pipeline binaries inside one, stay. Returns how many.
template <typename Selected>
unsigned RemoveFiles(const fs::path& directory, Selected&& selected) {
    unsigned removed = 0;
    std::error_code error;
    for (fs::directory_iterator it{directory, error}, end; !error && it != end;
         it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && selected(it->path()) &&
            fs::remove(it->path(), entry_error)) {
            ++removed;
        }
    }
    return removed;
}

/// Removes the unfinished writes of earlier sessions; returns how many.
inline unsigned RemoveLeftoverTemps(const fs::path& directory,
                                    std::chrono::seconds min_age = OrphanAge) {
    unsigned removed = 0;
    std::error_code error;
    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it{directory, error}, end; !error && it != end;
         it.increment(error)) {
        std::error_code entry_error;
        if (!IsTempFile(it->path()) || !it->is_regular_file(entry_error)) {
            continue;
        }
        const auto written = it->last_write_time(entry_error);
        if (!entry_error && now - written >= min_age && fs::remove(it->path(), entry_error)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace PipelineCacheFiles
