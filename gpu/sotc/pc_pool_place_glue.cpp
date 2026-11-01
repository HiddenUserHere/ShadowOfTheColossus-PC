// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: placement by memory class, glue (see pc_pool_place_glue.h and docs/re-stutters/reports3/11-placement-by-class.md).
#include "pc_pool_place_glue.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace SotcPcPlaceGlue {
namespace {

constexpr uint64_t kDefaultQuarantineFrames = 120;
constexpr uint64_t kPage = PP::kPlacePage;

thread_local int32_t t_heap_slot = -1;

// Auto window plan: the class split of each pool set, in per-mille of the set's whole MiB count. The part below
// 1 MiB, and the per-mille rounding, is left unwindowed. The shares follow the wave-B live bytes per class
// (PC-MEMORY-REWRITE.md section 6, reports3/11 section 2.3): S0 holds CPU data only; S1 is CPU first, with a small
// GPU_WRITTEN part for the XPR mask heap; S2 holds the GPU static, dynamic and render-target data, with a small CPU
// spill. These are priors. The windows are carved once, at the first observed allocation, before any class byte has
// been seen, so the measured sizes come back through SOTC_PC_PLACE_WINDOWS (the report prints the live bytes per window).
struct SplitPart {
    PP::Class cls;
    uint64_t permille;
};
const SplitPart kPlanS0[] = {{PP::Class::CpuOnly, 1000}};
const SplitPart kPlanS1[] = {{PP::Class::CpuOnly, 875}, {PP::Class::GpuWritten, 125}};
const SplitPart kPlanS2[] = {{PP::Class::GpuStatic, 680},
                             {PP::Class::GpuDynamic, 160},
                             {PP::Class::GpuWritten, 100},
                             {PP::Class::CpuOnly, 60}};
struct SetPlan {
    const SplitPart* parts;
    size_t count;
};
const SetPlan kPlans[3] = {{kPlanS0, sizeof(kPlanS0) / sizeof(kPlanS0[0])},
                           {kPlanS1, sizeof(kPlanS1) / sizeof(kPlanS1[0])},
                           {kPlanS2, sizeof(kPlanS2) / sizeof(kPlanS2[0])}};

void Appendf(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

double MiB(uint64_t bytes) {
    return static_cast<double>(bytes) / 1048576.0;
}

void SetWhy(std::string* why, const std::string& text) {
    if (why) {
        *why = text;
    }
}

bool IsPow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

std::vector<std::string> Split(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool ParseU64(const std::string& text, uint64_t* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = static_cast<uint64_t>(v);
    return true;
}

bool EqualsIgnoreCase(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool ParseClassName(const std::string& text, PP::Class* out) {
    for (int i = 0; i < PP::kClassCount; ++i) {
        const PP::Class c = static_cast<PP::Class>(i);
        if (c == PP::Class::Unknown) {
            continue;
        }
        if (EqualsIgnoreCase(text, PP::ClassName(c))) {
            *out = c;
            return true;
        }
    }
    return false;
}

// Provisional name and kind priors (design section 6, item 2). The owner addresses of the nine block owners are
// the real key, and are added with AddRule(Addr) once a run has printed them. PageAllocator (kind 1) is deliberately
// absent: a live run showed 1,685 write faults per second on it, so a name prior would publish it wrongly.
void AddDefaultRules(PP::ClassTable& table) {
    struct Prior {
        const char* name;
        int32_t kind;
        PP::Class cls;
        const char* label;
    };
    static const Prior kPriors[] = {
        {"CPUMemory", 1, PP::Class::CpuOnly, "CPUMemory"},
        {"GPUMemory", 2, PP::Class::GpuDynamic, "GPUMemory"},
        {"Default TaggedArena", 2, PP::Class::GpuDynamic, "TaggedArena GPU"},
        {"RTAllocator", 2, PP::Class::GpuWritten, "RTAllocator"},
    };
    for (const Prior& p : kPriors) {
        PP::Rule r;
        r.match = PP::Rule::Match::NameKind;
        r.name = p.name;
        r.kind = p.kind;
        r.cls = p.cls;
        r.label = p.label;
        std::string why;
        table.Add(r, &why);
    }
}

} // namespace

// ---- configuration -------------------------------------------------------------------------------------

// Observed layout (reports3/06 section 1.1): S0 flexible (VA only) at 0x123c000000, S1 Onion 1 GiB at 0x1000300000,
// S2 Garlic at 0x1040300000 ending at 0x123bfe0000 (128 KiB below S0).
const PoolSetLayout kDefaultPoolSets[3] = {
    {UINT64_C(0x123c000000), UINT64_C(425984000), true},   // S0 flexible: 406 MiB and 256 KiB
    {UINT64_C(0x1000300000), UINT64_C(0x40000000), true},  // S1 Onion: 1 GiB
    {UINT64_C(0x1040300000), UINT64_C(8519548928), true},  // S2 Garlic: 8124 MiB and 256 KiB
};

Config MakeConfig(const char* place, const char* recycle, const char* frames, const char* windows) {
    Config cfg;
    bool bad = false;
    cfg.place = PP::ParseMode(place, &bad);
    if (bad) {
        cfg.warnings += std::string("SOTC_PC_PLACE '") + place + "' is not 0..3 or a mode name, off. ";
    }

    if (!recycle || !recycle[0] || !std::strcmp(recycle, "0")) {
        cfg.recycle = PP::VaRecycler::Mode::Off;
    } else if (!std::strcmp(recycle, "1")) {
        cfg.recycle = PP::VaRecycler::Mode::Quarantine;
    } else if (!std::strcmp(recycle, "2")) {
        cfg.recycle = PP::VaRecycler::Mode::Recycle;
    } else {
        cfg.warnings += std::string("SOTC_PC_RECYCLE '") + recycle + "' is not 0..2, off. ";
    }

    cfg.quarantine_frames = kDefaultQuarantineFrames;
    if (frames && frames[0]) {
        char* end = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(frames, &end, 10);
        if (end == frames || *end != '\0' || errno == ERANGE) {
            cfg.warnings += std::string("SOTC_PC_RECYCLE_FRAMES '") + frames + "' is not a count, default used. ";
        } else {
            cfg.quarantine_frames = static_cast<uint64_t>(v);
        }
    }

    // Unset or empty: the windows are carved from the pool sets at the first observed allocation (auto).
    // "off" or "none": no windows. Anything else is an explicit spec, checked by the Glue constructor.
    if (!windows || !windows[0]) {
        cfg.windows_auto = true;
    } else if (!std::strcmp(windows, "off") || !std::strcmp(windows, "none")) {
        cfg.windows_auto = false;
    } else {
        cfg.windows_spec = windows;
    }
    return cfg;
}

Config ConfigFromEnv() {
    return MakeConfig(std::getenv("SOTC_PC_PLACE"), std::getenv("SOTC_PC_RECYCLE"),
                      std::getenv("SOTC_PC_RECYCLE_FRAMES"), std::getenv("SOTC_PC_PLACE_WINDOWS"));
}

bool ParseWindowSpec(const char* spec, std::vector<PP::VaWindow>* out, std::string* why) {
    if (out) {
        out->clear();
    }
    SetWhy(why, "");
    if (!spec || !spec[0]) {
        return true;
    }
    std::vector<PP::VaWindow> windows;
    for (const std::string& entry : Split(spec, ';')) {
        if (entry.empty()) {
            continue; // a trailing ';' is fine
        }
        const std::vector<std::string> fields = Split(entry, ':');
        if (fields.size() != 3) {
            SetWhy(why, "window '" + entry + "' is not base:size:CLASS");
            return false;
        }
        PP::VaWindow w;
        if (!ParseU64(fields[0], &w.base) || !ParseU64(fields[1], &w.size)) {
            SetWhy(why, "window '" + entry + "' has a bad base or size");
            return false;
        }
        if (!ParseClassName(fields[2], &w.cls)) {
            SetWhy(why, "window '" + entry + "' has an unknown class");
            return false;
        }
        windows.push_back(w);
    }
    if (out) {
        *out = windows;
    }
    return true;
}

// ---- heap context --------------------------------------------------------------------------------------

HeapScope::HeapScope(int32_t slot) : prev_(t_heap_slot) {
    t_heap_slot = slot;
}

HeapScope::~HeapScope() {
    t_heap_slot = prev_;
}

int32_t CurrentHeapSlot() {
    return t_heap_slot;
}

int32_t PushHeapSlot(int32_t slot) {
    const int32_t previous = t_heap_slot;
    t_heap_slot = slot;
    return previous;
}

void PopHeapSlot(int32_t previous) {
    t_heap_slot = previous;
}

// ---- Glue ----------------------------------------------------------------------------------------------

Glue::Glue(const Config& cfg)
    : cfg_(cfg),
      enabled_(cfg.place != PP::Mode::Off || cfg.recycle != PP::VaRecycler::Mode::Off),
      recycler_(cfg.recycle, cfg.quarantine_frames) {
    if (!cfg_.windows_spec.empty()) {
        std::vector<PP::VaWindow> windows;
        std::string why;
        if (!ParseWindowSpec(cfg_.windows_spec.c_str(), &windows, &why) || !windows_.Set(windows, &why)) {
            cfg_.warnings += "SOTC_PC_PLACE_WINDOWS refused (" + why + "), no windows. ";
        }
    }
    // Auto windows are carved lazily (EnsureWindows) at the first observed allocation, only when the glue is on.
    windows_done_ = !(cfg_.windows_auto && enabled_) || !cfg_.windows_spec.empty();
    if (enabled_) {
        AddDefaultRules(rules_);
    }
}

bool Glue::AddRule(const PP::Rule& rule, std::string* why) {
    std::lock_guard<std::mutex> lock(mu_);
    return rules_.Add(rule, why);
}

void Glue::SetPoolLayout(const PoolSetLayout sets[3]) {
    std::lock_guard<std::mutex> lock(mu_);
    if (windows_done_) {
        return; // the windows are already carved: a later layout cannot change them
    }
    for (int i = 0; i < 3; ++i) {
        layout_[i] = sets[i];
    }
}

// The auto plan. Called with mu_ held, before the first block is classified (and before any window is read).
// Each set is split by kPlans[set] inside its own 1 MiB-rounded size; a set that is not pristine gets no windows.
// The result is final: an explicit spec or a failed plan never retries.
void Glue::EnsureWindows() {
    if (windows_done_) {
        return;
    }
    windows_done_ = true;
    std::vector<PP::VaWindow> all;
    std::string note;
    for (int s = 0; s < 3; ++s) {
        const PoolSetLayout& L = layout_[s];
        if (!L.pristine) {
            note += "S" + std::to_string(s) + " not pristine, no windows; ";
            continue;
        }
        const uint64_t mib = L.size / PP::kPlaceMiB;
        if (mib == 0) {
            note += "S" + std::to_string(s) + " smaller than 1 MiB; ";
            continue;
        }
        std::vector<std::pair<PP::Class, uint64_t>> parts;
        for (size_t i = 0; i < kPlans[s].count; ++i) {
            const SplitPart& p = kPlans[s].parts[i];
            const uint64_t part_mib = mib * p.permille / 1000;
            if (part_mib != 0) {
                parts.emplace_back(p.cls, part_mib * PP::kPlaceMiB);
            }
        }
        std::string why;
        const std::vector<PP::VaWindow> w = PP::CarveWindows(L.base, mib * PP::kPlaceMiB, parts, &why);
        if (w.empty()) {
            note += "S" + std::to_string(s) + " not carved (" + why + "); ";
            continue;
        }
        all.insert(all.end(), w.begin(), w.end());
    }
    std::sort(all.begin(), all.end(), [](const PP::VaWindow& a, const PP::VaWindow& b) { return a.base < b.base; });
    std::string why;
    if (!windows_.Set(all, &why)) {
        cfg_.warnings += "auto windows refused (" + why + "), no windows. ";
        note += "refused; ";
    }
    windows_note_ = note;
}

std::vector<PP::VaWindow> Glue::Windows() {
    std::lock_guard<std::mutex> lock(mu_);
    EnsureWindows();
    return windows_.windows();
}

std::vector<PP::VaWindow> Glue::WindowsFor(const PP::OwnerKey& owner, int32_t heap_slot) {
    std::vector<PP::VaWindow> out;
    if (!enabled_ || cfg_.place != PP::Mode::Apply) {
        return out;
    }
    std::lock_guard<std::mutex> lock(mu_);
    EnsureWindows();
    if (heap_slot < 0) {
        heap_slot = CurrentHeapSlot();
    }
    PP::Request req;
    req.owner = owner;
    req.heap_slot = heap_slot;
    const PP::Decision d = PP::Decide(req, rules_);
    if (d.place == PP::Class::Unknown) {
        return out;
    }
    for (const PP::VaWindow& w : windows_.windows()) {
        if (w.cls == d.place) {
            out.push_back(w);
        }
    }
    return out;
}

// Stage 3 window fit (pure). Walks the free runs of set in list order, as pool_take_free_run does, but only the part of
// each run that lies inside the window counts. The run is cut to [max(run.base, w.base), min(run end, window end)), and
// the block starts at the first address there aligned to align. The pool's own rule (pad + bytes <= run size) becomes
// "the aligned start plus bytes fits in the cut run".
bool FitInWindow(const SotcPcPool::Model::Pools& pre, int set, const PP::VaWindow& w, uint64_t bytes, uint64_t align,
                 WindowFit* out) {
    namespace M = SotcPcPool::Model;
    if (set < 0 || set >= M::kSetCount || bytes == 0 || bytes % kPage != 0 || w.size == 0 || align == 0 ||
        !IsPow2(align) || w.base + w.size < w.base) {
        return false;
    }
    const M::Set& s = pre.set[set];
    if (static_cast<int64_t>(bytes / kPage) > static_cast<int64_t>(s.free_va_pages)) {
        return false; // the pool's own early test (0x1a800 checks pages <= +0x70 first)
    }
    const uint64_t wend = w.base + w.size;
    for (size_t i = 0; i < s.free_va.size(); ++i) {
        const M::Run& r = s.free_va[i];
        const uint64_t rend = r.base + r.size;
        const uint64_t lo = r.base > w.base ? r.base : w.base;
        const uint64_t hi = rend < wend ? rend : wend;
        if (hi <= lo) {
            continue;
        }
        const uint64_t start = (lo + align - 1) & ~(align - 1);
        if (start < lo || start >= hi || hi - start < bytes) {
            continue;
        }
        if (out) {
            out->run = i;
            out->start = start;
            // The same test as HitPath (pc_pool.cpp): a pad or a split takes a node, so only this is native-exact.
            out->exact = start == r.base && bytes >= r.backed;
        }
        return true;
    }
    return false;
}

bool On() {
    return Global().enabled();
}

size_t Glue::blocks() const {
    std::lock_guard<std::mutex> lock(mu_);
    return blocks_.size();
}

PP::Class Glue::WindowClassOfBlock(uint64_t base, uint64_t size) const {
    PP::VaWindow w;
    if (size == 0 || !windows_.Find(base, &w)) {
        return PP::Class::Unknown;
    }
    // The whole block must lie in one window; a block straddling a boundary is not trusted.
    if (base + size - 1 >= w.base + w.size) {
        return PP::Class::Unknown;
    }
    return w.cls;
}

bool Glue::LedgerOk(uint64_t base, PP::Class publish) const {
    if (ledger_.Conflicted(base)) {
        return false;
    }
    const PP::Class carved = ledger_.Get(base); // Unknown for an unseen block (no carve evidence)
    return carved == PP::Class::Unknown || carved == publish;
}

// The class a block would publish as. use_window: with the window check (verify and apply), else the decision alone
// (observe). Bind and write evidence are sticky, so once demoted a CPU_ONLY block publishes Unknown for good.
PP::Class Glue::Publish(const Block& b, uint64_t base, bool use_window) const {
    if (b.decision.publish == PP::Class::Unknown || !LedgerOk(base, b.decision.publish)) {
        return PP::Class::Unknown;
    }
    const PP::Class window = use_window ? WindowClassOfBlock(base, b.size) : b.decision.publish;
    return PP::Effective(b.decision, window, b.bound || b.demoted);
}

Glue::Block* Glue::FindBlock(uint64_t va, uint64_t* base_out) {
    auto it = blocks_.upper_bound(va);
    if (it == blocks_.begin()) {
        return nullptr;
    }
    --it;
    if (va - it->first >= it->second.size) {
        return nullptr;
    }
    if (base_out) {
        *base_out = it->first;
    }
    return &it->second;
}

void Glue::OnAllocObserved(const PP::OwnerKey& owner, uint64_t va, uint64_t size, int set, int32_t heap_slot) {
    if (!enabled_ || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (heap_slot < 0) {
        heap_slot = CurrentHeapSlot();
    }
    EnsureWindows(); // the auto plan is fixed at the first block, before it is classified
    ++c_.allocs;
    c_.alloc_bytes += size;
    if (heap_slot < 0) {
        ++c_.no_heap_context;
    }
    PP::Request req;
    req.owner = owner;
    req.heap_slot = heap_slot;

    Block b;
    b.size = size;
    b.set = set;
    b.decision = PP::Decide(req, rules_);

    if (blocks_.count(va)) {
        // The VA was handed out again without a free we saw: the old record is stale. A fresh block starts clean.
        ++c_.dup_allocs;
        blocks_.erase(va);
        ledger_.Forget(va);
    }
    blocks_.emplace(va, b);
}

void Glue::OnFreeObserved(uint64_t va, uint64_t size) {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    EnsureWindows();
    ++c_.frees;
    auto it = blocks_.find(va);
    if (it == blocks_.end()) {
        ++c_.frees_unobserved;
        return;
    }
    const Block b = it->second;
    blocks_.erase(it);
    ledger_.Forget(va);
    if (cfg_.recycle == PP::VaRecycler::Mode::Off) {
        return;
    }
    if (size != b.size) {
        ++c_.frees_size_mismatch; // not the block we recorded: never retire it
        return;
    }
    // The run keeps the class of the window it sits in: Take hands it back only to that class (design 4, rule 5).
    PP::Class cls = PP::Class::Unknown;
    PP::VaWindow w;
    if (windows_.Find(va, &w) && va + size <= w.base + w.size) {
        cls = w.cls;
    }
    Pending p;
    p.va = va;
    p.size = size;
    p.cls = cls;
    pending_.push_back(p);
}

void Glue::OnFlushDone(const std::function<bool(uint64_t, uint64_t)>& is_unmapped) {
    if (!enabled_ || cfg_.recycle == PP::VaRecycler::Mode::Off) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const Pending& p : pending_) {
        // The retire frame is the frame of the flush: the unmap is only known now, so the quarantine starts here.
        const bool unmapped = is_unmapped && is_unmapped(p.va, p.size);
        std::string why;
        recycler_.Retire(p.va, p.size, p.cls, frame_, unmapped, &why);
    }
    pending_.clear();
}

void Glue::OnHeapCarve(uint64_t addr, int32_t heap_slot) {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    ++c_.carves;
    PP::Request req;
    req.heap_slot = heap_slot;
    const PP::Decision d = PP::Decide(req, rules_);
    if (!d.heap_matched) {
        ++c_.carves_unruled; // an unruled heap makes the block Unknown from this carve on (BlockLedger)
    }
    uint64_t base = 0;
    if (!FindBlock(addr, &base)) {
        ++c_.carves_untracked;
        return;
    }
    ledger_.NoteCarve(base, d.place);
}

void Glue::OnGpuBind(uint64_t addr, uint64_t size) {
    if (!enabled_ || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    ++c_.binds;
    const uint64_t end = (UINT64_MAX - addr < size) ? UINT64_MAX : addr + size;
    bool any = false;
    // Blocks are disjoint, so walking down from the first block at or past end visits the overlapping ones and
    // stops at the first block that ends at or before addr.
    auto it = blocks_.lower_bound(end);
    while (it != blocks_.begin()) {
        --it;
        Block& b = it->second;
        if (it->first + b.size <= addr) {
            break;
        }
        any = true;
        if (!b.bound) {
            if (Publish(b, it->first, false) == PP::Class::CpuOnly) {
                ++c_.cpu_only_bound;
            }
            if (Publish(b, it->first, true) == PP::Class::CpuOnly) {
                ++c_.cpu_only_bound_windowed;
            }
            b.bound = true;
            ++c_.bound_blocks;
        }
    }
    if (!any) {
        ++c_.binds_untracked;
    }
}

void Glue::OnWriteFault(uint64_t addr) {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    ++c_.write_faults;
    uint64_t base = 0;
    Block* found = FindBlock(addr, &base);
    if (!found) {
        ++c_.write_faults_untracked;
        return;
    }
    Block& b = *found;
    if (Publish(b, base, false) == PP::Class::CpuOnly) {
        ++c_.cpu_only_write; // the GPU reads a page we predicted CPU_ONLY: demote it for good
        b.demoted = true;
    }
}

void Glue::OnFrameBegin(uint64_t frame) {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    frame_ = frame;
}

void Glue::OnSubmitFence(uint64_t gpu_done_frame) {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (gpu_done_frame > gpu_done_) {
        gpu_done_ = gpu_done_frame;
    }
}

void Glue::OnCachesInvalidated(uint64_t va, uint64_t size) {
    if (!enabled_ || cfg_.recycle == PP::VaRecycler::Mode::Off) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    recycler_.MarkInvalidated(va, size);
}

bool Glue::ChooseWindow(const PP::OwnerKey& owner, int32_t heap_slot, uint64_t size, uint64_t align,
                        PP::VaWindow* out) {
    if (!enabled_ || cfg_.place != PP::Mode::Apply || size == 0) {
        return false;
    }
    if (align == 0) {
        align = kPage;
    }
    if (!IsPow2(align)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (heap_slot < 0) {
        heap_slot = CurrentHeapSlot();
    }
    PP::Request req;
    req.owner = owner;
    req.heap_slot = heap_slot;
    const PP::Decision d = PP::Decide(req, rules_);
    if (d.place == PP::Class::Unknown) {
        ++c_.choose_misses;
        return false;
    }
    EnsureWindows();
    for (const PP::VaWindow& w : windows_.windows()) {
        if (w.cls != d.place) {
            continue;
        }
        const uint64_t start = (w.base + align - 1) & ~(align - 1);
        if (start < w.base || start - w.base > w.size || size > w.size - (start - w.base)) {
            continue;
        }
        if (out) {
            out->base = start;
            out->size = w.size - (start - w.base);
            out->cls = w.cls;
        }
        ++c_.choose_hits;
        return true;
    }
    ++c_.choose_misses;
    return false;
}

bool Glue::TakeRecycled(PP::Class cls, uint64_t size, uint64_t align, uint64_t* va_out, std::string* why) {
    if (!enabled_ || cfg_.recycle == PP::VaRecycler::Mode::Off) {
        SetWhy(why, "recycler off");
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    ++c_.takes;
    uint64_t va = 0;
    const bool ok = recycler_.Take(cls, size, align, frame_, gpu_done_, &va, why);
    if (ok) {
        ++c_.takes_hit;
        if (va_out) {
            *va_out = va;
        }
    }
    return ok;
}

std::vector<PublishRange> Glue::PublishList(uint64_t* version) {
    std::vector<PublishRange> out;
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled_ && cfg_.place == PP::Mode::Apply) {
        uint64_t bytes = 0;
        for (const auto& kv : blocks_) {
            if (Publish(kv.second, kv.first, true) == PP::Class::CpuOnly) {
                PublishRange r;
                r.addr = kv.first;
                r.size = kv.second.size;
                r.set = kv.second.set;
                out.push_back(r);
                bytes += r.size;
            }
        }
        bool same = out.size() == published_.size();
        for (size_t i = 0; same && i < out.size(); ++i) {
            same = out[i].addr == published_[i].addr && out[i].size == published_[i].size;
        }
        if (!same) {
            ++version_;
            published_ = out;
        }
        last_publish_bytes_ = bytes;
    }
    if (version) {
        *version = version_;
    }
    return out;
}

void Glue::Report(std::string& out, double seconds) {
    if (!enabled_) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    const double secs = seconds > 0.0 ? seconds : 1.0;
    Appendf(out, "SOTC PC PLACE mode %s, recycle %d (quarantine %llu frames), windows %zu, frame %llu, gpu done %llu\n",
            PP::ModeName(cfg_.place), static_cast<int>(cfg_.recycle),
            static_cast<unsigned long long>(cfg_.quarantine_frames), windows_.size(),
            static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(gpu_done_));
    if (!cfg_.warnings.empty()) {
        Appendf(out, "SOTC PC PLACE warnings: %s\n", cfg_.warnings.c_str());
    }
    EnsureWindows(); // a run with no block yet still prints its windows
    ReportWindows(out);
    if (cfg_.place != PP::Mode::Off) {
        ReportPlace(out, secs);
    }
    if (cfg_.recycle != PP::VaRecycler::Mode::Off) {
        ReportRecycle(out);
    }
    last_report_ = c_;
}

void Glue::ReportPlace(std::string& out, double secs) {
    const bool windows = windows_.size() != 0;

    // Stage 1: the per-class table (observe: the decision, the ledger and the bind evidence; no window).
    uint64_t by_class_blocks[PP::kClassCount] = {};
    uint64_t by_class_bytes[PP::kClassCount] = {};
    uint64_t placed_blocks[PP::kClassCount] = {};
    uint64_t placed_bytes[PP::kClassCount] = {};
    uint64_t total_bytes = 0;
    uint64_t mixed_blocks = 0;
    uint64_t mixed_bytes = 0;
    uint64_t no_carve_blocks = 0;
    uint64_t bound_cpu_now = 0;
    uint64_t set_blocks[3] = {};
    uint64_t no_rule_blocks = 0;

    // Stage 2: the window predictions.
    uint64_t place_checked = 0;
    uint64_t place_mismatch[PP::kClassCount] = {};
    uint64_t pub_nowin_blocks = 0;
    uint64_t pub_nowin_bytes = 0;
    uint64_t pub_win_blocks = 0;
    uint64_t pub_win_bytes = 0;
    uint64_t lost_to_window = 0;

    for (const auto& kv : blocks_) {
        const uint64_t base = kv.first;
        const Block& b = kv.second;
        total_bytes += b.size;
        if (b.set >= 0 && b.set < 3) {
            ++set_blocks[b.set];
        }
        const PP::Class obs = Publish(b, base, false);
        ++by_class_blocks[static_cast<int>(obs)];
        by_class_bytes[static_cast<int>(obs)] += b.size;
        if (b.decision.place == PP::Class::Unknown) {
            ++no_rule_blocks;
        } else {
            ++placed_blocks[static_cast<int>(b.decision.place)];
            placed_bytes[static_cast<int>(b.decision.place)] += b.size;
        }
        if (ledger_.Conflicted(base)) {
            ++mixed_blocks;
            mixed_bytes += b.size;
        } else if (ledger_.Get(base) == PP::Class::Unknown) {
            ++no_carve_blocks;
        }
        if (b.bound && b.decision.publish == PP::Class::CpuOnly) {
            ++bound_cpu_now;
        }
        if (b.decision.place != PP::Class::Unknown) {
            ++place_checked;
            if (WindowClassOfBlock(base, b.size) != b.decision.place) {
                ++place_mismatch[static_cast<int>(b.decision.place)];
            }
        }
        const PP::Class pn = Publish(b, base, false);
        const PP::Class pw = Publish(b, base, true);
        if (pn == PP::Class::CpuOnly) {
            ++pub_nowin_blocks;
            pub_nowin_bytes += b.size;
            if (pw == PP::Class::CpuOnly) {
                ++pub_win_blocks;
                pub_win_bytes += b.size;
            } else {
                ++lost_to_window;
            }
        } else if (pw == PP::Class::CpuOnly) {
            ++pub_win_blocks;
            pub_win_bytes += b.size;
        }
    }

    Appendf(out, "SOTC PC PLACE blocks %zu (%.1f MiB), sets S0 %llu S1 %llu S2 %llu, no rule %llu\n", blocks_.size(),
            MiB(total_bytes), static_cast<unsigned long long>(set_blocks[0]),
            static_cast<unsigned long long>(set_blocks[1]), static_cast<unsigned long long>(set_blocks[2]),
            static_cast<unsigned long long>(no_rule_blocks));
    Appendf(out, "SOTC PC PLACE class (observe, no window): ");
    for (int i = 0; i < PP::kClassCount; ++i) {
        Appendf(out, "%s %llu blocks %.1f MiB%s", PP::ClassName(static_cast<PP::Class>(i)),
                static_cast<unsigned long long>(by_class_blocks[i]), MiB(by_class_bytes[i]),
                i + 1 < PP::kClassCount ? ", " : "\n");
    }
    Appendf(out, "SOTC PC PLACE placed as (decision): ");
    for (int i = 0; i < PP::kClassCount - 1; ++i) {
        Appendf(out, "%s %llu blocks %.1f MiB, ", PP::ClassName(static_cast<PP::Class>(i)),
                static_cast<unsigned long long>(placed_blocks[i]), MiB(placed_bytes[i]));
    }
    Appendf(out, "\n");
    Appendf(out, "SOTC PC PLACE mixed (ledger conflict) %llu blocks %.1f MiB, no carve evidence %llu blocks\n",
            static_cast<unsigned long long>(mixed_blocks), MiB(mixed_bytes),
            static_cast<unsigned long long>(no_carve_blocks));
    Appendf(out, "SOTC PC PLACE allocs %llu (%.1f MiB), no heap context %llu, dup %llu; frees %llu (unobserved %llu, "
                 "size mismatch %llu); carves %llu (unruled %llu, untracked %llu)\n",
            static_cast<unsigned long long>(c_.allocs), MiB(c_.alloc_bytes),
            static_cast<unsigned long long>(c_.no_heap_context), static_cast<unsigned long long>(c_.dup_allocs),
            static_cast<unsigned long long>(c_.frees), static_cast<unsigned long long>(c_.frees_unobserved),
            static_cast<unsigned long long>(c_.frees_size_mismatch), static_cast<unsigned long long>(c_.carves),
            static_cast<unsigned long long>(c_.carves_unruled), static_cast<unsigned long long>(c_.carves_untracked));
    Appendf(out, "SOTC PC PLACE binds %llu (%.0f/s, untracked %llu), blocks bound %llu, CPU_ONLY bound %llu "
                 "(windowed %llu, now %llu); writes %llu (%.0f/s, untracked %llu), CPU_ONLY written %llu\n",
            static_cast<unsigned long long>(c_.binds), static_cast<double>(c_.binds - last_report_.binds) / secs,
            static_cast<unsigned long long>(c_.binds_untracked), static_cast<unsigned long long>(c_.bound_blocks),
            static_cast<unsigned long long>(c_.cpu_only_bound),
            static_cast<unsigned long long>(c_.cpu_only_bound_windowed),
            static_cast<unsigned long long>(bound_cpu_now), static_cast<unsigned long long>(c_.write_faults),
            static_cast<double>(c_.write_faults - last_report_.write_faults) / secs,
            static_cast<unsigned long long>(c_.write_faults_untracked),
            static_cast<unsigned long long>(c_.cpu_only_write));

    if (cfg_.place == PP::Mode::Verify) {
        Appendf(out, "SOTC PC PLACE verify%s: placement checked %llu blocks, mismatch ",
                windows ? "" : " (NO WINDOWS: every placed block mismatches)",
                static_cast<unsigned long long>(place_checked));
        for (int i = 0; i < PP::kClassCount - 1; ++i) {
            Appendf(out, "%s %llu/%llu, ", PP::ClassName(static_cast<PP::Class>(i)),
                    static_cast<unsigned long long>(place_mismatch[i]),
                    static_cast<unsigned long long>(placed_blocks[i]));
        }
        Appendf(out, "\n");
        Appendf(out, "SOTC PC PLACE verify publish untracked: ignoring window %llu blocks %.1f MiB, with window "
                     "%llu blocks %.1f MiB, lost to the window %llu\n",
                static_cast<unsigned long long>(pub_nowin_blocks), MiB(pub_nowin_bytes),
                static_cast<unsigned long long>(pub_win_blocks), MiB(pub_win_bytes),
                static_cast<unsigned long long>(lost_to_window));
    }
    if (cfg_.place == PP::Mode::Apply) {
        Appendf(out, "SOTC PC PLACE apply: publish list %zu ranges %.1f MiB (version %llu), ChooseWindow hits %llu "
                     "misses %llu (since start)\n",
                published_.size(), MiB(last_publish_bytes_), static_cast<unsigned long long>(version_),
                static_cast<unsigned long long>(c_.choose_hits), static_cast<unsigned long long>(c_.choose_misses));
    }

}

// The windows in use, with the live blocks whose base lies in each one. The auto plan is fixed before any class byte is
// seen, so this is the evidence for resizing the windows through SOTC_PC_PLACE_WINDOWS.
void Glue::ReportWindows(std::string& out) {
    const char* src = cfg_.windows_auto ? "auto" : (cfg_.windows_spec.empty() ? "off" : "env");
    Appendf(out, "SOTC PC PLACE windows (%s): %zu\n", src, windows_.size());
    if (!windows_note_.empty()) {
        Appendf(out, "SOTC PC PLACE windows note: %s\n", windows_note_.c_str());
    }
    for (const PP::VaWindow& w : windows_.windows()) {
        uint64_t n = 0;
        uint64_t bytes = 0;
        for (auto it = blocks_.lower_bound(w.base); it != blocks_.end() && it->first < w.base + w.size; ++it) {
            ++n;
            bytes += it->second.size;
        }
        Appendf(out, "SOTC PC PLACE window 0x%llx-0x%llx %s %.0f MiB: blocks %llu (%.1f MiB live)\n",
                static_cast<unsigned long long>(w.base), static_cast<unsigned long long>(w.base + w.size),
                PP::ClassName(w.cls), MiB(w.size), static_cast<unsigned long long>(n), MiB(bytes));
    }
}

void Glue::ReportRecycle(std::string& out) {
    {
        const PP::VaRecycler::Stats& s = recycler_.stats();
        Appendf(out, "SOTC PC RECYCLE retired %llu runs (%.1f MiB), held now %zu runs %.1f MiB, taken %llu (%.1f MiB), "
                     "would reuse %llu, takes %llu hit %llu\n",
                static_cast<unsigned long long>(s.retired), MiB(s.retired_bytes), recycler_.runs(),
                MiB(recycler_.retired_bytes_now()), static_cast<unsigned long long>(s.taken), MiB(s.taken_bytes),
                static_cast<unsigned long long>(s.would_reuse), static_cast<unsigned long long>(c_.takes),
                static_cast<unsigned long long>(c_.takes_hit));
        Appendf(out, "SOTC PC RECYCLE refused: retire %llu, quarantine %llu, gpu %llu, invalid %llu; merged %llu; "
                     "size mismatch frees %llu\n",
                static_cast<unsigned long long>(s.refused_retire),
                static_cast<unsigned long long>(s.refused_quarantine), static_cast<unsigned long long>(s.refused_gpu),
                static_cast<unsigned long long>(s.refused_invalid), static_cast<unsigned long long>(s.merged),
                static_cast<unsigned long long>(c_.frees_size_mismatch));
    }
}

// ---- process-wide glue --------------------------------------------------------------------------------

Glue& Global() {
    static Glue glue(ConfigFromEnv());
    return glue;
}

} // namespace SotcPcPlaceGlue
