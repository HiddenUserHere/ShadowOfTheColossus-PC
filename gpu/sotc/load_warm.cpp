// sotc: SOTC_LOAD_WARM, see load_warm.h.
#include "load_warm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/path_util.h"
#include "common/thread.h"

namespace SotcLoadWarm {
namespace {

constexpr std::uint64_t kPageShift = 16;                // 64 KiB pages
constexpr std::uint64_t kMaxPage = std::uint64_t{1} << (40 - kPageShift); // guest addresses < 2^40
constexpr std::uint64_t kWindowNs = 20'000'000'000ULL;  // record and warm for 20 s after an event
constexpr std::uint64_t kRepassNs = 250'000'000ULL;     // a new pass over the set every 250 ms
constexpr std::size_t kMaxPagesPerKey = 32768;          // 2 GiB per key
constexpr std::size_t kMaxRunPages = 16;                // a warm call covers at most 1 MiB
constexpr char kMagic[8] = {'S', 'O', 'T', 'C', 'W', 'R', 'M', '1'};
constexpr std::uint64_t kQuietBytes = 4ULL << 20;     // a pass this quiet ends the warm...
constexpr std::uint64_t kArrivedBytes = 64ULL << 20;  // ...once the window uploaded this much
// Uploads per presented frame: two gameplay frames held 42-53 MiB (~20 ms) of warm copies each
// (user/trace/_lw6); 16 MiB is ~6 ms, and over a 6 s load still ~2.9 GB.
constexpr std::uint64_t kFrameCapBytes = 16ULL << 20;

const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_LOAD_WARM");
    return v && v[0] == '1';
}();

// sotc: SOTC_LOAD_STREAM (see load_warm.h). Its own switch, read once.
const bool kStreamEnabled = [] {
    const char* v = std::getenv("SOTC_LOAD_STREAM");
    return v && (v[0] == '1' || v[0] == '2');
}();
const bool kStreamVerify = [] {
    const char* v = std::getenv("SOTC_LOAD_STREAM");
    return v && v[0] == '2';
}();
// Uploads of the idle drain per presented frame (SOTC_LOAD_STREAM_MB, 1-256, default 16).
const std::uint64_t kStreamFrameCapBytes = [] {
    const char* v = std::getenv("SOTC_LOAD_STREAM_MB");
    const long long mb = v ? std::atoll(v) : 16;
    return static_cast<std::uint64_t>(std::clamp<long long>(mb, 1, 256)) << 20;
}();
constexpr std::uint64_t kStreamPieceShift = 20;   // ranges are kept in 1 MiB pieces
constexpr std::size_t kStreamMaxPieces = 32768;   // 32 GiB of pieces per thread, a hard cap
constexpr unsigned kStreamCallsPerGap = 64;       // clean checks per idle gap, at most

std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string SafeKey(const char* name) {
    std::string out;
    for (const char* p = name; *p && out.size() < 64; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    return out.empty() ? std::string{"_"} : out;
}

std::filesystem::path KeyPath(const std::string& key) {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "load_warm" / (key + ".bin");
}

std::vector<std::uint64_t> ReadSet(const std::string& key) {
    std::vector<std::uint64_t> pages;
    std::ifstream in(KeyPath(key), std::ios::binary);
    char magic[8];
    std::uint32_t count = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 ||
        !in.read(reinterpret_cast<char*>(&count), 4) || count > kMaxPagesPerKey) {
        return pages;
    }
    pages.resize(count);
    if (!in.read(reinterpret_cast<char*>(pages.data()), count * sizeof(std::uint64_t))) {
        pages.clear();
    }
    // A foreign or damaged file: only page numbers inside the guest address space are kept.
    pages.erase(std::remove_if(pages.begin(), pages.end(),
                               [](std::uint64_t p) { return p >= kMaxPage; }),
                pages.end());
    return pages;
}

bool WriteSet(const std::string& key, const std::vector<std::uint64_t>& pages) {
    std::error_code ec;
    const auto path = KeyPath(key);
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const auto count = static_cast<std::uint32_t>(pages.size());
        out.write(kMagic, 8);
        out.write(reinterpret_cast<const char*>(&count), 4);
        out.write(reinterpret_cast<const char*>(pages.data()), pages.size() * sizeof(std::uint64_t));
        if (!out) {
            return false;
        }
    }
#ifdef _WIN32
    // Replaces an existing file (a plain rename may refuse to).
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::filesystem::rename(tmp, path, ec);
    return !ec;
#endif
}

// Texture records: one blob of the caller's fixed size per texture (the T# and the shader's image
// resource), stored raw. The file header holds the blob size, so a build whose blob differs
// ignores the file instead of misreading it.
constexpr char kTexMagic[8] = {'S', 'O', 'T', 'C', 'W', 'T', 'X', '1'};
constexpr std::size_t kMaxTexPerKey = 4096;

std::filesystem::path TexPath(const std::string& key) {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "load_warm" /
           (key + ".tex.bin");
}

// blob_size 0: whatever the file says (the caller checks the size of each blob it replays).
std::vector<std::string> ReadTex(const std::string& key, std::size_t& blob_size) {
    std::vector<std::string> blobs;
    std::ifstream in(TexPath(key), std::ios::binary);
    char magic[8];
    std::uint32_t size = 0, count = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, kTexMagic, 8) != 0 ||
        !in.read(reinterpret_cast<char*>(&size), 4) || size == 0 || size > 4096 ||
        (blob_size != 0 && size != blob_size) ||
        !in.read(reinterpret_cast<char*>(&count), 4) || count > kMaxTexPerKey) {
        return blobs;
    }
    blob_size = size;
    std::string blob(blob_size, '\0');
    for (std::uint32_t i = 0; i < count && in.read(blob.data(), blob_size); ++i) {
        blobs.push_back(blob);
    }
    return blobs;
}

bool WriteTex(const std::string& key, const std::vector<std::string>& blobs,
              std::size_t blob_size) {
    std::error_code ec;
    const auto path = TexPath(key);
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const auto size = static_cast<std::uint32_t>(blob_size);
        const auto count = static_cast<std::uint32_t>(blobs.size());
        out.write(kTexMagic, 8);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(reinterpret_cast<const char*>(&count), 4);
        for (const auto& b : blobs) {
            out.write(b.data(), static_cast<std::streamsize>(b.size()));
        }
        if (!out) {
            return false;
        }
    }
#ifdef _WIN32
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::filesystem::rename(tmp, path, ec);
    return !ec;
#endif
}

// Shared state, allocated once and never destroyed: the writer thread is detached and may still
// run while the process exits. Lock order: mutex only (the writer copies under it, writes outside).
struct State {
    // Textures (blob_size 0 until the first NoteTexture or a read tells it).
    std::size_t blob_size = 0;
    std::unordered_map<std::string, std::unordered_set<std::string>> tex_sets;
    std::vector<std::string> tex_dirty;
    std::vector<std::string> tex_queue; // the current key's records
    std::size_t tex_pos = 0;
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> sets; // learned, per key
    std::unordered_set<std::string> loaded;                                  // keys read from disk
    std::vector<std::string> dirty;                                          // keys to save
    std::string key;           // current window
    std::string prev_key;      // the window before it, for pages recorded just before the event
    std::uint64_t prev_seq = 0;
    std::uint64_t window_end = 0;
    std::vector<std::uint64_t> queue; // sorted pages of the current key's set
    std::size_t queue_pos = 0;
    std::uint64_t next_pass = 0;
    std::uint64_t pass_bytes = 0;   // uploaded by the current pass
    std::uint64_t window_bytes = 0; // uploaded since the event
};
State& S() {
    static State* const s = new State;
    return *s;
}

std::atomic<std::uint64_t> g_seq{0};                // window number (draw thread compares)
std::atomic<std::uint64_t> g_window_end_atomic{0};  // NoteBind's cheap check
std::atomic<bool> g_has_work{false};

// Draw-thread-local recording (only the draw thread binds).
thread_local std::uint64_t t_seq = 0;
thread_local std::uint64_t t_closed_seq = 0; // the window this thread saw end (skips the clock)
thread_local std::vector<std::uint64_t> t_pages;
thread_local int t_in_warm = 0;
thread_local std::uint64_t t_warm_frame = ~0ULL; // WarmIdle's frame and its uploads
thread_local std::uint64_t t_warm_frame_bytes = 0;
thread_local std::uint32_t t_warm_frame_tex = 0;
constexpr std::uint32_t kFrameCapTextures = 32;
// Ranges this thread already recorded in this window: the game binds the same buffers (some of
// them whole 64 MiB pools) on every draw, and walking their pages again on every bind cost ~25 ms
// of the first frame (samples, user/trace/_lw5). Direct-mapped by address and size.
struct RecentRange {
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
    std::uint64_t seq = 0;
};
constexpr std::size_t kRecentRanges = 4096;
thread_local RecentRange* t_recent = nullptr; // allocated on first use (the draw thread only)
struct RecentTex {
    std::uint64_t hash = 0;
    std::uint64_t seq = 0;
};
constexpr std::size_t kRecentTex = 1024;
thread_local RecentTex* t_recent_tex = nullptr;
std::atomic<std::uint64_t> s_tex_warmed{0};

std::atomic<std::uint64_t> s_noted{0}, s_warm_runs{0}, s_warm_bytes{0}, s_passes{0}, s_events{0},
    s_uploaded{0}, s_stops{0};

// sotc: SOTC_LOAD_STREAM state. The draw thread only (NoteBind and StreamIdle), so no lock.
// t_live_addrs: each bound 1 MiB piece once, in the order first bound; t_live_size: its size
// (the last bind of that address), so a grown bind is seen again by the drain.
thread_local std::vector<std::uint64_t> t_live_addrs;
thread_local std::unordered_map<std::uint64_t, std::uint64_t> t_live_size;
thread_local std::size_t t_live_pos = 0;
thread_local std::uint64_t t_stream_frame = ~0ULL;
thread_local std::uint64_t t_stream_frame_bytes = 0;
struct RecentStream {
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
};
constexpr std::size_t kRecentStream = 1024;
thread_local RecentStream* t_recent_stream = nullptr;
std::atomic<std::uint64_t> s_stream_drained{0}, s_stream_calls{0}, s_stream_drain_uploads{0},
    s_stream_async{0}, s_stream_bind{0}, s_stream_bind_max_ns{0}, s_stream_verify_bad{0};

void WriterLoop() {
    Common::SetCurrentThreadName("sotc:LoadWarmSave");
    State& s = S();
    for (;;) {
        std::vector<std::pair<std::string, std::vector<std::uint64_t>>> todo;
        std::vector<std::pair<std::string, std::vector<std::string>>> tex_todo;
        std::size_t blob_size = 0;
        {
            std::unique_lock lk{s.mutex};
            s.cv.wait_for(lk, std::chrono::seconds(5));
            for (const auto& key : s.dirty) {
                const auto& set = s.sets[key];
                std::vector<std::uint64_t> pages(set.begin(), set.end());
                std::sort(pages.begin(), pages.end());
                todo.emplace_back(key, std::move(pages));
            }
            s.dirty.clear();
            for (const auto& key : s.tex_dirty) {
                const auto& set = s.tex_sets[key];
                tex_todo.emplace_back(key, std::vector<std::string>(set.begin(), set.end()));
            }
            s.tex_dirty.clear();
            blob_size = s.blob_size;
        }
        for (const auto& [key, blobs] : tex_todo) {
            const bool ok = blob_size != 0 && WriteTex(key, blobs, blob_size);
            std::fprintf(stderr, "SOTC LOAD WARM: %s %s textures (%zu)\n",
                         ok ? "saved" : "could not save", key.c_str(), blobs.size());
        }
        for (const auto& [key, pages] : todo) {
            const bool ok = WriteSet(key, pages);
            std::fprintf(stderr, "SOTC LOAD WARM: %s %s (%zu pages, %.0f MiB)\n",
                         ok ? "saved" : "could not save", key.c_str(), pages.size(),
                         pages.size() * 64.0 / 1024.0);
        }
        std::fprintf(stderr,
                     "SOTC LOAD WARM: %llu events, %llu pages noted, %llu warm runs over %.0f MiB "
                     "in %llu passes, %.0f MiB uploaded, %llu early stops, %llu textures\n",
                     static_cast<unsigned long long>(s_events.load()),
                     static_cast<unsigned long long>(s_noted.load()),
                     static_cast<unsigned long long>(s_warm_runs.load()),
                     s_warm_bytes.load() / 1048576.0,
                     static_cast<unsigned long long>(s_passes.load()),
                     s_uploaded.load() / 1048576.0,
                     static_cast<unsigned long long>(s_stops.load()),
                     static_cast<unsigned long long>(s_tex_warmed.load()));
        if (kStreamEnabled) {
            std::fprintf(stderr,
                         "SOTC LOAD STREAM: %.0f MiB drained in %llu uploads (%llu calls), %.0f MiB "
                         "to the copy threads, %.0f MiB copied by binds on the draw thread (max "
                         "bind sync %.1f ms), verify mismatches %llu\n",
                         s_stream_drained.load() / 1048576.0,
                         static_cast<unsigned long long>(s_stream_drain_uploads.load()),
                         static_cast<unsigned long long>(s_stream_calls.load()),
                         s_stream_async.load() / 1048576.0, s_stream_bind.load() / 1048576.0,
                         s_stream_bind_max_ns.load() / 1e6,
                         static_cast<unsigned long long>(s_stream_verify_bad.load()));
        }
    }
}

void StartWriter() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread(WriterLoop).detach();
        std::fputs("SOTC LOAD WARM: on\n", stderr);
    });
}

// Draw thread: hands its recorded pages to the window they belong to. Called under S().mutex.
// Pages of a window older than the previous one are dropped (two events between flushes).
void FlushLocalLocked() {
    if (t_pages.empty()) {
        return;
    }
    State& s = S();
    const std::uint64_t cur = g_seq.load(std::memory_order_relaxed);
    const std::string* key = t_seq == cur ? &s.key : t_seq == s.prev_seq ? &s.prev_key : nullptr;
    if (key && !key->empty()) {
        auto& set = s.sets[*key];
        const std::size_t before = set.size();
        for (const std::uint64_t p : t_pages) {
            if (set.size() >= kMaxPagesPerKey) {
                break;
            }
            set.insert(p);
        }
        if (set.size() != before &&
            std::find(s.dirty.begin(), s.dirty.end(), *key) == s.dirty.end()) {
            s.dirty.push_back(*key);
        }
    }
    t_pages.clear();
}

} // namespace

bool Enabled() noexcept {
    return kEnabled;
}

bool TexturesEnabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_LOAD_WARM_TEX");
        return kEnabled && v && v[0] == '1';
    }();
    return on;
}

bool HasWork() noexcept {
    return g_has_work.load(std::memory_order_relaxed);
}

bool InWarm() noexcept {
    return t_in_warm != 0;
}

bool StreamEnabled() noexcept {
    return kStreamEnabled;
}

bool StreamVerify() noexcept {
    return kStreamVerify;
}

WarmScope::WarmScope() noexcept {
    ++t_in_warm;
}
WarmScope::~WarmScope() {
    --t_in_warm;
}

void OnEvent(const char* name) {
    if (!kEnabled || !name || !*name) {
        return;
    }
    StartWriter();
    State& s = S();
    const std::string key = SafeKey(name);
    // The saved set of the new key, read outside the lock (file I/O).
    bool need_read;
    {
        std::lock_guard lk{s.mutex};
        need_read = !s.loaded.count(key);
    }
    std::vector<std::uint64_t> from_disk = need_read ? ReadSet(key) : std::vector<std::uint64_t>{};
    std::size_t tex_size = 0;
    {
        std::lock_guard lk{s.mutex};
        tex_size = s.blob_size;
    }
    std::vector<std::string> tex_disk =
        need_read && TexturesEnabled() ? ReadTex(key, tex_size) : std::vector<std::string>{};
    std::lock_guard lk{s.mutex};
    if (need_read && s.loaded.insert(key).second) {
        s.sets[key].insert(from_disk.begin(), from_disk.end());
        if (!tex_disk.empty() && (s.blob_size == 0 || s.blob_size == tex_size)) {
            s.blob_size = tex_size;
            s.tex_sets[key].insert(tex_disk.begin(), tex_disk.end());
        }
    }
    // The old window ends here; the draw thread's pages of it are flushed on its next NoteBind
    // or warm call into the previous key.
    s.prev_key = s.key;
    s.prev_seq = g_seq.load(std::memory_order_relaxed);
    s.key = key;
    s.window_end = Now() + kWindowNs;
    g_window_end_atomic.store(s.window_end, std::memory_order_relaxed);
    g_seq.fetch_add(1, std::memory_order_release);
    const auto& set = s.sets[key];
    s.queue.assign(set.begin(), set.end());
    std::sort(s.queue.begin(), s.queue.end());
    s.queue_pos = 0;
    s.next_pass = 0;
    s.pass_bytes = 0;
    s.window_bytes = 0;
    const auto& tex = s.tex_sets[key];
    s.tex_queue.assign(tex.begin(), tex.end());
    s.tex_pos = 0;
    g_has_work.store(!s.queue.empty() || !s.tex_queue.empty(), std::memory_order_relaxed);
    s_events.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "SOTC LOAD WARM: event %s, %zu pages (%.0f MiB) and %zu textures to warm\n",
                 key.c_str(), s.queue.size(), s.queue.size() * 64.0 / 1024.0, s.tex_queue.size());
}

namespace {
// sotc: SOTC_LOAD_STREAM: records the 1 MiB pieces of a bound range on the draw thread. A
// direct-mapped cache of the last ranges skips the ones a loop of binds repeats.
void NoteStreamRange(std::uint64_t addr, std::uint64_t size) noexcept {
    if (!t_recent_stream) {
        t_recent_stream = new RecentStream[kRecentStream]{};
    }
    RecentStream& recent =
        t_recent_stream[((addr >> 8) ^ (addr >> 19) ^ size) & (kRecentStream - 1)];
    if (recent.addr == addr && recent.size == size) {
        return;
    }
    recent = RecentStream{addr, size};
    const std::uint64_t piece = std::uint64_t{1} << kStreamPieceShift;
    const std::uint64_t first = addr & ~(piece - 1);
    const std::uint64_t end = addr + size;
    for (std::uint64_t p = first; p < end; p += piece) {
        // The part of this piece the bind covers: starts at the bind's address or at the piece.
        const std::uint64_t lo = std::max(addr, p);
        const std::uint64_t len = std::min(end, p + piece) - lo;
        auto [it, inserted] = t_live_size.try_emplace(lo, len);
        if (inserted) {
            if (t_live_addrs.size() >= kStreamMaxPieces) {
                t_live_size.erase(it);
                return;
            }
            t_live_addrs.push_back(lo);
        } else if (len > it->second) {
            it->second = len;
        }
    }
}
} // namespace

void NoteBind(std::uint64_t addr, std::uint64_t size) noexcept {
    if (kStreamEnabled && !t_in_warm && size != 0) {
        NoteStreamRange(addr, size);
    }
    if (!kEnabled || t_in_warm || size == 0) {
        return;
    }
    const std::uint64_t seq = g_seq.load(std::memory_order_acquire);
    if (seq == 0 || seq == t_closed_seq) {
        return; // no window yet, or this thread already saw this one end (no clock read)
    }
    if (!t_recent) {
        t_recent = new RecentRange[kRecentRanges]{};
    }
    RecentRange& recent =
        t_recent[((addr >> 8) ^ (addr >> 20) ^ (size * 0x9E3779B97F4A7C15ULL >> 52)) &
                 (kRecentRanges - 1)];
    if (recent.seq == seq && recent.addr == addr && recent.size == size) {
        return; // recorded already in this window
    }
    recent = RecentRange{addr, size, seq};
    if (seq != t_seq) {
        // A new window: what this thread recorded belongs to the previous one. The window number
        // is read again under the lock: an event between the two reads would file this thread's
        // next pages under the previous key.
        std::lock_guard lk{S().mutex};
        FlushLocalLocked();
        t_seq = g_seq.load(std::memory_order_relaxed);
    }
    if (Now() >= g_window_end_atomic.load(std::memory_order_relaxed)) {
        std::lock_guard lk{S().mutex};
        FlushLocalLocked();
        t_closed_seq = seq;
        return;
    }
    const std::uint64_t first = addr >> kPageShift;
    const std::uint64_t last = (addr + size - 1) >> kPageShift;
    for (std::uint64_t p = first; p <= last && p - first < 4096; ++p) {
        if (t_pages.empty() || t_pages.back() != p) {
            t_pages.push_back(p);
        }
    }
    s_noted.fetch_add(last - first + 1, std::memory_order_relaxed);
    if (t_pages.size() >= 4096) {
        std::lock_guard lk{S().mutex};
        FlushLocalLocked();
    }
}

void SetTextureBlobSize(std::size_t size) noexcept {
    State& s = S();
    std::lock_guard lk{s.mutex};
    s.blob_size = size;
}

void NoteTexture(const void* blob, std::size_t size) noexcept {
    if (!TexturesEnabled() || t_in_warm || !blob || size == 0) {
        return;
    }
    const std::uint64_t seq = g_seq.load(std::memory_order_acquire);
    if (seq == 0 || seq == t_closed_seq || seq != t_seq ||
        Now() >= g_window_end_atomic.load(std::memory_order_relaxed)) {
        return; // no window, a closed or ended one, or NoteBind has not opened it on this thread
    }
    // FNV-1a of the blob: a texture is recorded once per window per thread cache slot.
    std::uint64_t h = 0xCBF29CE484222325ULL;
    const auto* p = static_cast<const unsigned char*>(blob);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ULL;
    }
    if (!t_recent_tex) {
        t_recent_tex = new RecentTex[kRecentTex]{};
    }
    RecentTex& slot = t_recent_tex[h & (kRecentTex - 1)];
    if (slot.seq == seq && slot.hash == h) {
        return;
    }
    slot = RecentTex{h, seq};
    State& s = S();
    std::lock_guard lk{s.mutex};
    if (s.key.empty() || seq != g_seq.load(std::memory_order_relaxed)) {
        return;
    }
    if (s.blob_size == 0) {
        s.blob_size = size;
    }
    if (s.blob_size != size) {
        return;
    }
    auto& set = s.tex_sets[s.key];
    if (set.size() < kMaxTexPerKey &&
        set.emplace(static_cast<const char*>(blob), size).second &&
        std::find(s.tex_dirty.begin(), s.tex_dirty.end(), s.key) == s.tex_dirty.end()) {
        s.tex_dirty.push_back(s.key);
    }
}

unsigned WarmIdle(std::uint64_t budget_ns, std::uint64_t frame, WarmFn warm, TexWarmFn tex,
                  StopFn stop, void* ctx) {
    if (!kEnabled) {
        return 0;
    }
    if (frame != t_warm_frame) {
        t_warm_frame = frame;
        t_warm_frame_bytes = 0;
        t_warm_frame_tex = 0;
    }
    if (t_warm_frame_bytes >= kFrameCapBytes) {
        return 0; // this frame's share is used
    }
    State& s = S();
    if (!g_has_work.load(std::memory_order_relaxed)) {
        if (!t_pages.empty()) { // a first visit records with an empty queue: keep its pages
            std::lock_guard lk{s.mutex};
            FlushLocalLocked();
        }
        return 0;
    }
    const std::uint64_t deadline = Now() + budget_ns;
    unsigned runs = 0;
    for (;;) {
        // Textures first, once per window: creating, tracking (page protection) and uploading an
        // image is the cost the first frame paid per new texture. A later guest write is tracked
        // and uploaded again at the real bind, as for any image.
        std::string blob;
        {
            std::lock_guard lk{s.mutex};
            // At most kFrameCapTextures per presented frame (their uploads are not counted in
            // bytes), so textures cannot take a gameplay frame's whole idle budget.
            if (tex && TexturesEnabled() && s.tex_pos < s.tex_queue.size() && Now() < s.window_end &&
                t_warm_frame_tex < kFrameCapTextures) {
                blob = s.tex_queue[s.tex_pos++];
                ++t_warm_frame_tex;
            }
        }
        if (!blob.empty()) {
            {
                WarmScope scope;
                tex(ctx, blob.data(), blob.size());
            }
            ++runs;
            s_tex_warmed.fetch_add(1, std::memory_order_relaxed);
            if (stop(ctx) || Now() >= deadline) {
                return runs;
            }
            continue;
        }
        std::uint64_t first = 0, count = 0;
        {
            std::lock_guard lk{s.mutex};
            FlushLocalLocked();
            const std::uint64_t now = Now();
            if (s.queue.empty() || now >= s.window_end) {
                g_has_work.store(false, std::memory_order_relaxed);
                return runs;
            }
            if (s.queue_pos >= s.queue.size()) {
                if (now < s.next_pass) {
                    return runs; // this pass is done; the next one starts later
                }
                s.queue_pos = 0;
                s.pass_bytes = 0;
                s_passes.fetch_add(1, std::memory_order_relaxed);
            }
            // One run of consecutive pages, at most kMaxRunPages.
            first = s.queue[s.queue_pos];
            count = 1;
            while (s.queue_pos + count < s.queue.size() && count < kMaxRunPages &&
                   s.queue[s.queue_pos + count] == first + count) {
                ++count;
            }
            s.queue_pos += count;
        }
        std::uint64_t uploaded;
        {
            WarmScope scope;
            uploaded = warm(ctx, first << kPageShift, count << kPageShift);
        }
        s_uploaded.fetch_add(uploaded, std::memory_order_relaxed);
        t_warm_frame_bytes += uploaded;
        {
            std::lock_guard lk{s.mutex};
            s.pass_bytes += uploaded;
            s.window_bytes += uploaded;
            if (s.queue_pos >= s.queue.size()) {
                // The end of a pass. A quiet pass after the data arrived (the window already
                // uploaded kArrivedBytes) ends this window's warm: it stays clean from here, and
                // more passes would only spend the gameplay frames' idle gaps on clean ranges.
                if (s.pass_bytes < kQuietBytes && s.window_bytes >= kArrivedBytes) {
                    g_has_work.store(false, std::memory_order_relaxed);
                    s_stops.fetch_add(1, std::memory_order_relaxed);
                    s_warm_runs.fetch_add(1, std::memory_order_relaxed);
                    return runs + 1;
                }
                s.next_pass = Now() + kRepassNs;
            }
        }
        ++runs;
        s_warm_runs.fetch_add(1, std::memory_order_relaxed);
        s_warm_bytes.fetch_add(count << kPageShift, std::memory_order_relaxed);
        if (stop(ctx) || Now() >= deadline || t_warm_frame_bytes >= kFrameCapBytes) {
            return runs;
        }
    }
}

unsigned StreamIdle(std::uint64_t budget_ns, std::uint64_t frame, WarmFn drain, StopFn stop,
                    void* ctx) {
    if (!kStreamEnabled || t_live_addrs.empty()) {
        return 0;
    }
    if (frame != t_stream_frame) {
        t_stream_frame = frame;
        t_stream_frame_bytes = 0;
    }
    if (t_stream_frame_bytes >= kStreamFrameCapBytes) {
        return 0; // this frame's share is used
    }
    const std::uint64_t deadline = Now() + budget_ns;
    unsigned calls = 0;
    while (calls < kStreamCallsPerGap) {
        if (t_live_pos >= t_live_addrs.size()) {
            t_live_pos = 0; // the next pass starts over; a gap never spans two passes
        }
        const std::uint64_t addr = t_live_addrs[t_live_pos++];
        const std::uint64_t size = t_live_size[addr];
        ++calls;
        s_stream_calls.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t uploaded;
        {
            WarmScope scope;
            uploaded = drain(ctx, addr, size);
        }
        if (uploaded != 0) {
            t_stream_frame_bytes += uploaded;
            s_stream_drained.fetch_add(uploaded, std::memory_order_relaxed);
            s_stream_drain_uploads.fetch_add(1, std::memory_order_relaxed);
        }
        if (stop(ctx) || Now() >= deadline || t_stream_frame_bytes >= kStreamFrameCapBytes) {
            break;
        }
    }
    return calls;
}

void NoteStreamAsync(std::uint64_t bytes) noexcept {
    s_stream_async.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteStreamBind(std::uint64_t bytes, std::uint64_t ns) noexcept {
    s_stream_bind.fetch_add(bytes, std::memory_order_relaxed);
    std::uint64_t cur = s_stream_bind_max_ns.load(std::memory_order_relaxed);
    while (ns > cur &&
           !s_stream_bind_max_ns.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
    }
}

void NoteStreamVerifyBad() noexcept {
    s_stream_verify_bad.fetch_add(1, std::memory_order_relaxed);
}

} // namespace SotcLoadWarm
