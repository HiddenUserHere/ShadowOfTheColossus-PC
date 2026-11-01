// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-owner VA windows of the native pool path (pc_pool_windows.h). Pure bookkeeping: no guest access, no host
// call except the reservation in Table::Reserve.
#include "pc_pool_windows.h"

#include <cstddef>
#include <cstdio>
#include <iterator>

namespace SotcPcPoolWindows {
namespace {

constexpr int kNone = -1;

uint64_t AlignUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

bool IsPow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

// The first free run (address order) whose aligned start leaves bytes before its end. Returns the run's start or 0.
// The aligned start is written to *start_out.
bool FindFit(const std::map<uint64_t, uint64_t>& free, uint64_t bytes, uint64_t align, uint64_t* start_out,
             std::map<uint64_t, uint64_t>::const_iterator* run_out) {
    for (auto it = free.begin(); it != free.end(); ++it) {
        const uint64_t a = AlignUp(it->first, align);
        if (a < it->second && it->second - a >= bytes) {
            *start_out = a;
            *run_out = it;
            return true;
        }
    }
    return false;
}

const SlotSpec kSpecs[kSlotCount] = {
    {"A gpu-arena", 128 * kMiB, 32 * kMiB, 2},
    {"B mask-heap", 16 * kMiB, 8 * kMiB, 1},
    {"C render-targets", 352 * kMiB, 0x14280000, 2}, // 322.5 MiB: the one RT block the logs show
};

} // namespace

const SlotSpec& Spec(int slot) {
    return kSpecs[slot];
}

uint64_t ReserveAlign(uint64_t align) {
    const uint64_t a = AlignUp(align, kPage);
    return a > kMiB ? a : kMiB;
}

// ---- Window ---------------------------------------------------------------------------------------------------

bool Window::Init(uint64_t base, uint64_t size, std::string* why) {
    auto fail = [&](const char* text) {
        if (why) {
            *why = text;
        }
        return false;
    };
    if (size == 0) {
        return fail("empty window");
    }
    if (base % kMiB != 0 || size % kMiB != 0) {
        return fail("window not 1 MiB aligned");
    }
    if (base > kWindowLimit || size > kWindowLimit - base) {
        return fail("window above the owner-tag or 2^40 limit");
    }
    base_ = base;
    size_ = size;
    free_.clear();
    live_.clear();
    free_[base] = base + size;
    if (why) {
        why->clear();
    }
    return true;
}

bool Window::Contains(uint64_t va, uint64_t bytes) const {
    return active() && bytes != 0 && va >= base_ && bytes <= size_ && va - base_ <= size_ - bytes;
}

bool Window::CanTake(uint64_t bytes, uint64_t align) const {
    if (!active() || bytes == 0 || !IsPow2(align)) {
        return false;
    }
    uint64_t start = 0;
    std::map<uint64_t, uint64_t>::const_iterator run;
    return FindFit(free_, bytes, align, &start, &run);
}

uint64_t Window::Take(uint64_t bytes, uint64_t align) {
    if (!active() || bytes == 0 || !IsPow2(align)) {
        return 0;
    }
    uint64_t start = 0;
    std::map<uint64_t, uint64_t>::const_iterator run;
    if (!FindFit(free_, bytes, align, &start, &run)) {
        return 0;
    }
    const uint64_t run_start = run->first;
    const uint64_t run_end = run->second;
    free_.erase(run);
    // The part of the run before the aligned start stays free (it is not lost), and so does the tail after the block.
    if (start > run_start) {
        free_[run_start] = start;
    }
    if (start + bytes < run_end) {
        free_[start + bytes] = run_end;
    }
    live_[start] = bytes;
    return start;
}

bool Window::Give(uint64_t va, uint64_t bytes) {
    const auto live = live_.find(va);
    if (live == live_.end() || live->second != bytes) {
        return false;
    }
    live_.erase(live);
    uint64_t s = va;
    uint64_t e = va + bytes;
    // Coalesce with the free run that starts at the block's end, then with the one that ends at its start.
    auto next = free_.find(e);
    if (next != free_.end()) {
        e = next->second;
        free_.erase(next);
    }
    auto after = free_.lower_bound(va);
    if (after != free_.begin()) {
        auto prev = std::prev(after);
        if (prev->second == va) {
            s = prev->first;
            free_.erase(prev);
        }
    }
    free_[s] = e;
    return true;
}

bool Window::IsLive(uint64_t va, uint64_t bytes) const {
    const auto live = live_.find(va);
    return live != live_.end() && live->second == bytes;
}

uint64_t Window::free_bytes() const {
    uint64_t total = 0;
    for (const auto& run : free_) {
        total += run.second - run.first;
    }
    return total;
}

// ---- Table ----------------------------------------------------------------------------------------------------

bool Table::Reserve(int slot, SotcPcPoolNativeFresh::FreshHost& host, const RegisterFn& reg, std::string* why) {
    if (slot < 0 || slot >= kSlotCount) {
        if (why) {
            *why = "no such window";
        }
        return false;
    }
    SlotState& s = slots_[slot];
    if (s.window.active()) {
        return true;
    }
    const SlotSpec& sp = Spec(slot);
    const uint64_t va = host.ReserveVirtual(sp.size, kMiB);
    if (va == 0 || va % kMiB != 0) {
        // An unaligned range cannot be given back to the host here: it is counted as leaked (boot only).
        ++stats_.reserve_failed;
        if (why) {
            *why = va == 0 ? "host gave no VA" : "host VA not 1 MiB aligned (leaked)";
        }
        return false;
    }
    if (va > kWindowLimit || sp.size > kWindowLimit - va) {
        ++stats_.reserve_limit;
        if (why) {
            *why = "window above the owner-tag or 2^40 limit (range leaked)";
        }
        return false;
    }
    if (!s.window.Init(va, sp.size, why)) {
        ++stats_.reserve_failed;
        return false;
    }
    ++stats_.reserve_ok;
    // The window still places blocks when the host refuses the registration (the announce path then covers its data).
    if (reg && !reg(va, sp.size, sp.name)) {
        ++stats_.register_failed;
    }
    return true;
}

bool Table::Bind(int slot, uint64_t obj, int64_t key) {
    if (slot < 0 || slot >= kSlotCount || obj == 0 || !slots_[slot].window.active()) {
        return false;
    }
    SlotState& s = slots_[slot];
    s.bound = true;
    s.obj = obj;
    s.key = key;
    return true;
}

int Table::Match(uint64_t obj, int32_t kind, int64_t key, uint64_t bytes) const {
    if (obj == 0) {
        return -1;
    }
    for (int i = 0; i < kSlotCount; ++i) {
        const SlotState& s = slots_[i];
        if (!s.bound || s.obj != obj) {
            continue;
        }
        const SlotSpec& sp = Spec(i);
        if ((kind >= 0 && sp.kind != kind) || sp.block != bytes) {
            continue;
        }
        if (s.key != kAnyKey && s.key != key) {
            continue;
        }
        return i;
    }
    return -1;
}

int Table::Route(uint64_t obj, int32_t kind, int64_t key, uint64_t bytes, uint64_t align) {
    const int slot = Match(obj, kind, key, bytes);
    if (slot < 0) {
        return -1;
    }
    ++stats_.routed[slot];
    if (!slots_[slot].window.CanTake(bytes, ReserveAlign(align))) {
        ++stats_.full[slot];
        return -1;
    }
    return slot;
}

uint64_t Table::TakeVa(int slot, uint64_t bytes, uint64_t align) {
    if (slot < 0 || slot >= kSlotCount || !slots_[slot].bound) {
        return 0;
    }
    const uint64_t va = slots_[slot].window.Take(bytes, align);
    if (va == 0) {
        ++stats_.take_failed[slot];
        return 0;
    }
    ++stats_.served[slot];
    return va;
}

void Table::GiveVa(int slot, uint64_t va, uint64_t bytes) {
    if (slot < 0 || slot >= kSlotCount) {
        return;
    }
    if (!slots_[slot].window.Give(va, bytes)) {
        ++stats_.take_failed[slot];
        return;
    }
    ++stats_.given_back[slot];
}

void Table::Commit(int slot, uint64_t va, uint64_t bytes, uint64_t node) {
    if (slot < 0 || slot >= kSlotCount) {
        return;
    }
    SlotState& s = slots_[slot];
    if (!s.window.IsLive(va, bytes)) {
        ++stats_.take_failed[slot];
        return;
    }
    s.node_of[va] = node;
    ++stats_.committed[slot];
    // The writer has backed the block (its batch map ran before this call), so the host can import it now.
    if (import_ && s.imported.count(va) == 0) {
        if (import_(va, bytes)) {
            s.imported[va] = bytes;
            ++stats_.imported[slot];
        } else {
            ++stats_.import_refused[slot];
        }
    }
}

void Table::Forget(uint64_t va) {
    const int slot = SlotOf(va);
    if (slot < 0) {
        return; // not a window block: nothing of the window's
    }
    SlotState& s = slots_[slot];
    for (const Parked& p : s.parked) {
        if (p.va == va) {
            return; // parked: the pool still holds it (a second release), its import stays
        }
    }
    s.node_of.erase(va);
    const auto it = s.imported.find(va);
    if (it == s.imported.end()) {
        return;
    }
    const uint64_t bytes = it->second;
    s.imported.erase(it);
    if (drop_) {
        drop_(va, bytes);
    }
    ++stats_.dropped[slot];
}

int Table::SlotOf(uint64_t va) const {
    for (int i = 0; i < kSlotCount; ++i) {
        const Window& w = slots_[i].window;
        if (w.active() && va >= w.base() && va < w.end()) {
            return i;
        }
    }
    return kNone;
}

bool Table::Release(uint64_t node, uint64_t va, uint64_t bytes) {
    const int slot = SlotOf(va);
    if (slot < 0) {
        return false; // not a windowed block: an ordinary free
    }
    SlotState& s = slots_[slot];
    bool ok = false;
    const auto it = s.node_of.find(va);
    if (it != s.node_of.end() && it->second == node && s.window.IsLive(va, bytes)) {
        ok = true;
        for (const Parked& p : s.parked) {
            if (p.va == va) {
                ok = false; // already parked: a second release is not a block of its own
                break;
            }
        }
    }
    if (!ok) {
        ++stats_.park_refused[slot];
        return false;
    }
    s.parked.push_back(Parked{node, va, bytes});
    ++stats_.parked[slot];
    return true;
}

bool Table::Reuse(uint64_t obj, int64_t key, uint64_t bytes, uint64_t* node) {
    const int slot = Match(obj, -1, key, bytes);
    if (slot < 0) {
        return false;
    }
    std::vector<Parked>& list = slots_[slot].parked;
    size_t best = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].bytes == bytes && (best == list.size() || list[i].va < list[best].va)) {
            best = i;
        }
    }
    if (best == list.size()) {
        ++stats_.reuse_miss[slot];
        return false;
    }
    if (node != nullptr) {
        *node = list[best].node;
    }
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(best));
    ++stats_.reused[slot];
    return true;
}

// ---- WindowFreshHost ------------------------------------------------------------------------------------------

uint64_t WindowFreshHost::ReserveVirtual(uint64_t size, uint64_t align) {
    last_va_ = 0;
    last_bytes_ = 0;
    if (slot_ >= 0) {
        const uint64_t va = table_.TakeVa(slot_, size, align);
        if (va != 0) {
            last_va_ = va;
            last_bytes_ = size;
            return va;
        }
        // The window cannot place it (counted by the table): the normal host gives the VA, as a non-window call.
    }
    return normal_.ReserveVirtual(size, align);
}

// ---- report ---------------------------------------------------------------------------------------------------

void ReportTable(const Table& table, std::string& out) {
    const Stats& s = table.stats();
    char line[512];
    std::snprintf(line, sizeof line, "SOTC PC POOL WINDOWS: reserved %llu, refused %llu (host %llu, limit %llu), "
                                     "register failed %llu\n",
                  static_cast<unsigned long long>(s.reserve_ok),
                  static_cast<unsigned long long>(s.reserve_failed + s.reserve_limit),
                  static_cast<unsigned long long>(s.reserve_failed), static_cast<unsigned long long>(s.reserve_limit),
                  static_cast<unsigned long long>(s.register_failed));
    out += line;
    for (int i = 0; i < kSlotCount; ++i) {
        const Window& w = table.window(i);
        if (!w.active()) {
            continue;
        }
        std::snprintf(line, sizeof line,
                      "SOTC PC POOL WINDOWS %s 0x%llx+%llu MiB bound %s: live %zu, parked %zu, free %llu MiB; "
                      "routed %llu, served %llu, full %llu, committed %llu, given back %llu, take failed %llu; "
                      "parked %llu (refused %llu), reused %llu (miss %llu)\n",
                      Spec(i).name, static_cast<unsigned long long>(w.base()),
                      static_cast<unsigned long long>(w.size() / kMiB), table.bound(i) ? "yes" : "no",
                      w.live_blocks(), table.parked(i).size(),
                      static_cast<unsigned long long>(w.free_bytes() / kMiB),
                      static_cast<unsigned long long>(s.routed[i]), static_cast<unsigned long long>(s.served[i]),
                      static_cast<unsigned long long>(s.full[i]), static_cast<unsigned long long>(s.committed[i]),
                      static_cast<unsigned long long>(s.given_back[i]),
                      static_cast<unsigned long long>(s.take_failed[i]), static_cast<unsigned long long>(s.parked[i]),
                      static_cast<unsigned long long>(s.park_refused[i]), static_cast<unsigned long long>(s.reused[i]),
                      static_cast<unsigned long long>(s.reuse_miss[i]));
        out += line;
        uint64_t imported_bytes = 0;
        for (const auto& [va, bytes] : table.imported(i)) {
            imported_bytes += bytes;
        }
        std::snprintf(line, sizeof line,
                      "SOTC PC POOL WINDOWS %s imports: imported %llu, refused %llu, dropped %llu; "
                      "imported blocks %zu (%llu MiB)\n",
                      Spec(i).name, static_cast<unsigned long long>(s.imported[i]),
                      static_cast<unsigned long long>(s.import_refused[i]),
                      static_cast<unsigned long long>(s.dropped[i]), table.imported(i).size(),
                      static_cast<unsigned long long>(imported_bytes / kMiB));
        out += line;
    }
}

} // namespace SotcPcPoolWindows
