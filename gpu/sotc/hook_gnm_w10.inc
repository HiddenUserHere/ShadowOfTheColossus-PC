// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W10 (Dispatches and compute queues); callbacks live in
// gpu/native/gnm/w10_dispatch.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW10_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW10_869990, "0x869990 W10 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
// 0x883730: NOP 0xc0051000 (rdi = cb only).
extern "C" __attribute__((sysv_abi)) void GnmW10_883730(void* cb);
// 0x883d60 / 0x883de0: SET_SH_REG of a thread-management register (rdi = cb, esi = stage index,
// edx = value). Two copies of the same writer; 0x85c580 calls both forms.
extern "C" __attribute__((sysv_abi)) void GnmW10_883d60(void* cb, unsigned int index,
                                                        unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW10_883de0(void* cb, unsigned int index,
                                                        unsigned int value);
// 0x8841d0: SET_SH_REG of two values (rdi = cb, esi = reg_base, rdx = 64-bit value).
extern "C" __attribute__((sysv_abi)) void GnmW10_8841d0(void* cb, unsigned int reg_base,
                                                        unsigned long long value);
// 0x884240: NOP + SET_SH_REG of count values (rdi = cb, esi = reg_base, rdx = values, ecx = count).
extern "C" __attribute__((sysv_abi)) void GnmW10_884240(void* cb, unsigned int reg_base,
                                                        const unsigned int* values, int count);
// 0x884360: SET_SH_REG of one value (rdi = cb, esi = reg_base, edx = value).
extern "C" __attribute__((sysv_abi)) void GnmW10_884360(void* cb, unsigned int reg_base,
                                                        unsigned int value);
// 0x8832b0: SET_SH_REG of count values at the absolute SH register (rdi = cb, esi = reg_abs,
// rdx = values pointer, ecx = count).
extern "C" __attribute__((sysv_abi)) void GnmW10_8832b0(void* cb, unsigned int reg_abs,
                                                        const unsigned int* values,
                                                        unsigned int count);
// 0x883780: SET_SH_REG of compute register 0x218 (rdi = cb, esi = a, edx = b).
extern "C" __attribute__((sysv_abi)) void GnmW10_883780(void* cb, unsigned int a, unsigned int b);
// 0x883ce0: SET_SH_REG of compute register 0x215 (rdi = cb, esi = a, edx = b, ecx = c).
extern "C" __attribute__((sysv_abi)) void GnmW10_883ce0(void* cb, unsigned int a, unsigned int b,
                                                        unsigned int c);
// 0x884050 / 0x8840d0 / 0x884150: SET_SH_REG of user data at reg_base + 0x240 (rdi = cb,
// esi = reg_base, rdx = values pointer, 64-bit).
extern "C" __attribute__((sysv_abi)) void GnmW10_884050(void* cb, unsigned int reg_base,
                                                        const unsigned long long* values);
extern "C" __attribute__((sysv_abi)) void GnmW10_8840d0(void* cb, unsigned int reg_base,
                                                        const unsigned long long* values);
extern "C" __attribute__((sysv_abi)) void GnmW10_884150(void* cb, unsigned int reg_base,
                                                        const unsigned long long* values);
#else
    { 0x883730, (void*)&GnmW10_883730, "0x883730 W10 NOP7", "native-gnm:dispatch" },
    { 0x883d60, (void*)&GnmW10_883d60, "0x883d60 W10 ThreadMgmt", "native-gnm:dispatch" },
    { 0x883de0, (void*)&GnmW10_883de0, "0x883de0 W10 ThreadMgmt Neo", "native-gnm:dispatch" },
    { 0x8841d0, (void*)&GnmW10_8841d0, "0x8841d0 W10 SetShReg2", "native-gnm:dispatch" },
    { 0x884240, (void*)&GnmW10_884240, "0x884240 W10 SetShRegN", "native-gnm:dispatch" },
    { 0x884360, (void*)&GnmW10_884360, "0x884360 W10 SetShReg1", "native-gnm:dispatch" },
    { 0x8832b0, (void*)&GnmW10_8832b0, "0x8832b0 W10 SetShRegN compute", "native-gnm:dispatch" },
    { 0x883780, (void*)&GnmW10_883780, "0x883780 W10 SetShReg218", "native-gnm:dispatch" },
    { 0x883ce0, (void*)&GnmW10_883ce0, "0x883ce0 W10 ResourceLimits", "native-gnm:dispatch" },
    { 0x884050, (void*)&GnmW10_884050, "0x884050 W10 UserData4", "native-gnm:dispatch" },
    { 0x8840d0, (void*)&GnmW10_8840d0, "0x8840d0 W10 UserData8", "native-gnm:dispatch" },
    { 0x884150, (void*)&GnmW10_884150, "0x884150 W10 UserData4b", "native-gnm:dispatch" },
#endif
