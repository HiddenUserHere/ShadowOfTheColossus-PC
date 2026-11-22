// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend, GNM boundary W13: INDIRECT_BUFFER (class Ib).
//
// Coverage (VA | contract | native op | evidence):
//   0x884c40 | IT_INDIRECT_BUFFER on a compute ring: cb in rdi, buffer address in rsi (64-bit),
//            size in edx (& 0xfffff dwords); writes 0xc0023f02, addr lo, addr hi & 0xffff,
//            size | 0x1800000 (cache policy), cursor += 4 dwords | StreamOps::EmitIb: the compute
//            parser runs the buffer as ProcessCompute<true> on the same queue | raw bytes
//            0x884c40-0x884cb0 (out/eboot.elf), decomp 0088/00884c40.c (caller 0x85d3d0, the
//            bulk loop that queues compute buffers into the ASC ring before sceGnmDingDong)
//   0x86fa00 | the same on the graphics ring: 0xc0023f00, size | 0x1900000 | EmitIb: the
//            graphics parser runs it as a nested ProcessGraphics | raw bytes 0x86fa00-0x86fa70
// The cache-policy bits (23-24) have no effect in the port's IndirectBuffer handler.

#include "native/gnm/gnm_common.h"

using Native::Gnm::Class;

namespace {
using IbFn = void(__attribute__((sysv_abi)) *)(void*, unsigned long long, unsigned long long);

void EmitOrGuest(u64 va, const char* name, void* cb, unsigned long long address, unsigned long long size) {
    const u32 site = Native::Gnm::Site(name);
    const u32 dwords = static_cast<u32>(size) & 0xfffffu;
    // The guest masks the high address word to 16 bits (movzx ecx, cx).
    const u64 addr = address & 0x0000ffffffffffffull;
    if (Native::Gnm::Enabled(Class::Ib) && !Native::Gnm::ForceFallback(va) &&
        Native::StreamOps::EmitIb(reinterpret_cast<u64>(cb), addr, dwords)) {
        Native::Stats::SiteNative(site);
        return;
    }
    Native::Stats::SiteFallback(site);
    Native::Gnm::Guest<IbFn>(va)(cb, address, size);
}
} // namespace

extern "C" __attribute__((sysv_abi)) void GnmW13_884c40(void* cb, unsigned long long address,
                                                       unsigned long long size) {
    EmitOrGuest(0x884c40, "w13:884c40", cb, address, size);
}

extern "C" __attribute__((sysv_abi)) void GnmW13_86fa00(void* cb, unsigned long long address,
                                                       unsigned long long size) {
    EmitOrGuest(0x86fa00, "w13:86fa00", cb, address, size);
}
