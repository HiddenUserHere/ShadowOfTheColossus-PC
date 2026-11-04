// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    /// sotc: the shader variant this session translates for (flags that change the SPIR-V or the profile). Set
    /// before Open: a folder holds one variant, and another variant gets a folder of its own instead of
    /// overwriting the binaries or resetting the cache (~800 shaders translated again on every flag switch).
    void SetVariant(std::string key);
    void Open();
    void Close();
    /// sotc: the writes queued for the IO worker and not done yet (the boot hold waits for them).
    [[nodiscard]] u32 PendingWrites() const noexcept;
    /// sotc: the folder of this variant's files (SOTC_PRETRANSLATE keeps its pack there).
    [[nodiscard]] const std::filesystem::path& CachePath() const noexcept {
        return cache_path;
    }
    void Clear();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// Sets the current cache aside (renamed with ".incompatible") and opens an empty one.
    void Reset();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    /// sotc: ForEachBlob that also passes each blob name, without the extension (as Save).
    void ForEachNamedBlob(
        BlobType type,
        const std::function<void(std::vector<u8>&& data, const std::string& name)>& func);
    /// sotc: how many blobs of a type there are (the loading screen's progress).
    size_t CountBlobs(BlobType type);

private:
    void Prefetch();
    bool TakePrefetched(const std::filesystem::path& path, std::vector<u8>& data);
    /// sotc: after FinishPreload, Load of a file this never saw at Prefetch or Save time returns
    /// nothing without touching the disk: every new shader's collision check opened a missing
    /// file on the command processor, ~28 ms each on Windows. Built from the same directory
    /// listing Prefetch already does, kept current by Save/Clear.
    bool MayExist(const std::filesystem::path& path);
    void AddKnown(const std::filesystem::path& path);

    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::string variant_key{}; ///< sotc: SetVariant
    bool opened{};
    /// sotc: every loose cache file read up front (in parallel) for the boot preload, by file
    /// name; ordered like an NTFS directory listing. Emptied by FinishPreload.
    std::map<std::string, std::vector<u8>> prefetched{};
    bool prefetch_active{};
    std::unordered_set<std::string> known_files{};
    std::mutex known_mutex{};
    bool known_valid{};
};

} // namespace Storage
