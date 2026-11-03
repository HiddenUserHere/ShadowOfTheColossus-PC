// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine texture lifecycle events (texlife.h). The registry is keyed by the guest base address
// of the T# (dword0 << 8). A file texture is registered where its factory returns (create, the site
// after `call 0x96680` in 0x98aa0), its T# base and levels are refreshed on 0x880c30 (level) and on the
// streamed mip copy (mip), and it leaves on the dtor 0x985d0 (destroy). Lookups come from the texture
// cache under its own mutex; the registry mutex is a leaf (no call back into the cache).
#include "texlife.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "hooks.h"
#include "guest_mem.h"

namespace SotcTexLife {
namespace {

using SotcHooks::HookAction;

constexpr uintptr_t kTSharpOffset = 0xc4;       // CTexture: T# (32 bytes)
constexpr uintptr_t kChunkTextureOffset = 0x78; // TextureStreamingChunk -> CTexture (0x112d30)
constexpr uintptr_t kCTextureFromT = 0xc4;      // 0x880c30 receives CTexture + 0xc4 in rdi

struct Record {
    uint32_t gen = 0;  // NextGen() of the last event on this base
    uint32_t live = 0; // CTexture objects alive at this base
};

std::mutex g_mutex;
// One sequence for all records, so a generation is never reused: a record erased on destroy and
// created again at the same base gets a new value, and no image can match a stale one.
uint32_t g_gen_seq = 0;
uint32_t NextGen() {
    if (++g_gen_seq == 0) { // wrapped: skip 0 (it means "no record")
        ++g_gen_seq;
    }
    return g_gen_seq;
}

std::unordered_map<uint64_t, Record> g_records;    // guest base -> record
std::unordered_map<uintptr_t, uint64_t> g_base_of; // CTexture -> guest base (live objects)

std::atomic<uint64_t> g_create{0}, g_level{0}, g_mip{0}, g_destroy{0}, g_unmatched{0};
std::atomic<uint64_t> g_fast{0}, g_verify{0}, g_missed{0};

// Hook state handed from a pre phase to its post phase on the same thread.
thread_local uintptr_t t_ctex = 0;

uint64_t BaseOf(const uint32_t words[8]) {
    return static_cast<uint64_t>(words[0]) << 8;
}

// Create (0x98ac0, the file-texture factory's caller: rax = CTexture, T# complete).
void OnCreate(uintptr_t ctex) {
    if (ctex == 0) {
        return;
    }
    uint32_t words[8] = {};
    if (!SotcGuest::Read(static_cast<uint64_t>(ctex + kTSharpOffset), words, 32)) {
        g_unmatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t base = BaseOf(words);
    std::lock_guard lk{g_mutex};
    // A pointer seen again without a destroy (the dtor was not seen for it): drop the old entry.
    if (const auto old = g_base_of.find(ctex); old != g_base_of.end()) {
        auto& prev = g_records[old->second];
        if (prev.live > 0) {
            --prev.live;
        }
        prev.gen = NextGen();
        g_base_of.erase(old);
    }
    Record& rec = g_records[base];
    ++rec.live;
    rec.gen = NextGen();
    g_base_of.emplace(ctex, base);
    g_create.fetch_add(1, std::memory_order_relaxed);
}

// Level (0x880c30) and mip (0x112d30 -> 0x98ae0) events: the object is known; its base does not
// change, so the record only gets a new generation.
void OnChange(uintptr_t ctex, std::atomic<uint64_t>& counter) {
    if (ctex == 0) {
        return;
    }
    std::lock_guard lk{g_mutex};
    const auto it = g_base_of.find(ctex);
    if (it == g_base_of.end()) {
        g_unmatched.fetch_add(1, std::memory_order_relaxed); // not a file texture (or not created yet)
        return;
    }
    g_records[it->second].gen = NextGen();
    counter.fetch_add(1, std::memory_order_relaxed);
}

// Destroy (dtor 0x985d0, entry: rdi = CTexture).
void OnDestroy(uintptr_t ctex) {
    if (ctex == 0) {
        return;
    }
    std::lock_guard lk{g_mutex};
    const auto it = g_base_of.find(ctex);
    if (it == g_base_of.end()) {
        g_unmatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t base = it->second;
    g_base_of.erase(it);
    g_destroy.fetch_add(1, std::memory_order_relaxed);
    auto rec_it = g_records.find(base);
    if (rec_it == g_records.end()) {
        return;
    }
    Record& rec = rec_it->second;
    if (rec.live > 0) {
        --rec.live;
    }
    rec.gen = NextGen();
    if (rec.live == 0) {
        g_records.erase(rec_it);
    }
}

// Hook handlers.

HookAction PreCreateFile(SotcHooks::GuestRegs& regs, void*) {
    OnCreate(static_cast<uintptr_t>(regs.rax)); // Site after `call 0x96680` in 0x98aa0
    return HookAction::Continue;
}

HookAction PreLevel(SotcHooks::GuestRegs& regs, void*) {
    const uintptr_t t = static_cast<uintptr_t>(regs.rdi);
    t_ctex = t >= kCTextureFromT ? t - kCTextureFromT : 0; // 0x880c30: rdi = CTexture + 0xc4
    return HookAction::Continue;
}

void PostLevel(SotcHooks::GuestRegs&, void*) {
    OnChange(t_ctex, g_level);
    t_ctex = 0;
}

HookAction PreMip(SotcHooks::GuestRegs& regs, void*) {
    // 0x112d30 (tail-jumps to 0x98ae0): the CTexture is at chunk + 0x78.
    uint64_t tex = 0;
    t_ctex = SotcGuest::Read(static_cast<uint64_t>(regs.rdi + kChunkTextureOffset), &tex, 8)
                 ? static_cast<uintptr_t>(tex)
                 : 0;
    return HookAction::Continue;
}

void PostMip(SotcHooks::GuestRegs&, void*) {
    OnChange(t_ctex, g_mip);
    t_ctex = 0;
}

HookAction PreDestroy(SotcHooks::GuestRegs& regs, void*) {
    OnDestroy(static_cast<uintptr_t>(regs.rdi));
    return HookAction::Continue;
}

void Register(std::string_view name, SotcHooks::PreHandler pre, SotcHooks::PostHandler post) {
    SotcHooks::Handler handler;
    handler.pre = pre;
    handler.post = post;
    SotcHooks::RegisterHandler(name, handler);
}

// The mode the switch asks for (1, 2), before the hook check.
int RequestedMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TEX_LIFECYCLE");
        const int m = v ? std::atoi(v) : 0;
        return (m == 1 || m == 2) ? m : 0;
    }();
    return mode;
}

// Names of the hooks the registry needs that are not installed. The registry is used only when none is
// missing: without the create site, a level or mip change, or a destroy, a record could be stale.
std::string MissingHooks() {
    static const char* const kRequired[] = {"TEXLIFE_CREATE_FILE", "TEXLIFE_LEVEL", "TEXLIFE_MIP",
                                            "TEXLIFE_DESTROY"};
    std::string missing;
    for (const char* name : kRequired) {
        if (!SotcHooks::IsInstalled(name)) {
            missing += missing.empty() ? name : std::string(", ") + name;
        }
    }
    return missing;
}

} // namespace

int Mode() {
    // Read once, on the first draw or report after the hooks are installed.
    static const int mode = [] {
        if (RequestedMode() == 0) {
            return 0;
        }
        return MissingHooks().empty() ? RequestedMode() : 0;
    }();
    return mode;
}

uint32_t StaticGen(uint64_t base) {
    if (Mode() == 0) {
        return 0;
    }
    std::lock_guard lk{g_mutex};
    const auto it = g_records.find(base);
    if (it == g_records.end() || it->second.live == 0) {
        return 0;
    }
    return it->second.gen;
}

void NoteFast() {
    g_fast.fetch_add(1, std::memory_order_relaxed);
}

void NoteVerify(bool missed) {
    g_verify.fetch_add(1, std::memory_order_relaxed);
    if (missed) {
        g_missed.fetch_add(1, std::memory_order_relaxed);
    }
}

void Report(std::string& out) {
    if (RequestedMode() == 0) {
        return;
    }
    if (Mode() == 0) {
        out += "SOTC TEX LIFECYCLE: inactive, hook(s) not installed: " + MissingHooks() + "\n";
        return;
    }
    size_t live = 0;
    {
        std::lock_guard lk{g_mutex};
        live = g_base_of.size();
    }
    char line[320];
    std::snprintf(line, sizeof(line),
                  "SOTC TEX LIFECYCLE: textures live %zu, events create/level/mip/destroy "
                  "%llu/%llu/%llu/%llu, fast validations %llu, verify checks %llu, missed events %llu\n",
                  live, static_cast<unsigned long long>(g_create.load()),
                  static_cast<unsigned long long>(g_level.load()),
                  static_cast<unsigned long long>(g_mip.load()),
                  static_cast<unsigned long long>(g_destroy.load()),
                  static_cast<unsigned long long>(g_fast.load()),
                  static_cast<unsigned long long>(g_verify.load()),
                  static_cast<unsigned long long>(g_missed.load()));
    out += line;
    std::snprintf(line, sizeof(line), "SOTC TEX LIFECYCLE: unmatched events %llu\n",
                  static_cast<unsigned long long>(g_unmatched.load()));
    out += line;
}

void RegisterTexLife() {
    Register("TEXLIFE_CREATE_FILE", PreCreateFile, nullptr);
    Register("TEXLIFE_LEVEL", PreLevel, PostLevel);
    Register("TEXLIFE_MIP", PreMip, PostMip);
    Register("TEXLIFE_DESTROY", PreDestroy, nullptr);
}

} // namespace SotcTexLife
