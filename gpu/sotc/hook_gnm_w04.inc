// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W04 (Render targets); callbacks live in
// gpu/native/gnm/w04_targets.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW04_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW04_869990, "0x869990 W04 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
extern "C" __attribute__((sysv_abi)) void GnmW04_86a1a0(void* cb, int slot, const void* desc);
extern "C" __attribute__((sysv_abi)) void GnmW04_86a290(void* cb, const void* desc);
extern "C" __attribute__((sysv_abi)) void GnmW04_86e200(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW04_86e260(void* cb, unsigned int hi, unsigned int lo);
extern "C" __attribute__((sysv_abi)) void GnmW04_86e340(void* cb, float value);
extern "C" __attribute__((sysv_abi)) void GnmW04_86f0c0(void* cb, unsigned long long addr,
                                                        unsigned int a, unsigned int b);
extern "C" __attribute__((sysv_abi)) void GnmW04_86f170(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW04_86fbd0(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW04_86fc90(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW04_86fcf0(void* cb, unsigned int a, unsigned int b);
extern "C" __attribute__((sysv_abi)) void GnmW04_86fd60(void* cb, unsigned int value);
#else
{ 0x86a1a0, (void*)&GnmW04_86a1a0, "0x86a1a0 W04 CB_COLORn surface block", "native-gnm:targets" },
{ 0x86a290, (void*)&GnmW04_86a290, "0x86a290 W04 DB block", "native-gnm:targets" },
{ 0x86e200, (void*)&GnmW04_86e200, "0x86e200 W04 SET_CONTEXT_REG 0x282", "native-gnm:targets" },
{ 0x86e260, (void*)&GnmW04_86e260, "0x86e260 W04 SET_CONTEXT_REG 0x280 pair", "native-gnm:targets" },
{ 0x86e340, (void*)&GnmW04_86e340, "0x86e340 W04 SET_CONTEXT_REG 0x2df float", "native-gnm:targets" },
{ 0x86f0c0, (void*)&GnmW04_86f0c0, "0x86f0c0 W04 SET_CONTEXT_REG 0x2cc/0x2ca + COPY_DATA", "native-gnm:targets" },
{ 0x86f170, (void*)&GnmW04_86f170, "0x86f170 W04 SET_CONTEXT_REG 0x2e5 u8", "native-gnm:targets" },
{ 0x86fbd0, (void*)&GnmW04_86fbd0, "0x86fbd0 W04 SET_CONTEXT_REG 0x2a6", "native-gnm:targets" },
{ 0x86fc90, (void*)&GnmW04_86fc90, "0x86fc90 W04 SET_CONTEXT_REG 0x2d4", "native-gnm:targets" },
{ 0x86fcf0, (void*)&GnmW04_86fcf0, "0x86fcf0 W04 SET_CONTEXT_REG 0x20d bits", "native-gnm:targets" },
{ 0x86fd60, (void*)&GnmW04_86fd60, "0x86fd60 W04 SET_CONTEXT_REG 0xd8 bit31", "native-gnm:targets" },
#endif
