// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <type_traits>
#include "../../sotc/block_ledger.h" // sotc: SOTC_BLOCK_LEDGER

namespace {

std::mutex submit_mutex{};
// sotc: atomic. The writer increments under submit_mutex but the IO thread decrements without
// it; a lost decrement left the IO loop spinning on an empty queue (one core) for the session.
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

// sotc: the test switches that change how shaders are translated, which the cache keys do not
// see: with any of them set the cache goes to a folder of its own ("_t" and a hash of their
// values), so switching a test on or off never reuses shaders of the other setting and needs no
// "Clear shader cache".
std::string ShaderTestTag() {
    static constexpr const char* switches[] = {
        "SOTC_NAN_GUARD", "SOTC_AMD_AS_NV", "SOTC_MANUAL_INTERP", "SOTC_NO_EXPLICIT_VERTEX",
        "SOTC_NO_RTZ", "SOTC_CLIP_EMU", "SOTC_WAVE64_UNIFORM", "SOTC_WAVE64_IDENTITY",
        "SOTC_LDS_BARRIERS", "SOTC_LDS_SMALL", "SOTC_AMD64_LDS", "SHADPS4_IEEE_MINMAX", "SOTC_WQM_COPY"};
    u64 hash = 1469598103934665603ULL;
    bool any = false;
    for (const char* name : switches) {
        const char* value = std::getenv(name);
        if (!value) {
            continue;
        }
        any = true;
        for (const char* p : {name, "=", value, ";"}) {
            for (; *p; ++p) {
                hash = (hash ^ u8(*p)) * 1099511628211ULL;
            }
        }
    }
    if (!any) {
        return {};
    }
    char tag[24];
    std::snprintf(tag, sizeof(tag), "_t%08x", static_cast<u32>(hash ^ (hash >> 32)));
    return tag;
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests.load() != 0; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
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

void DataBase::SetVariant(std::string key) {
    variant_key = std::move(key);
}

// sotc: the folder of a shader variant. `base/variant.key` names the variant a folder holds. A folder without it
// (new, or written before the variants) takes the current one; another variant goes to `base_v<hash>`.
static std::filesystem::path VariantFolder(const std::filesystem::path& base, const std::string& key) {
    const auto held_key = [](const std::filesystem::path& dir) {
        std::string held;
        if (std::FILE* f = std::fopen((dir / "variant.key").string().c_str(), "rb")) {
            char buf[512];
            const size_t n = std::fread(buf, 1, sizeof buf, f);
            std::fclose(f);
            held.assign(buf, n);
        }
        return held;
    };
    const auto claim = [&key](const std::filesystem::path& dir) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (std::FILE* f = std::fopen((dir / "variant.key").string().c_str(), "wb")) {
            std::fwrite(key.data(), 1, key.size(), f);
            std::fclose(f);
        }
    };
    const std::string held = held_key(base);
    if (held == key) {
        return base;
    }
    if (held.empty()) {
        claim(base);
        return base;
    }
    u64 hash = 0xcbf29ce484222325ull; // FNV-1a
    for (const char c : key) {
        hash = (hash ^ static_cast<u8>(c)) * 0x100000001b3ull;
    }
    auto path = base;
    path += fmt::format("_v{:08x}", static_cast<u32>(hash ^ (hash >> 32)));
    if (held_key(path) != key) {
        claim(path);
    }
    return path;
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        // sotc: shaders compiled with and without direct memory access differ, and the cache keys
        // do not tell them apart: the launcher's DMA switch gets its own cache folder. The same
        // for SOTC_NEO: a cache filled in PS4 Pro mode gave a black picture in the normal mode.
        cache_path = GetUserPath(PathType::CacheDir) /
                     (std::string{game_info.GameSerial()} +
                      (EmulatorSettings.IsDirectMemoryAccessEnabled() ? "" : "_nodma") +
                      (EmulatorSettings.IsNoInt64() ? "_noi64" : "") +
                      (Libraries::Kernel::sceKernelIsNeoMode() ? "_neo" : "") + ShaderTestTag());
        if (!variant_key.empty()) {
            cache_path = VariantFolder(cache_path, variant_key);
        }
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        Prefetch();
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

// sotc: all cache files in one file (cache.sotcpack in the cache folder), read in one go. On
// a hard disk the ~12k small files took 30-100 s whenever Windows had dropped them from its file
// cache (the game's 13.7 GB of archives read into memory push them out): the loading screen
// changed between 1 s and 100 s from one start to the next. The loose files stay the cache
// (written as before, whole or not at all); the pack is a copy, rewritten in the background
// when files were missing from it. Format: "SOTCPK1" + NUL, u32 count, then per file u16 name
// length, name, u32 size, data. SOTC_CACHE_PACK=0 turns it off.
static bool CachePackEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_CACHE_PACK");
        return !v || v[0] != '0';
    }();
    return enabled;
}

static constexpr char PackMagic[8] = {'S', 'O', 'T', 'C', 'P', 'K', '1', '\0'};

// sotc: SOTC_PREFETCH_LEAN (on unless set to 0). The cache prefetch holds no whole-file copies: the pack is read entry
// by entry (no 1.3 GB buffer next to the entries), its background rewrite shares the prefetched bytes instead of a
// copy of all of them, and it runs only when an entry's content changed (size and hash, not the file time: identical
// pipeline keys saved again made ~3,000 files "newer than the pack" at every launch). Save skips a blob whose bytes are
// on disk already, and FinishPreload drops the entries the boot preload consumed. The files the cache folder holds
// for their own readers (sotc_pack_libraries.bin, sotc_recent_pso.bin) are not prefetched. 0: the former path.
static bool PrefetchLean() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PREFETCH_LEAN");
        return !v || v[0] != '0';
    }();
    return on;
}

// sotc: SOTC_PREFETCH_LEAN: the content signature of a blob (its size is the hash seed).
static u64 BlobSig(const u8* data, size_t size) {
    return XXH3_64bits_withSeed(data, size, static_cast<u64>(size));
}

// sotc: SOTC_PREFETCH_LEAN: written into the cache folder by their own code, read by it (never through Load).
static bool OwnReaderFile(const std::string& name) {
    return name == "sotc_pack_libraries.bin" || name == "sotc_recent_pso.bin";
}

static void ReadCachePack(const std::filesystem::path& path,
                          std::map<std::string, std::vector<u8>>& out) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        return;
    }
    if (PrefetchLean()) {
        // sotc: SOTC_PREFETCH_LEAN: each entry is read into its own vector (no whole-file buffer copied out).
        std::setvbuf(f, nullptr, _IOFBF, 1u << 20);
        char magic[8]{};
        u32 count = 0;
        if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, PackMagic, 8) != 0 ||
            std::fread(&count, 4, 1, f) != 1) {
            std::fclose(f);
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            u16 name_len = 0;
            u32 data_len = 0;
            std::string name;
            if (std::fread(&name_len, 2, 1, f) != 1) {
                break;
            }
            name.resize(name_len);
            if ((name_len != 0 && std::fread(name.data(), 1, name_len, f) != name_len) ||
                std::fread(&data_len, 4, 1, f) != 1) {
                break;
            }
            const auto ext = std::filesystem::path{name}.extension();
            if (ext == ".pack" || OwnReaderFile(name)) {
                // An entry no reader takes from here (an older pack held the 1.1 GB pretranslate pack).
                if (_fseeki64(f, static_cast<s64>(data_len), SEEK_CUR) != 0) {
                    break;
                }
                continue;
            }
            std::vector<u8> data(data_len);
            if (data_len != 0 && std::fread(data.data(), 1, data_len, f) != data_len) {
                break;
            }
            out.emplace(std::move(name), std::move(data));
        }
        std::fclose(f);
        return;
    }
    std::vector<u8> all;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 12) {
        all.resize(static_cast<size_t>(size));
        if (std::fread(all.data(), 1, all.size(), f) != all.size()) {
            all.clear();
        }
    }
    std::fclose(f);
    if (all.size() < 12 || std::memcmp(all.data(), PackMagic, 8) != 0) {
        return;
    }
    size_t pos = 8;
    const auto take = [&](void* dst, size_t n) {
        if (pos + n > all.size()) {
            return false;
        }
        std::memcpy(dst, all.data() + pos, n);
        pos += n;
        return true;
    };
    u32 count = 0;
    take(&count, 4);
    for (u32 i = 0; i < count; ++i) {
        u16 name_len = 0;
        u32 data_len = 0;
        std::string name;
        if (!take(&name_len, 2) || pos + name_len > all.size()) {
            break;
        }
        name.assign(reinterpret_cast<const char*>(all.data() + pos), name_len);
        pos += name_len;
        if (!take(&data_len, 4) || pos + data_len > all.size()) {
            break;
        }
        out.emplace(std::move(name),
                    std::vector<u8>(all.begin() + pos, all.begin() + pos + data_len));
        pos += data_len;
    }
}

// sotc: SOTC_PREFETCH_LEAN passes shared references to the prefetched bytes (Bytes = shared_ptr), the former path a copy.
template <typename Bytes>
static const std::vector<u8>& PackBytes(const Bytes& b) {
    if constexpr (std::is_same_v<Bytes, std::vector<u8>>) {
        return b;
    } else {
        return *b;
    }
}

template <typename Bytes>
static void WriteCachePack(std::filesystem::path path, std::vector<std::pair<std::string, Bytes>> files) {
    auto tmp = path;
    tmp += L".tmp";
    std::FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        return;
    }
    bool ok = std::fwrite(PackMagic, 1, 8, f) == 8;
    const u32 count = static_cast<u32>(files.size());
    ok = ok && std::fwrite(&count, 4, 1, f) == 1;
    for (const auto& [name, bytes] : files) {
        const std::vector<u8>& data = PackBytes(bytes);
        const u16 name_len = static_cast<u16>(name.size());
        const u32 data_len = static_cast<u32>(data.size());
        ok = ok && std::fwrite(&name_len, 2, 1, f) == 1 &&
             std::fwrite(name.data(), 1, name.size(), f) == name.size() &&
             std::fwrite(&data_len, 4, 1, f) == 1 &&
             (data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size());
    }
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
    }
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
    }
}

// sotc: the boot preload opened ~12k small files one by one (~21 s of a 22 s warm-up on Windows,
// where each open is slow and scanned). Read them all in parallel first; Load and ForEachBlob
// take them from memory until FinishPreload.
void DataBase::Prefetch() {
    const auto start = std::chrono::steady_clock::now();
    const bool lean = PrefetchLean();
    {
        std::scoped_lock lock{known_mutex};
        dropped_sigs.clear();
    }
    const auto pack_path = cache_path / "cache.sotcpack";
    std::map<std::string, std::vector<u8>> packed;
    if (CachePackEnabled()) {
        ReadCachePack(pack_path, packed);
    }
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    size_t in_pack = 0, on_disk = 0, newer = 0;
    // sotc: SOTC_PREFETCH_LEAN: the signature of the pack's copy of each newer file, compared with the loose file's
    // bytes below: only a different content (or an added or removed file) rewrites the pack.
    std::unordered_map<std::string, u64> newer_sigs;
    // sotc: a loose file written after the pack (a shader translated again in a later session,
    // e.g. after a ShaderBinaryVersion change) holds the current data: it wins over the pack's
    // copy, and the pack is rewritten with it below. Taking the pack's copy left those entries
    // stale for good (1849 pipelines refused at every boot, ~2100 stages translated again and
    // ~6900 draws skipped per session; fix by HiddenUserHere, upstream PR #339).
    std::error_code time_ec;
    const auto pack_time = std::filesystem::last_write_time(pack_path, time_ec);
    const bool have_pack_time = !time_ec && !packed.empty();
    const auto keep = [this](const std::string& name, std::shared_ptr<const std::vector<u8>> data) {
        prefetched.try_emplace(name).first->second.data = std::move(data);
    };
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        const auto ext = entry.path().extension();
        // sotc: ".pack" is the pretranslate pack (sotc_pretranslate.pack, ~1.1 GB): its readers open the
        // file themselves (PrePackPath), so a prefetched copy only held 1.1 GB of RAM for the session.
        if (!entry.is_regular_file(ec) || ext == ".tmp" || ext == ".sotcpack" || ext == ".pack" ||
            (lean && OwnReaderFile(entry.path().filename().string()))) {
            continue;
        }
        ++on_disk;
        auto it = packed.find(entry.path().filename().string());
        if (it != packed.end() && have_pack_time) {
            std::error_code entry_ec;
            const auto entry_time = entry.last_write_time(entry_ec);
            if (!entry_ec && entry_time > pack_time) {
                if (lean) {
                    newer_sigs.emplace(it->first, BlobSig(it->second.data(), it->second.size()));
                }
                packed.erase(it);
                it = packed.end();
                ++newer;
            }
        }
        if (it != packed.end()) {
            ++in_pack;
            keep(it->first, std::make_shared<const std::vector<u8>>(std::move(it->second)));
            packed.erase(it);
        } else {
            files.push_back(entry.path());
        }
    }
    // Pack entries whose loose file is gone (a cleared cache) are dropped with `packed`.
    const size_t gone = packed.size();
    packed.clear();
    {
        // sotc: the same listing, kept for MayExist once prefetch_active ends below.
        std::scoped_lock lock{known_mutex};
        known_files.clear();
        for (const auto& file : files) {
            known_files.insert(file.filename().string());
        }
        for (const auto& [name, entry] : prefetched) { // taken from the pack, on disk too
            known_files.insert(name);
        }
        known_valid = !ec;
    }
    const u32 num_threads =
        std::clamp<u32>(std::thread::hardware_concurrency(), 1, 16);
    std::vector<std::vector<std::pair<std::string, std::vector<u8>>>> results(num_threads);
    std::atomic<size_t> next{0};
    {
        std::vector<std::jthread> workers;
        for (u32 t = 0; t < num_threads; ++t) {
            workers.emplace_back([&, t] {
                using namespace Common::FS;
                for (size_t i = next++; i < files.size(); i = next++) {
                    const IOFile file{files[i], FileAccessMode::Read};
                    if (!file.IsOpen()) {
                        continue;
                    }
                    std::vector<u8> data(file.GetSize());
                    if (file.Read(data) != data.size()) {
                        continue;
                    }
                    results[t].emplace_back(files[i].filename().string(), std::move(data));
                }
            });
        }
    }
    u64 bytes = 0;
    size_t added = 0, changed = 0, same = 0;
    for (auto& list : results) {
        for (auto& [name, data] : list) {
            bytes += data.size();
            if (lean) {
                const auto sig = newer_sigs.find(name);
                if (sig == newer_sigs.end()) {
                    ++added;
                } else if (sig->second != BlobSig(data.data(), data.size())) {
                    ++changed;
                } else {
                    ++same;
                }
            }
            keep(name, std::make_shared<const std::vector<u8>>(std::move(data)));
        }
    }
    prefetch_active = true;
    // sotc: SOTC_PREFETCH_LEAN: a file that could not be read (gone meanwhile) counts as a change.
    const size_t unread = files.size() - std::min(files.size(), added + changed + same);
    const bool rewrite = lean ? (added + changed + gone + unread) != 0 : (!files.empty() || gone != 0);
    if (CachePackEnabled() && rewrite) {
        if (lean) {
            std::vector<std::pair<std::string, std::shared_ptr<const std::vector<u8>>>> refs;
            refs.reserve(prefetched.size());
            for (const auto& [name, entry] : prefetched) {
                refs.emplace_back(name, entry.data);
            }
            std::thread{WriteCachePack<std::shared_ptr<const std::vector<u8>>>, pack_path, std::move(refs)}.detach();
        } else {
            std::vector<std::pair<std::string, std::vector<u8>>> copy;
            copy.reserve(prefetched.size());
            for (const auto& [name, entry] : prefetched) {
                copy.emplace_back(name, *entry.data);
            }
            std::thread{WriteCachePack<std::vector<u8>>, pack_path, std::move(copy)}.detach();
        }
    } else if (CachePackEnabled() && lean && newer != 0) {
        // sotc: SOTC_PREFETCH_LEAN: the newer files hold the pack's bytes: the pack's time moves past them, so the
        // next launch takes them from the pack again instead of reading them one by one.
        std::error_code touch_ec;
        std::filesystem::last_write_time(pack_path, std::filesystem::file_time_type::clock::now(), touch_ec);
    }
    std::fprintf(stderr,
                 "SOTC shader cache: %zu files, %zu from the pack, %zu read one by one (%zu newer "
                 "than the pack)\n",
                 on_disk, in_pack, files.size(), newer);
    if (lean) {
        std::fprintf(stderr,
                     "SOTC PREFETCH LEAN: pack %s (added %zu, changed %zu, same bytes %zu, removed %zu)\n",
                     !CachePackEnabled() ? "off" : rewrite ? "rewritten in the background (shared bytes, no copy)"
                                                           : "unchanged, not rewritten",
                     added, changed, same, gone + unread);
    }
    LOG_INFO(Render, "Prefetched {} cache files ({} MiB) in {} ms with {} threads",
             prefetched.size(), bytes >> 20,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count(),
             num_threads);
}

bool DataBase::TakePrefetched(const std::filesystem::path& path, std::vector<u8>& data) {
    if (!prefetch_active) {
        return false;
    }
    const auto it = prefetched.find(path.filename().string());
    if (it == prefetched.end()) {
        // Not on disk at prefetch time: the same answer the file system would give.
        data.clear();
        return true;
    }
    // Copied, not moved: a shader's .meta and .spv are loaded once per pipeline that uses it.
    // sotc: SOTC_PREFETCH_LEAN: marked consumed, so FinishPreload may drop it.
    data = *it->second.data;
    it->second.consumed.store(true, std::memory_order_relaxed);
    return true;
}

bool DataBase::SameOnDisk(const std::string& file_name, const std::vector<u8>& bytes) {
    if (!PrefetchLean()) {
        return false;
    }
    std::scoped_lock lock{known_mutex}; // FinishPreload drops entries under it
    if (const auto it = prefetched.find(file_name); it != prefetched.end()) {
        return *it->second.data == bytes;
    }
    const auto sig = dropped_sigs.find(file_name);
    if (sig == dropped_sigs.end()) {
        return false;
    }
    if (sig->second == BlobSig(bytes.data(), bytes.size())) {
        return true;
    }
    dropped_sigs.erase(sig); // written below with other bytes
    return false;
}

u32 DataBase::PendingWrites() const noexcept {
    return num_requests.load(std::memory_order_relaxed);
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();
    // sotc: the worker leaves when asked to stop, with writes still queued: pipelines compiled
    // shortly before the game was closed were dropped and compiled again (a stutter) next time.
    {
        std::scoped_lock lock{m_request};
        while (!req_queue.empty()) {
            auto request = std::move(req_queue.front());
            req_queue.pop();
            if (request.valid()) {
                request();
            }
        }
        num_requests = 0;
    }

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

// sotc: restored from bbport. Deletes the loose cache files so an incompatible cache (another
// GPU, a driver or build change) is rebuilt instead of being ignored by every later session.
void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    prefetched.clear();
    {
        std::scoped_lock lock{known_mutex};
        known_files.clear();
        dropped_sigs.clear(); // sotc: SOTC_PREFETCH_LEAN: nothing is on disk any more
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        // sotc: the blob is moved into the request (it was copied, on the caller's thread: the command processor for
        // most saves, SOTC_BLOCK_LEDGER "storage save").
        auto request = std::packaged_task<void()>{[type, path_ = std::move(path_), v = std::move(v)]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                // sotc: written whole or not at all. The game process is often ended (launcher
                // Stop, a crash) while blobs are being written, and a truncated blob from the
                // last session is read back at the next start.
                auto temporary = path;
                temporary += ".tmp";
                {
                    const auto file = IOFile{temporary, FileAccessMode::Create};
                    file.Write(v);
                }
                std::error_code error;
                std::filesystem::rename(temporary, path, error);
                if (error) {
                    std::filesystem::remove(temporary, error);
                }
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::MayExist(const std::filesystem::path& path) {
    std::scoped_lock lock{known_mutex};
    return !known_valid || known_files.contains(path.filename().string());
}

void DataBase::AddKnown(const std::filesystem::path& path) {
    std::scoped_lock lock{known_mutex};
    known_files.insert(path.filename().string());
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::StorageSave};

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        AddKnown(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)));
    }
    if (!EmulatorSettings.IsPipelineCacheArchived() && PrefetchLean()) {
        // sotc: SOTC_PREFETCH_LEAN: the same bytes are on disk: not written again (the file keeps its time).
        const auto file_name = std::filesystem::path{name + "." + GetBlobFileExtension(type)}.filename().string();
        if (SameOnDisk(file_name, data)) {
            return true;
        }
    }
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::StorageSave};

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        AddKnown(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)));
    }
    if (!EmulatorSettings.IsPipelineCacheArchived() && PrefetchLean()) {
        // sotc: SOTC_PREFETCH_LEAN: the same bytes are on disk: not written again (the file keeps its time).
        const auto file_name = std::filesystem::path{name + "." + GetBlobFileExtension(type)}.filename().string();
        std::vector<u8> bytes(data.size() * sizeof(u32));
        if (!data.empty()) {
            std::memcpy(bytes.data(), data.data(), bytes.size());
        }
        if (SameOnDisk(file_name, bytes)) {
            return true;
        }
    }
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::StorageLoad};

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (prefetch_active) {
        path.replace_extension(GetBlobFileExtension(type));
        TakePrefetched(path, data);
        return;
    }
    if (!prefetched.empty()) {
        auto file = path;
        file.replace_extension(GetBlobFileExtension(type));
        if (const auto it = prefetched.find(file.filename().string()); it != prefetched.end()) {
            data = *it->second.data;
            return;
        }
    }
    if (!EmulatorSettings.IsPipelineCacheArchived() &&
        !MayExist(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)))) {
        data.clear();
        return;
    }
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }
    const SotcBlockLedger::Scope ledger{SotcBlockLedger::StorageLoad};

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (prefetch_active) {
        path.replace_extension(GetBlobFileExtension(type));
        std::vector<u8> bytes;
        TakePrefetched(path, bytes);
        data.resize(bytes.size() / sizeof(u32));
        if (!data.empty()) {
            std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
        }
        return;
    }
    if (!prefetched.empty()) {
        auto file = path;
        file.replace_extension(GetBlobFileExtension(type));
        if (const auto it = prefetched.find(file.filename().string()); it != prefetched.end()) {
            const std::vector<u8>& bytes = *it->second.data;
            data.resize(bytes.size() / sizeof(u32));
            if (!data.empty()) {
                std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
            }
            return;
        }
    }
    if (!EmulatorSettings.IsPipelineCacheArchived() &&
        !MayExist(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)))) {
        data.clear();
        return;
    }
    return LoadVector(type, path, data);
}

// sotc: the blob name Save and Load take: the file name without its extension.
static std::string BlobName(const std::filesystem::path& file) {
    return file.stem().string();
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    ForEachNamedBlob(type,
                     [&](std::vector<u8>&& data, const std::string&) { func(std::move(data)); });
}

void DataBase::ForEachNamedBlob(
    BlobType type,
    const std::function<void(std::vector<u8>&& data, const std::string& name)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data), BlobName(file_name.data()));
            }
        }
    } else if (prefetch_active) {
        for (auto& [file_name, entry] : prefetched) {
            if (std::filesystem::path{file_name}.extension().string().ends_with(ext)) {
                entry.consumed.store(true, std::memory_order_relaxed); // sotc: SOTC_PREFETCH_LEAN
                func(std::vector<u8>{*entry.data}, BlobName(file_name));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data), BlobName(file_name.path()));
                }
            }
        }
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
    } else if (prefetch_active) {
        for (const auto& [file_name, entry] : prefetched) {
            count += std::filesystem::path{file_name}.extension().string().ends_with(ext);
        }
    } else {
        std::error_code ec;
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path, ec}) {
            count += file_name.path().extension().string().ends_with(ext);
        }
    }
    return count;
}

void DataBase::Reset() {
    Close();
    // Open() sets cache_path: a cache that was never opened has nothing to set aside.
    if (!cache_path.empty()) {
        auto old_path = cache_path;
        old_path += ".incompatible";
        std::error_code error;
        std::filesystem::remove_all(old_path, error);
        std::filesystem::rename(cache_path, old_path, error);
        if (error) {
            // Could not be renamed (e.g. a file in use): an empty cache matters more than the copy.
            std::filesystem::remove_all(cache_path, error);
        }
    }
    Open();
}

void DataBase::FinishPreload() {
    // sotc: the preloaded files stay in memory for the session (~220 MB): a load reads them from
    // there. Dropping them sent every later lookup to the disk, one small file at a time, while
    // the game loaded a save from the same hard disk: the game stood still for 27 s (22 Oct).
    prefetch_active = false;
    if (PrefetchLean() && !prefetched.empty()) {
        // sotc: SOTC_PREFETCH_LEAN: the entries the boot preload consumed (pipeline keys, the stages the warm-up
        // loaded: those programs stay resident) leave memory; their signatures stay for Save. The others are kept,
        // as before: a later load of an unloaded program reads them from here, not one small file from the disk.
        size_t dropped = 0, kept = 0;
        u64 dropped_bytes = 0, kept_bytes = 0;
        std::scoped_lock lock{known_mutex};
        for (auto it = prefetched.begin(); it != prefetched.end();) {
            const std::vector<u8>& bytes = *it->second.data;
            if (!it->second.consumed.load(std::memory_order_relaxed)) {
                ++kept;
                kept_bytes += bytes.size();
                ++it;
                continue;
            }
            ++dropped;
            dropped_bytes += bytes.size();
            dropped_sigs[it->first] = BlobSig(bytes.data(), bytes.size());
            it = prefetched.erase(it);
        }
        std::fprintf(stderr,
                     "SOTC PREFETCH LEAN: preload done: %zu consumed files dropped (%llu MB), %zu kept for later "
                     "loads (%llu MB)\n",
                     dropped, static_cast<unsigned long long>(dropped_bytes >> 20), kept,
                     static_cast<unsigned long long>(kept_bytes >> 20));
    }
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
