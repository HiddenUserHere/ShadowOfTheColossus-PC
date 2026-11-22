// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary. See gnm_common.h.

#include "native/gnm/gnm_common.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "native/guest_bridge.h"
#include "sotc/hooks.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Native::Gnm {

namespace {

constexpr const char* kClassNames[static_cast<u32>(Class::Count)] = {
    "cb",       "db",    "raster", "targets", "uconfig", "memory",
    "sync",     "shaders", "draws", "dispatch", "ce",     "submit", "ib",
};

u32 ParseMask() {
    // sotc: N2-109: every class is on by default (SOTC_NATIVE_GNM unset = "all"); an empty value
    // turns every class off.
    const char* e = std::getenv("SOTC_NATIVE_GNM");
    if (e == nullptr) {
        e = "all";
    }
    if (e[0] == '\0') {
        return 0;
    }
    if (std::strcmp(e, "all") == 0) {
        return (1u << static_cast<u32>(Class::Count)) - 1;
    }
    u32 mask = 0;
    const char* p = e;
    while (*p != '\0') {
        const char* end = std::strchr(p, ',');
        const std::size_t len = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
        for (u32 i = 0; i < static_cast<u32>(Class::Count); ++i) {
            if (std::strlen(kClassNames[i]) == len && std::strncmp(p, kClassNames[i], len) == 0) {
                mask |= 1u << i;
            }
        }
        if (end == nullptr) {
            break;
        }
        p = end + 1;
    }
    return mask;
}

} // namespace

bool Enabled(Class c) {
    static const u32 mask = ParseMask();
    return (mask >> static_cast<u32>(c)) & 1u;
}

bool ForceFallback(u64 va) {
    const char* list = std::getenv("SOTC_NATIVE_GNM_FALLBACK");
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
}

AmdGpu::Liverpool& Lv() {
    return presenter->GetLiverpool();
}

void* GuestEntry(u64 va) {
    return reinterpret_cast<void*>(Guest::GuestImageBase() + va);
}

void* GuestBody(u64 va) {
    return Core::SotcHooks::OriginalEntry(va);
}

namespace {
std::mutex g_patch_mutex;
std::unordered_map<u32, VAddr> g_patches;
} // namespace

void SetIndirectPatch(u32 op_id_dword, VAddr src) {
    std::scoped_lock lock{g_patch_mutex};
    g_patches[op_id_dword & ~StreamOps::kIdFlagMask] = src;
}

bool TakeIndirectPatch(u32 op_id_dword, VAddr& src) {
    std::scoped_lock lock{g_patch_mutex};
    const auto it = g_patches.find(op_id_dword & ~StreamOps::kIdFlagMask);
    if (it == g_patches.end()) {
        return false;
    }
    src = it->second;
    g_patches.erase(it);
    return true;
}

const Table& GetTable() {
    static const Table table = [] {
        Table t{};
        RegisterW01(t);
        RegisterW02(t);
        RegisterW03(t);
        RegisterW04(t);
        RegisterW05(t);
        RegisterW06(t);
        RegisterW07(t);
        RegisterW08(t);
        RegisterW09(t);
        RegisterW10(t);
        RegisterW11(t);
        RegisterW12(t);
        return t;
    }();
    return table;
}

bool Shadow::CsRange(u32 off, u32 count) {
    constexpr u32 kCsWords = sizeof(AmdGpu::ComputeProgram) / 4;
    return count != 0 && off < 0x200u + kCsWords && off + count > 0x200u;
}

Shadow& Shadow::Sh(u32 off, const u32* vals, u32 count) {
    if (!CsRange(off, count) || vals == nullptr || n + 2 + count > kMaxDwords) {
        return *this;
    }
    dw[n++] = 0xc0007600u | (count << 16); // SET_SH_REG, header count = body - 1 = values
    dw[n++] = off & 0xffffu;
    for (u32 i = 0; i < count; ++i) {
        dw[n++] = vals[i];
    }
    return *this;
}

Shadow& Shadow::DispatchDirect(u32 x, u32 y, u32 z, u32 initiator) {
    if (n + 5 > kMaxDwords) {
        return *this;
    }
    dw[n++] = 0xc0031500u; // DISPATCH_DIRECT, 4 body dwords
    dw[n++] = x;
    dw[n++] = y;
    dw[n++] = z;
    dw[n++] = initiator;
    return *this;
}

Shadow& Shadow::DispatchIndirect(u32 data_offset, u32 initiator) {
    if (n + 3 > kMaxDwords) {
        return *this;
    }
    dw[n++] = 0xc0011600u; // DISPATCH_INDIRECT, 2 body dwords
    dw[n++] = data_offset;
    dw[n++] = initiator;
    return *this;
}

bool EmitCpShadow(u64 cb, Common::UniqueFunction<void>&& op, const Shadow& shadow,
                  bool writes_memory) {
    if (shadow.n == 0) {
        return EmitCp(cb, std::move(op), writes_memory);
    }
    const u64 body = StreamOps::EmitWithSpace(cb, std::move(op), 1 + shadow.n, 4, writes_memory,
                                              StreamOps::Where::CommandProcessor);
    if (body == 0) {
        return false;
    }
    u32* out = reinterpret_cast<u32*>(body);
    out[0] = StreamOps::kShadowTag;
    std::memcpy(out + 1, shadow.dw.data(), shadow.n * sizeof(u32));
    return true;
}

bool EmitCpInSlotShadow(u32* slot, u32 size_dwords, Common::UniqueFunction<void>&& op,
                        const Shadow& shadow, bool writes_memory) {
    if (!EmitCpInSlot(slot, size_dwords, std::move(op), writes_memory)) {
        return false;
    }
    if (shadow.n != 0 && 4 + shadow.n <= size_dwords) {
        slot[3] = StreamOps::kShadowTag;
        std::memcpy(slot + 4, shadow.dw.data(), shadow.n * sizeof(u32));
    }
    return true;
}

} // namespace Native::Gnm
