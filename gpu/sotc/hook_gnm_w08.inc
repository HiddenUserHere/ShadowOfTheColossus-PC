// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W08 (Shader binds and user data); callbacks live in
// gpu/native/gnm/w08_shaders.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW08_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW08_869990, "0x869990 W08 name", "native-gnm" },
// The sceGnm* entries of W08 need no detour: they are set in RegisterW08 (boundary 2).
#ifdef SOTC_GNM_DECLS
#include "native/gnm/w08_shaders.h"
// 0x869630: SET_SH_REG x7 (seven dwords passed as one 28-byte struct on the guest stack).
extern "C" __attribute__((sysv_abi)) void GnmW08_869630(void* cb, SotcW08Stack7 s);
// 0x862490: user-data writer of one stage (rdi = state, esi = stage, rdx = values).
extern "C" __attribute__((sysv_abi)) void GnmW08_862490(void* state, int stage,
                                                        const unsigned int* values);
// 0x86d4c0: SET_SH_REG x2 of a 64-bit value (rcx), one stage (rdi = cb, esi = stage, edx = p3).
extern "C" __attribute__((sysv_abi)) void GnmW08_86d4c0(void* cb, unsigned int stage, int p3,
                                                        unsigned long long value);
// 0x86d550: SET_SH_REG x1 (ecx), one stage.
extern "C" __attribute__((sysv_abi)) void GnmW08_86d550(void* cb, unsigned int stage, int p3,
                                                        int p4);
// 0x86d5e0: stage user-data writer, count values at `values` (r8d = count).
extern "C" __attribute__((sysv_abi)) void GnmW08_86d5e0(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values, int count);
// 0x86b810: opcode 0x8d packet with a NOP; nine integer args (seven in registers, three on the stack).
extern "C" __attribute__((sysv_abi)) void GnmW08_86b810(void* cb, unsigned int p2,
                                                        unsigned int p3, int p4, int p5, int p6,
                                                        unsigned int p7, unsigned int p8, int p9);
// 0x8695d0: SET_SH_REG of `count` values (rcx, r8d); esi & 1 = the compute-packet bit (no static callers).
extern "C" __attribute__((sysv_abi)) void GnmW08_8695d0(void* cb, unsigned int flag, int p3,
                                                        const unsigned int* values,
                                                        unsigned int count);
// 0x869710: SET_SH_REG of one compute value (p2 esi, p3 edx, p4 ecx); no static callers.
extern "C" __attribute__((sysv_abi)) void GnmW08_869710(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4);
// 0x869790 / 0x869810: SET_SH_REG of p3 & 0xffff at the compute offset of stage p2; no static callers.
extern "C" __attribute__((sysv_abi)) void GnmW08_869790(void* cb, unsigned int p2, unsigned int p3);
extern "C" __attribute__((sysv_abi)) void GnmW08_869810(void* cb, unsigned int p2, unsigned int p3);
// 0x869910: SET_SH_REG of one compute value at offset 0x218; no static callers.
extern "C" __attribute__((sysv_abi)) void GnmW08_869910(void* cb, unsigned int p2, unsigned int p3);
// 0x86d2e0 / 0x86d420 (four values) and 0x86d380 (eight values): stage writers, values at rcx.
extern "C" __attribute__((sysv_abi)) void GnmW08_86d2e0(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values);
extern "C" __attribute__((sysv_abi)) void GnmW08_86d380(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values);
extern "C" __attribute__((sysv_abi)) void GnmW08_86d420(void* cb, unsigned int stage, int p3,
                                                        const unsigned int* values);
#else
    { 0x869630, (void*)&GnmW08_869630, "0x869630 W08 SetShReg7", "native-gnm:shaders" },
    { 0x862490, (void*)&GnmW08_862490, "0x862490 W08 SetUserData", "native-gnm:shaders" },
    { 0x86d4c0, (void*)&GnmW08_86d4c0, "0x86d4c0 W08 SetShReg2", "native-gnm:shaders" },
    { 0x86d550, (void*)&GnmW08_86d550, "0x86d550 W08 SetShReg1", "native-gnm:shaders" },
    { 0x86d5e0, (void*)&GnmW08_86d5e0, "0x86d5e0 W08 SetShRegN", "native-gnm:shaders" },
    { 0x86b810, (void*)&GnmW08_86b810, "0x86b810 W08 Op8d", "native-gnm:shaders" },
    { 0x8695d0, (void*)&GnmW08_8695d0, "0x8695d0 W08 SetShRegFlagN", "native-gnm:shaders" },
    { 0x869710, (void*)&GnmW08_869710, "0x869710 W08 SetShRegCs215", "native-gnm:shaders" },
    { 0x869790, (void*)&GnmW08_869790, "0x869790 W08 SetShRegSlot", "native-gnm:shaders" },
    { 0x869810, (void*)&GnmW08_869810, "0x869810 W08 SetShRegSlot2", "native-gnm:shaders" },
    { 0x869910, (void*)&GnmW08_869910, "0x869910 W08 SetShRegCs218", "native-gnm:shaders" },
    { 0x86d2e0, (void*)&GnmW08_86d2e0, "0x86d2e0 W08 SetShReg4Stage", "native-gnm:shaders" },
    { 0x86d380, (void*)&GnmW08_86d380, "0x86d380 W08 SetShReg8Stage", "native-gnm:shaders" },
    { 0x86d420, (void*)&GnmW08_86d420, "0x86d420 W08 SetShReg4StageB", "native-gnm:shaders" },
#endif
