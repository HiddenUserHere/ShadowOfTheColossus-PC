// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: placement by memory class, decision core (pc_pool_place.h, design in
// docs/re-stutters/reports3/11-placement-by-class.md). Pure C++20: no guest access, no host calls.
#include "pc_pool_place.h"

#include <cstring>
#include <iterator>
#include <utility>

namespace SotcPcPlace {
namespace {

bool IsPow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

uint64_t RoundUp(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

bool SameName(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    return std::strcmp(a, b) == 0;
}

void SetWhy(std::string* why, const char* text) {
    if (why) {
        *why = text;
    }
}

} // namespace

const char* ClassName(Class c) {
    switch (c) {
    case Class::CpuOnly: return "CPU_ONLY";
    case Class::GpuStatic: return "GPU_STATIC";
    case Class::GpuDynamic: return "GPU_DYNAMIC";
    case Class::GpuWritten: return "GPU_WRITTEN";
    case Class::Unknown: return "UNKNOWN";
    case Class::kCount: break;
    }
    return "?";
}

const char* ModeName(Mode m) {
    switch (m) {
    case Mode::Off: return "off";
    case Mode::Observe: return "observe";
    case Mode::Verify: return "verify";
    case Mode::Apply: return "apply";
    }
    return "?";
}

Mode ParseMode(const char* text, bool* bad) {
    if (bad) {
        *bad = false;
    }
    if (!text || !text[0]) {
        return Mode::Off;
    }
    if (!std::strcmp(text, "0") || !std::strcmp(text, "off")) return Mode::Off;
    if (!std::strcmp(text, "1") || !std::strcmp(text, "observe")) return Mode::Observe;
    if (!std::strcmp(text, "2") || !std::strcmp(text, "verify")) return Mode::Verify;
    if (!std::strcmp(text, "3") || !std::strcmp(text, "apply")) return Mode::Apply;
    if (bad) {
        *bad = true;
    }
    return Mode::Off;
}

// ---- ClassTable ----------------------------------------------------------------------------------------

bool ClassTable::Add(const Rule& rule, std::string* why) {
    switch (rule.match) {
    case Rule::Match::Addr:
        if (rule.addr == 0) {
            SetWhy(why, "Addr rule with address 0");
            return false;
        }
        break;
    case Rule::Match::NameKind:
        if (!rule.name || !rule.name[0]) {
            SetWhy(why, "NameKind rule with no name");
            return false;
        }
        break;
    case Rule::Match::Heap:
        if (rule.heap_slot < 0) {
            SetWhy(why, "Heap rule with a negative slot");
            return false;
        }
        break;
    }
    if (static_cast<int>(rule.cls) < 0 || static_cast<int>(rule.cls) >= kClassCount) {
        SetWhy(why, "rule class out of range");
        return false;
    }
    rules_.push_back(rule);
    return true;
}

// ---- decision ------------------------------------------------------------------------------------------

Decision Decide(const Request& req, const ClassTable& table) {
    Decision d;
    const Rule* owner_rule = nullptr;
    const Rule* heap_rule = nullptr;

    // Owner: an exact address rule wins over a name and kind rule; table order breaks ties within a kind.
    for (const Rule& r : table.rules()) {
        if (r.match == Rule::Match::Addr && req.owner.addr != 0 && r.addr == req.owner.addr) {
            owner_rule = &r;
            break;
        }
    }
    if (!owner_rule) {
        for (const Rule& r : table.rules()) {
            if (r.match == Rule::Match::NameKind && r.kind == req.owner.kind &&
                SameName(r.name, req.owner.name)) {
                owner_rule = &r;
                break;
            }
        }
    }
    if (req.heap_slot >= 0) {
        for (const Rule& r : table.rules()) {
            if (r.match == Rule::Match::Heap && r.heap_slot == req.heap_slot) {
                heap_rule = &r;
                break;
            }
        }
    }

    if (owner_rule) {
        d.owner_matched = true;
        d.owner_class = owner_rule->cls;
    }
    if (heap_rule) {
        d.heap_matched = true;
        d.heap_class = heap_rule->cls;
    }

    if (!d.owner_matched && !d.heap_matched) {
        d.place = Class::Unknown;
        d.publish = Class::Unknown;
        d.reason = kReasonNoRule;
        return d;
    }

    // Placement: the heap is more specific than a shared owner (a CPU arena hosts several heaps).
    d.place = d.heap_matched ? d.heap_class : d.owner_class;

    if (d.heap_matched && d.owner_matched) {
        if (d.heap_class == d.owner_class) {
            d.publish = d.place;
            d.reason = kReasonAgree;
        } else {
            // A GPU heap inside a CPU owner (or the reverse): placed by the heap, never published as CPU_ONLY.
            d.publish = Class::Unknown;
            d.reason = kReasonDisagree;
        }
    } else if (d.heap_matched) {
        d.publish = d.place;
        d.reason = kReasonHeapOnly;
    } else {
        d.publish = d.place;
        d.reason = kReasonOwnerOnly;
    }

    // CPU_ONLY needs the owner to be classified: the heap alone does not say that the arena holds nothing else.
    if (d.publish == Class::CpuOnly && !d.owner_matched) {
        d.publish = Class::Unknown;
        d.reason = kReasonHeapOnly;
    }
    return d;
}

// ---- VA windows ----------------------------------------------------------------------------------------

bool WindowMap::Set(std::vector<VaWindow> windows, std::string* why) {
    for (const VaWindow& w : windows) {
        if (w.size == 0) {
            SetWhy(why, "window of size 0");
            return false;
        }
        if (w.base % kPlaceMiB != 0 || w.size % kPlaceMiB != 0) {
            SetWhy(why, "window not a multiple of 1 MiB");
            return false;
        }
        if (w.base < kVaFloor || w.base >= kVaCeiling || w.size > kVaCeiling - w.base) {
            SetWhy(why, "window outside the pool VA range");
            return false;
        }
        if (w.base + w.size > kVaFieldLimit) {
            SetWhy(why, "window beyond the 40-bit pointer field");
            return false;
        }
        if (w.cls == Class::Unknown || static_cast<int>(w.cls) >= kClassCount) {
            SetWhy(why, "window class is not a placement class");
            return false;
        }
    }
    // Insertion sort by base (the list is short; no <algorithm> needed).
    for (size_t i = 1; i < windows.size(); ++i) {
        for (size_t j = i; j > 0 && windows[j].base < windows[j - 1].base; --j) {
            VaWindow tmp = windows[j];
            windows[j] = windows[j - 1];
            windows[j - 1] = tmp;
        }
    }
    for (size_t i = 1; i < windows.size(); ++i) {
        if (windows[i].base < windows[i - 1].base + windows[i - 1].size) {
            SetWhy(why, "overlapping windows");
            return false;
        }
    }
    windows_ = std::move(windows);
    if (why) {
        why->clear();
    }
    return true;
}

bool WindowMap::Find(uint64_t va, VaWindow* out) const {
    // Last window with base <= va (the list is sorted and non-overlapping).
    size_t lo = 0;
    size_t hi = windows_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (windows_[mid].base <= va) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    const VaWindow& w = windows_[lo - 1];
    if (va < w.base || va - w.base >= w.size) {
        return false;
    }
    if (out) {
        *out = w;
    }
    return true;
}

Class WindowMap::ClassOf(uint64_t va) const {
    VaWindow w;
    return Find(va, &w) ? w.cls : Class::Unknown;
}

std::vector<VaWindow> CarveWindows(uint64_t base, uint64_t size,
                                   const std::vector<std::pair<Class, uint64_t>>& parts, std::string* why) {
    std::vector<VaWindow> out;
    if (base % kPlaceMiB != 0 || size % kPlaceMiB != 0) {
        SetWhy(why, "carve base or size not a multiple of 1 MiB");
        return out;
    }
    uint64_t used = 0;
    for (const auto& part : parts) {
        if (part.second == 0 || part.second % kPlaceMiB != 0) {
            SetWhy(why, "carve part not a non-zero multiple of 1 MiB");
            return {};
        }
        if (part.second > size - used) {
            SetWhy(why, "carve parts exceed the range");
            return {};
        }
        if (part.first == Class::Unknown || static_cast<int>(part.first) >= kClassCount) {
            SetWhy(why, "carve part class is not a placement class");
            return {};
        }
        out.push_back(VaWindow{base + used, part.second, part.first});
        used += part.second;
    }
    SetWhy(why, "");
    return out;
}

Class Effective(const Decision& decision, Class window_class, bool gpu_bound) {
    if (decision.publish == Class::Unknown) {
        return Class::Unknown;
    }
    // A block placed outside its class's window is not trusted: the window check is what the fault path uses.
    if (window_class != decision.publish) {
        return Class::Unknown;
    }
    // Bind evidence beats every prior: a CPU_ONLY block the GPU bound is demoted for good (the caller keeps it sticky).
    if (decision.publish == Class::CpuOnly && gpu_bound) {
        return Class::Unknown;
    }
    return decision.publish;
}

// ---- BlockLedger ---------------------------------------------------------------------------------------

void BlockLedger::NoteCarve(uint64_t base, Class cls) {
    auto it = blocks_.find(base);
    if (it == blocks_.end()) {
        Entry e;
        // An Unknown heap makes the block Unknown from its first carve.
        e.cls = cls;
        e.conflict = (cls == Class::Unknown);
        if (e.conflict) {
            e.cls = Class::Unknown;
        }
        blocks_.emplace(base, e);
        return;
    }
    Entry& e = it->second;
    if (e.conflict) {
        return;
    }
    if (cls != e.cls) {
        e.conflict = true;
        e.cls = Class::Unknown;
    }
}

Class BlockLedger::Get(uint64_t base) const {
    auto it = blocks_.find(base);
    if (it == blocks_.end() || it->second.conflict) {
        return Class::Unknown;
    }
    return it->second.cls;
}

bool BlockLedger::Conflicted(uint64_t base) const {
    auto it = blocks_.find(base);
    return it != blocks_.end() && it->second.conflict;
}

void BlockLedger::Forget(uint64_t base) {
    blocks_.erase(base);
}

// ---- VaRecycler ----------------------------------------------------------------------------------------

bool VaRecycler::Retire(uint64_t va, uint64_t size, Class cls, uint64_t frame, bool unmapped_confirmed,
                        std::string* why) {
    if (mode_ == Mode::Off) {
        SetWhy(why, "recycler off");
        return false;
    }
    if (size == 0 || va % kPlacePage != 0 || size % kPlacePage != 0 || va + size < va) {
        ++stats_.refused_retire;
        SetWhy(why, "bad size or alignment");
        return false;
    }
    if (cls == Class::Unknown || static_cast<int>(cls) >= kClassCount) {
        ++stats_.refused_retire;
        SetWhy(why, "class is not a placement class");
        return false;
    }
    if (!unmapped_confirmed) {
        ++stats_.refused_retire;
        SetWhy(why, "range not confirmed unmapped");
        return false;
    }
    // Double free or overlap with a run already retired.
    auto next = runs_.upper_bound(va);
    if (next != runs_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second.size > va) {
            ++stats_.refused_retire;
            SetWhy(why, "overlaps a retired run");
            return false;
        }
    }
    if (next != runs_.end() && next->first < va + size) {
        ++stats_.refused_retire;
        SetWhy(why, "overlaps a retired run");
        return false;
    }

    Run run;
    run.size = size;
    run.cls = cls;
    run.retire_frame = frame;
    run.invalidated = false;
    auto inserted = runs_.emplace(va, run).first;
    ++stats_.retired;
    stats_.retired_bytes += size;

    // Coalesce with a same-class left neighbour. The merged run waits for the later retire frame and needs
    // invalidation again (the new part is not invalidated yet).
    if (inserted != runs_.begin()) {
        auto left = std::prev(inserted);
        if (left->first + left->second.size == va && left->second.cls == cls) {
            left->second.size += size;
            if (frame > left->second.retire_frame) {
                left->second.retire_frame = frame;
            }
            left->second.invalidated = false;
            runs_.erase(inserted);
            inserted = left;
            ++stats_.merged;
        }
    }
    auto right = std::next(inserted);
    if (right != runs_.end() && inserted->first + inserted->second.size == right->first &&
        right->second.cls == cls) {
        inserted->second.size += right->second.size;
        if (right->second.retire_frame > inserted->second.retire_frame) {
            inserted->second.retire_frame = right->second.retire_frame;
        }
        inserted->second.invalidated = inserted->second.invalidated && right->second.invalidated;
        runs_.erase(right);
        ++stats_.merged;
    }
    SetWhy(why, "");
    return true;
}

void VaRecycler::MarkInvalidated(uint64_t va, uint64_t size) {
    if (size == 0) {
        return;
    }
    const uint64_t end = va + size;
    for (auto it = runs_.lower_bound(va); it != runs_.end() && it->first < end; ++it) {
        // Only a run lying wholly inside the invalidated range may be marked: a partly covered run stays unsafe.
        if (it->first + it->second.size <= end) {
            it->second.invalidated = true;
        }
    }
}

bool VaRecycler::Ready(const Run& r, uint64_t now_frame, uint64_t gpu_done_frame, const char** why) const {
    if (now_frame < r.retire_frame || now_frame - r.retire_frame < quarantine_) {
        *why = "quarantine";
        return false;
    }
    if (gpu_done_frame < r.retire_frame) {
        *why = "gpu";
        return false;
    }
    if (!r.invalidated) {
        *why = "invalid";
        return false;
    }
    return true;
}

bool VaRecycler::Take(Class cls, uint64_t size, uint64_t align, uint64_t now_frame, uint64_t gpu_done_frame,
                      uint64_t* va_out, std::string* why) {
    if (mode_ == Mode::Off) {
        SetWhy(why, "recycler off");
        return false;
    }
    if (size == 0 || size % kPlacePage != 0 || cls == Class::Unknown || static_cast<int>(cls) >= kClassCount) {
        SetWhy(why, "bad request");
        return false;
    }
    if (align == 0) {
        align = kPlacePage;
    }
    if (!IsPow2(align) || align % kPlacePage != 0) {
        SetWhy(why, "bad alignment");
        return false;
    }

    // First fit in address order over runs of the same class. The refusal of the first run that is big enough
    // is what gets counted: a run is not reusable for the reason it names.
    const char* first_refusal = nullptr;
    for (auto it = runs_.begin(); it != runs_.end(); ++it) {
        const uint64_t key = it->first;
        const Run& r = it->second;
        if (r.cls != cls) {
            continue;
        }
        const uint64_t start = RoundUp(key, align);
        if (start < key || start - key > r.size || size > r.size - (start - key)) {
            continue;
        }
        const char* refusal = nullptr;
        if (!Ready(r, now_frame, gpu_done_frame, &refusal)) {
            if (!first_refusal) {
                first_refusal = refusal;
            }
            continue;
        }
        if (mode_ == Mode::Quarantine) {
            ++stats_.would_reuse;
            SetWhy(why, "quarantine mode: would reuse");
            return false;
        }
        // Recycle: split the run around [start, start + size). Both remainders keep the run's state; a subset of an
        // invalidated range is invalidated too.
        const Run copy = r;
        runs_.erase(it);
        if (start > key) {
            runs_.emplace(key, Run{start - key, copy.cls, copy.retire_frame, copy.invalidated});
        }
        const uint64_t tail = key + copy.size - (start + size);
        if (copy.size > (start - key) + size) {
            runs_.emplace(start + size, Run{tail, copy.cls, copy.retire_frame, copy.invalidated});
        }
        ++stats_.taken;
        stats_.taken_bytes += size;
        if (va_out) {
            *va_out = start;
        }
        SetWhy(why, "");
        return true;
    }

    if (first_refusal) {
        if (!std::strcmp(first_refusal, "quarantine")) {
            ++stats_.refused_quarantine;
        } else if (!std::strcmp(first_refusal, "gpu")) {
            ++stats_.refused_gpu;
        } else {
            ++stats_.refused_invalid;
        }
        SetWhy(why, first_refusal);
    } else {
        SetWhy(why, "no retired run of that class is big enough");
    }
    return false;
}

uint64_t VaRecycler::retired_bytes_now() const {
    uint64_t total = 0;
    for (const auto& kv : runs_) {
        total += kv.second.size;
    }
    return total;
}

} // namespace SotcPcPlace
