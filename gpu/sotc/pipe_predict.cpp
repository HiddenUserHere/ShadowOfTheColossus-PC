// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PIPE_PREDICT (pipe_predict.h). The material side: the factory hook queues materials, and the command
// processor reads their pass slots. Guest reads go through the backing view with a null and mapped-region guard
// (probe_pass.cpp pattern), so a material freed meanwhile reads as garbage, never as a fault: its hashes then name no
// program and the command processor drops them.

#include "pipe_predict.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "gcn_tex_register.h" // sotc: SOTC_GCN_TEX_REGISTER reads the same materials
#include "hooks.h"
#include "nonblocking.h" // sotc: SOTC_GPL_PREDICT

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcPipePredict {
namespace {

// Material: MatSlot[14] at +0x60, stride 0x48 (pass-map.md section 3.1). In a slot, rec[0..4] at +0x10..+0x30, one per
// stage bit; rec[0] is the vertex program and rec[4] the pixel program (slot-stages.md, verified by SOTC_PASS_PROBE).
constexpr uintptr_t kSlotBase = 0x60;
constexpr uintptr_t kSlotStride = 0x48;
constexpr uint32_t kSlotCount = 14;
constexpr uintptr_t kSlotRecs = 0x10;
constexpr int kRecCount = 5;
// rec+0x10: the program bytes, which end in an OrbShdr footer with the 64-bit program hash at +16.
constexpr uintptr_t kProgramBytes = 0x10;
constexpr size_t kProgramScan = size_t{1} << 20;
constexpr char kOrb[] = "OrbShdr";
constexpr size_t kOrbLen = 7;
// A material whose slots hold no program yet is read again this many times (one call of TakeSlots apart).
constexpr int kRetries = 8;
// Queue bound: materials past it are dropped (counted in queued, never read).
constexpr size_t kQueueCap = size_t{1} << 16;

bool ReadGuest(uintptr_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!address || !runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + n > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

bool ReadU64(uintptr_t address, uint64_t* out) {
    return ReadGuest(address, out, sizeof *out);
}

// The program hash of the OrbShdr footer after prog (the first signature inside the bound).
bool ProgramHash(uintptr_t prog, uint64_t* hash) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!prog || !runtime_memory_region(prog, &start, &end, &mapped) || !mapped || prog < start) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(prog);
    if (!p) {
        return false;
    }
    const size_t n = std::min(kProgramScan, static_cast<size_t>(end - prog));
    size_t i = 0;
    while (i + 32 <= n) {
        const void* hit = std::memchr(p + i, 'O', n - i - 31);
        if (!hit) {
            break;
        }
        i = static_cast<size_t>(static_cast<const unsigned char*>(hit) - p);
        if (std::memcmp(p + i, kOrb, kOrbLen) == 0) {
            std::memcpy(hash, p + i + 16, sizeof *hash);
            return *hash != 0;
        }
        ++i;
    }
    return false;
}

// The footer scan reads the whole program, and most slots repeat a program another material uses: hashes are kept
// per program address (command processor only, as TakeSlots). A program freed and replaced at the same address keeps
// the old hash; its pipeline is then built for the old program, which only wastes that build.
std::unordered_map<uint64_t, uint64_t> g_prog_hash;

bool RecordHash(uint64_t rec, uint64_t* hash) {
    uint64_t prog = 0;
    if (!rec || !ReadU64(static_cast<uintptr_t>(rec) + kProgramBytes, &prog) || !prog) {
        return false;
    }
    if (const auto it = g_prog_hash.find(prog); it != g_prog_hash.end()) {
        *hash = it->second;
        return true;
    }
    if (!ProgramHash(static_cast<uintptr_t>(prog), hash)) {
        return false;
    }
    g_prog_hash.emplace(prog, *hash);
    return true;
}

struct Pending {
    uintptr_t material{};
    int tries{};
};

std::mutex g_mutex;
std::deque<Pending> g_queue; // guarded by g_mutex
std::atomic<uint64_t> g_queued{0}, g_read{0}, g_empty{0}, g_slots{0}, g_unreadable{0};
std::atomic<size_t> g_queue_size{0}; // g_queue.size(), read without the lock (Pending)
std::unordered_set<uint64_t> g_referenced; // guarded by g_mutex: VS and PS hashes of the materials taken

// The pipeline side wants the materials (MaterialsWanted without SOTC_GCN_TEX_REGISTER). Read once.
bool PipeWanted() noexcept {
    static const bool on = On() || SotcNonBlocking::GplPredictRequested() ||
                           SotcNonBlocking::GplPackReferenced(); // sotc: SOTC_PACK_REFERENCED reads the queue
    return on;
}

// PIPE_PREDICT_MATERIAL: post phase of the factory 0xf9390; rax is the material (0 when the parse failed).
void PostFactory(SotcHooks::GuestRegs& regs, void*) {
    const auto material = static_cast<uintptr_t>(regs.rax);
    if (!material) {
        return;
    }
    // sotc: SOTC_GCN_TEX_REGISTER registers the material's T#s (its blocks are final here, texture-binding.md T8.7).
    SotcGcnTexRegister::OnMaterial(material);
    if (!PipeWanted()) {
        return; // the hook is on for the T# registration only: no pipeline queue
    }
    g_queued.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() < kQueueCap) {
        g_queue.push_back({material, 0});
        g_queue_size.store(g_queue.size(), std::memory_order_relaxed);
    }
}

// Reads the VS+PS slots of a material, appended to out when given (count: unreadable slots are counted). any_program:
// some slot holds a program record. Returns the number of VS+PS slots found.
size_t ReadSlots(uintptr_t material, std::vector<Slot>* out, bool& any_program, bool count) {
    size_t found = 0;
    any_program = false;
    for (uint32_t k = 0; k < kSlotCount; ++k) {
        const uintptr_t slot = material + kSlotBase + kSlotStride * k;
        uint64_t recs[kRecCount] = {};
        for (int i = 0; i < kRecCount; ++i) {
            ReadU64(slot + kSlotRecs + 8 * static_cast<uintptr_t>(i), &recs[i]);
        }
        any_program = any_program || recs[0] || recs[4];
        // VS+PS only (the stage mask 17 of every material slot seen in a run): rec[0] and rec[4], nothing between.
        if (!recs[0] || !recs[4] || recs[1] || recs[2] || recs[3]) {
            continue;
        }
        Slot s{static_cast<uint8_t>(k), 0, 0};
        if (!RecordHash(recs[0], &s.vs) || !RecordHash(recs[4], &s.ps)) {
            if (count) {
                g_unreadable.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (out) {
            out->push_back(s);
        }
        ++found;
    }
    return found;
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PIPE_PREDICT");
        return v && v[0] == '1';
    }();
    return on;
}

bool MaterialsWanted() noexcept {
    // sotc: SOTC_GCN_TEX_REGISTER (gcn_tex_register.h) reads the materials of the same hook.
    static const bool on = PipeWanted() || SotcGcnTexRegister::MaterialsWanted();
    return on;
}

size_t TakeSlots(std::vector<Slot>& out, size_t max_materials) {
    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t n = std::min(max_materials, g_queue.size());
        batch.assign(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(n));
        g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(n));
        g_queue_size.store(g_queue.size(), std::memory_order_relaxed);
    }
    std::vector<Pending> again;
    std::vector<uint64_t> refs; // sotc: SOTC_PACK_REFERENCED: the programs these materials name
    for (const Pending& m : batch) {
        bool any_program = false;
        const size_t before = out.size();
        const size_t found = ReadSlots(m.material, &out, any_program, true);
        for (size_t i = before; i < out.size(); ++i) {
            refs.push_back(out[i].vs);
            refs.push_back(out[i].ps);
        }
        if (found != 0) {
            g_read.fetch_add(1, std::memory_order_relaxed);
            g_slots.fetch_add(found, std::memory_order_relaxed);
        } else if (!any_program && m.tries + 1 < kRetries) {
            again.push_back({m.material, m.tries + 1});
        } else {
            g_empty.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!again.empty() || !refs.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.insert(g_queue.end(), again.begin(), again.end());
        g_queue_size.store(g_queue.size(), std::memory_order_relaxed);
        g_referenced.insert(refs.begin(), refs.end());
    }
    return batch.size();
}

size_t ReferencedPrograms(std::unordered_set<uint64_t>& out) {
    const size_t before = out.size();
    std::vector<Pending> queued;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        out.insert(g_referenced.begin(), g_referenced.end());
        queued.assign(g_queue.begin(), g_queue.end());
    }
    std::vector<Slot> slots;
    for (const Pending& m : queued) {
        bool any_program = false;
        slots.clear();
        ReadSlots(m.material, &slots, any_program, false);
        for (const Slot& s : slots) {
            out.insert(s.vs);
            out.insert(s.ps);
        }
    }
    return out.size() - before;
}

bool HasQueued() noexcept {
    return g_queue_size.load(std::memory_order_relaxed) != 0;
}

MaterialStats Stats() noexcept {
    return {g_queued.load(std::memory_order_relaxed), g_read.load(std::memory_order_relaxed),
            g_empty.load(std::memory_order_relaxed), g_slots.load(std::memory_order_relaxed),
            g_unreadable.load(std::memory_order_relaxed)};
}

void Register() {
    SotcHooks::Handler handler;
    handler.post = PostFactory;
    SotcHooks::RegisterHandler("PIPE_PREDICT_MATERIAL", handler);
}

} // namespace SotcPipePredict
