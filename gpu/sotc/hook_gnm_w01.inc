// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W01 (Context registers: colour / blend); callbacks live in
// gpu/native/gnm/w01_ctx_cb.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm").
#ifdef SOTC_GNM_DECLS
extern "C" __attribute__((sysv_abi)) void GnmW01_869990(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW01_869a50(void* cb, int index, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW01_869ce0(void* cb, unsigned int mode,
                                                         unsigned int rop);
extern "C" __attribute__((sysv_abi)) void GnmW01_869ac0(void* cb, float red, float green,
                                                         float blue, float alpha);
// Round 3 (X1): context emitters A. Signatures from the raw prologues (gnm_abi_audit.py).
extern "C" __attribute__((sysv_abi)) void GnmW01_869370(void* cb, unsigned int reg_abs,
                                                         unsigned int value, unsigned int index);
extern "C" __attribute__((sysv_abi)) void GnmW01_8694e0(void* cb, unsigned int reg_abs,
                                                         const unsigned int* values,
                                                         unsigned int count);
extern "C" __attribute__((sysv_abi)) void GnmW01_869890(void* cb, unsigned int a, unsigned int b);
extern "C" __attribute__((sysv_abi)) void GnmW01_869c20(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW01_869c80(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW01_86a120(void* cb, unsigned int a, unsigned int b);
extern "C" __attribute__((sysv_abi)) void GnmW01_86a3d0(void* cb, unsigned long long value);
extern "C" __attribute__((sysv_abi)) void GnmW01_86a970(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW01_86bb30(void* cb, unsigned int arg);
#else
{ 0x869990, (void*)&GnmW01_869990, "0x869990 W01 CB_TARGET_MASK", "native-gnm:cb" },
{ 0x869a50, (void*)&GnmW01_869a50, "0x869a50 W01 CB_BLENDn_CONTROL", "native-gnm:cb" },
{ 0x869ce0, (void*)&GnmW01_869ce0, "0x869ce0 W01 CB_COLOR_CONTROL", "native-gnm:cb" },
{ 0x869ac0, (void*)&GnmW01_869ac0, "0x869ac0 W01 CB_BLEND_RGBA", "native-gnm:cb" },
{ 0x869370, (void*)&GnmW01_869370, "0x869370 W01 SET_CONTEXT_REG generic (index)", "native-gnm:cb" },
{ 0x8694e0, (void*)&GnmW01_8694e0, "0x8694e0 W01 SET_CONTEXT_REG range", "native-gnm:cb" },
{ 0x869890, (void*)&GnmW01_869890, "0x869890 W01 SET_CONTEXT_REG 0x1ba", "native-gnm:cb" },
{ 0x869c20, (void*)&GnmW01_869c20, "0x869c20 W01 SET_CONTEXT_REG 0x2b0", "native-gnm:cb" },
{ 0x869c80, (void*)&GnmW01_869c80, "0x869c80 W01 SET_CONTEXT_REG 0x2b1", "native-gnm:cb" },
{ 0x86a120, (void*)&GnmW01_86a120, "0x86a120 W01 PA_SC_WINDOW_SCISSOR_TL", "native-gnm:cb" },
{ 0x86a3d0, (void*)&GnmW01_86a3d0, "0x86a3d0 W01 PA_SC_AA_MASK", "native-gnm:cb" },
{ 0x86a970, (void*)&GnmW01_86a970, "0x86a970 W01 SET_CONTEXT_REG 0x102", "native-gnm:cb" },
{ 0x86bb30, (void*)&GnmW01_86bb30, "0x86bb30 W01 SET_CONTEXT_REG 0x2ad", "native-gnm:cb" },
#endif
