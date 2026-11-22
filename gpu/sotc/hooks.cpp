// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Entry-hook implementation (see hooks.h).
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <Zydis/Zydis.h>

#include "common/logging/log.h"
#include "sotc/hooks.h"
#include "sotc/hook_table.inc"

namespace Core::SotcHooks {

namespace {

// Original-entry thunks, by guest VA, so a callback can chain to the guest body. Written once by
// Install before the game starts, read only afterwards (by callbacks on several guest threads),
// so readers take no lock.
std::unordered_map<u64, void*> g_original;

// Child-call trampolines (GuestOriginal on a VA whose entry is NOT hooked): a chained pass hook
// may need the guest draw 0x93690 while that entry has no hook of its own. The call jumps to the
// entry's first instruction, so the prologue is replayed from a trampoline as Install does it.
std::mutex g_child_mutex;
std::map<u64, void*> g_original_children;
u8* g_tramp_cursor = nullptr;
u8* g_tramp_end = nullptr;
void* g_linked_image = nullptr;

// A function entry shorter than this cannot hold the absolute indirect jump; its first
// instructions are relocated into a trampoline instead.
constexpr size_t kAbsJumpSize = 14; // FF 25 00 00 00 00 + 8-byte address

/// Decodes the instruction lengths at `code` until at least `bytes` are covered. Returns the total
/// length (>= bytes), or 0 when decoding fails or the entry is too short overall.
size_t MeasurePrologue(u8* code, size_t bytes, size_t max_scan) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t total = 0;
    while (total < bytes) {
        if (total >= max_scan) {
            return 0;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + total, max_scan - total,
                                                 &instruction, operands))) {
            return 0;
        }
        if (instruction.length == 0) {
            return 0;
        }
        total += instruction.length;
    }
    return total;
}

/// Copies the prologue from `src` to `dst` (which are `delta = dst - src` apart), fixing up any
/// RIP-relative memory operand so it still points at the same absolute target. Returns false when
/// the prologue contains a relative branch (which would need widening) or cannot be decoded.
bool RelocatePrologue(u8* dst, const u8* src, size_t length, ptrdiff_t delta) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t pos = 0;
    while (pos < length) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, src + pos, length - pos, &instruction,
                                                 operands))) {
            return false;
        }
        // A relative branch in the prologue would need its displacement recomputed and possibly
        // widened; refuse rather than miscompile. (RenderJob prologues are push/mov/lea only.)
        if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_CALL) {
            return false;
        }
        std::memcpy(dst + pos, src + pos, instruction.length);
        // RIP-relative operand: adjust the 32-bit displacement by -delta.
        s32 disp = 0;
        for (u32 i = 0; i < instruction.operand_count_visible; ++i) {
            const ZydisDecodedOperand& op = operands[i];
            if (op.type != ZYDIS_OPERAND_TYPE_MEMORY) {
                continue;
            }
            if (op.mem.base != ZYDIS_REGISTER_RIP || !op.mem.disp.has_displacement) {
                continue;
            }
            // The displacement is always 32 bits in an address-size-64 memory operand; the raw
            // bytes hold it at instruction.raw.disp.offset.
            std::memcpy(&disp, src + pos + instruction.raw.disp.offset, sizeof(s32));
            disp -= delta;
            std::memcpy(dst + pos + instruction.raw.disp.offset, &disp, sizeof(s32));
        }
        pos += instruction.length;
    }
    return pos == length;
}

/// Writes `jmp qword ptr [rip+0]; dq target` at `at` (14 bytes).
void WriteAbsJump(u8* at, void* target) {
    u8* p = at;
    *p++ = 0xff;
    *p++ = 0x25;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    std::memcpy(p, &target, sizeof(target));
}

/// The trampoline that replays `guest_va`'s own prologue and then continues in its body, made on
/// first use from the free space Install left after the installed hooks. Null (and a trace) when
/// the image is unknown, the prologue cannot be decoded/relocated, or the space ran out.
void* ChildOriginal(u64 guest_va) {
    std::lock_guard lock{g_child_mutex};
    if (const auto it = g_original_children.find(guest_va); it != g_original_children.end()) {
        return it->second;
    }
    if (g_linked_image == nullptr) {
        return nullptr;
    }
    u8* entry = static_cast<u8*>(g_linked_image) + guest_va;
    const size_t prologue = MeasurePrologue(entry, kAbsJumpSize, 64);
    if (prologue == 0 || g_tramp_cursor == nullptr ||
        g_tramp_cursor + prologue + kAbsJumpSize > g_tramp_end) {
        std::fprintf(stderr, "SOTC hook: no child trampoline for guest %#llx\n",
                     static_cast<unsigned long long>(guest_va));
        return nullptr;
    }
    u8* slot = g_tramp_cursor;
    const auto delta = reinterpret_cast<std::ptrdiff_t>(slot) -
                       reinterpret_cast<std::ptrdiff_t>(entry);
    if (!RelocatePrologue(slot, entry, prologue, delta)) {
        return nullptr;
    }
    WriteAbsJump(slot + prologue, entry + prologue);
    g_tramp_cursor += prologue + kAbsJumpSize;
    g_original_children[guest_va] = slot;
    return slot;
}

} // namespace

u64 Install(void* image, u64 module_size, void* trampoline, u64 trampoline_size,
            std::span<const Hook> hooks, u64* failed, std::span<InstalledHook> out) {
    if (failed) {
        *failed = 0;
    }
    if (!image || hooks.empty()) {
        return 0;
    }
    u8* tramp = static_cast<u8*>(trampoline);
    size_t tramp_used = 0;
    u64 installed = 0;
    for (const Hook& hook : hooks) {
        if (hook.guest_va >= module_size || !hook.host_callback) {
            if (failed) {
                ++*failed;
            }
            continue;
        }
        u8* entry = static_cast<u8*>(image) + hook.guest_va;
        // The 14-byte jump overwrites whole instructions. Measure the prologue it covers so a
        // relocated copy can replay it after the hook. The prologue may be longer than 14 bytes
        // when an instruction straddles the boundary; those extra bytes become dead at the entry
        // (the jump transfers control away) but are still replayed by the original thunk.
        const size_t prologue = MeasurePrologue(entry, kAbsJumpSize, 64);
        if (prologue == 0) {
            LOG_WARNING(Core, "SOTC hook {}: cannot decode a prologue at {:#x}", hook.name,
                        hook.guest_va);
            if (failed) {
                ++*failed;
            }
            continue;
        }
        // The original-entry thunk: the relocated prologue, then a jump to entry+prologue. The
        // prologue is relocated (RIP-relative operands fixed) because it runs at a new address.
        if (!tramp || tramp_used + prologue + kAbsJumpSize > trampoline_size) {
            LOG_WARNING(Core, "SOTC hook {}: trampoline exhausted", hook.name);
            if (failed) {
                ++*failed;
            }
            continue;
        }
        u8* slot = tramp + tramp_used;
        const auto delta = reinterpret_cast<std::ptrdiff_t>(slot) -
                           reinterpret_cast<std::ptrdiff_t>(entry);
        if (!RelocatePrologue(slot, entry, prologue, delta)) {
            LOG_WARNING(Core, "SOTC hook {}: prologue at {:#x} needs relocation this trampoline "
                        "does not support", hook.name, hook.guest_va);
            if (failed) {
                ++*failed;
            }
            continue;
        }
        WriteAbsJump(slot + prologue, entry + prologue);
        tramp_used += prologue + kAbsJumpSize;
        g_original[hook.guest_va] = slot;
        if (!out.empty() && installed < out.size()) {
            out[installed] = InstalledHook{&hook, slot};
        }
        WriteAbsJump(entry, hook.host_callback);
        ++installed;
    }
    // What is left of the trampoline area serves GuestOriginal (child calls of hooked bodies).
    g_linked_image = image;
    g_tramp_cursor = tramp + tramp_used;
    g_tramp_end = tramp + trampoline_size;
    return installed;
}

void* OriginalEntry(u64 guest_va) {
    const auto it = g_original.find(guest_va);
    if (it != g_original.end()) {
        return it->second;
    }
    // A VA whose entry is not hooked is still behind its own prologue (the child-call path here).
    if (g_linked_image != nullptr) {
        return ChildOriginal(guest_va);
    }
    return nullptr;
}

std::span<const Hook> RenderJobHooks() {
    static const std::vector<Hook> hooks = [] {
        std::vector<Hook> v;
        // sotc: N2-109: the validated native render is the default (runs 62-75, N2-108): an unset
        // SOTC_RENDER means "native-pass,native-gnm"; SOTC_RENDER=off (or any unknown stage name)
        // installs nothing, the emulated path.
        const char* stage = std::getenv("SOTC_RENDER");
        if (!stage) {
            stage = "native-pass,native-gnm";
        }
        if (stage[0] == '\0') {
            return v;
        }
        // hook_table.inc lists every RenderJob entry hook and the stage that enables it; the
        // stage named by SOTC_RENDER selects the subset to install. One entry can be listed under
        // several stages (0x93ef0: the pass stage and the shadow stage both need it); the first
        // matching row in table order wins, so one detour is installed per entry.
        // SOTC_RENDER may name several stages separated by commas (e.g. "native-pass,native-gnm").
        auto stage_wanted = [stage](const char* name) {
            const std::size_t len = std::strlen(name);
            for (const char* p = stage; *p != '\0';) {
                const char* end = std::strchr(p, ',');
                const std::size_t n = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
                if ((n == len && std::strncmp(p, name, len) == 0) ||
                    (n == 6 && std::strncmp(p, "native", 6) == 0)) {
                    return true;
                }
                if (end == nullptr) {
                    break;
                }
                p = end + 1;
            }
            return false;
        };
        // GNM boundary rows carry their class: "native-gnm:<class>". They are installed only when
        // the native-gnm stage is named AND the class is on in SOTC_NATIVE_GNM (comma list or
        // "all"): a class that is off keeps the guest function untouched, so no detour signature
        // can affect it (gap N2-73: a detour declared with the wrong ABI corrupted the guest even
        // when it only forwarded to the guest body).
        auto gnm_class_on = [](const char* cls) {
            // sotc: N2-109: unset = every class (as Native::Gnm::ParseMask); empty = none.
            const char* list = std::getenv("SOTC_NATIVE_GNM");
            if (list == nullptr) {
                list = "all";
            }
            if (list[0] == '\0') {
                return false;
            }
            if (std::strcmp(list, "all") == 0) {
                return true;
            }
            const std::size_t len = std::strlen(cls);
            for (const char* p = list; *p != '\0';) {
                const char* end = std::strchr(p, ',');
                const std::size_t n = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
                if (n == len && std::strncmp(p, cls, len) == 0) {
                    return true;
                }
                if (end == nullptr) {
                    break;
                }
                p = end + 1;
            }
            return false;
        };
        // SOTC_NATIVE_GNM_SKIP: comma list of guest VAs (hex) whose rows are never installed, to
        // bisect a single detour inside a class.
        auto va_skipped = [](u64 va) {
            const char* list = std::getenv("SOTC_NATIVE_GNM_SKIP");
            for (const char* p = list; p != nullptr && *p != '\0';) {
                char* end = nullptr;
                const u64 v = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                if (v == va) {
                    return true;
                }
                p = (*end == ',') ? end + 1 : end;
            }
            return false;
        };
        for (const RenderJobHookEntry& e : kRenderJobHookTable) {
            if (va_skipped(e.va)) {
                continue;
            }
            const char* colon = std::strchr(e.stage, ':');
            if (colon != nullptr) {
                const std::string base(e.stage, static_cast<std::size_t>(colon - e.stage));
                if (!stage_wanted(base.c_str()) || !gnm_class_on(colon + 1)) {
                    continue;
                }
            } else if (!stage_wanted(e.stage)) {
                continue;
            }
            bool listed = false;
            for (const Hook& h : v) {
                listed = listed || h.guest_va == e.va;
            }
            if (!listed) {
                v.push_back(Hook{e.va, e.callback, e.name});
            }
        }
        return v;
    }();
    return hooks;
}

} // namespace Core::SotcHooks
