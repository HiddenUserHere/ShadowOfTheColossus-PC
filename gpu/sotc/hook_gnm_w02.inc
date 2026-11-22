// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W02 (Context registers: depth / stencil); callbacks live in
// gpu/native/gnm/w02_ctx_db.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW02_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW02_869990, "0x869990 W02 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
extern "C" __attribute__((sysv_abi)) void GnmW02_869d60(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_869e80(void* cb);
extern "C" __attribute__((sysv_abi)) void GnmW02_869dc0(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_869bb0(void* cb, unsigned int front,
                                                         unsigned int back);
extern "C" __attribute__((sysv_abi)) void GnmW02_869b50(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_869f50(float value, void* cb);
extern "C" __attribute__((sysv_abi)) void GnmW02_869fb0(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_869ee0(float min, float max, void* cb);
extern "C" __attribute__((sysv_abi)) void GnmW02_86dd20(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_86be10(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_86be70(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_86de50(void* cb, unsigned int bit,
                                                         unsigned int mode);
extern "C" __attribute__((sysv_abi)) void GnmW02_869e20(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_8699f0(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW02_86bb90(void* cb, unsigned int a2, unsigned int a3);
extern "C" __attribute__((sysv_abi)) void GnmW02_86c4d0(void* cb, unsigned int a2, unsigned int a3);
extern "C" __attribute__((sysv_abi)) void GnmW02_86c570(void* cb, unsigned int a2, unsigned int a3);
extern "C" __attribute__((sysv_abi)) void GnmW02_86c8a0(void* cb, unsigned int a2);
extern "C" __attribute__((sysv_abi)) void GnmW02_86d080(void* cb, unsigned int a2, unsigned int a3,
                                                         unsigned int a4);
extern "C" __attribute__((sysv_abi)) void GnmW02_86d100(void* cb, const unsigned int* src);
extern "C" __attribute__((sysv_abi)) void GnmW02_86d730(void* cb, unsigned int a2, unsigned int a3,
                                                         unsigned int a4);
extern "C" __attribute__((sysv_abi)) void GnmW02_86d9e0(void* cb, unsigned int a2, unsigned int a3,
                                                         unsigned int a4, unsigned int a5,
                                                         unsigned int a6);
extern "C" __attribute__((sysv_abi)) void GnmW02_86da90(void* cb, unsigned int a2, unsigned int a3,
                                                         unsigned int a4, unsigned int a5,
                                                         unsigned int a6);
#else
    { 0x869d60, (void*)&GnmW02_869d60, "0x869d60 W02 DB_DEPTH_CONTROL", "native-gnm:db" },
    { 0x869e80, (void*)&GnmW02_869e80, "0x869e80 W02 DB_DEPTH_CONTROL=0", "native-gnm:db" },
    { 0x869dc0, (void*)&GnmW02_869dc0, "0x869dc0 W02 DB_STENCIL_CONTROL", "native-gnm:db" },
    { 0x869bb0, (void*)&GnmW02_869bb0, "0x869bb0 W02 STENCILREFMASK front/back", "native-gnm:db" },
    { 0x869b50, (void*)&GnmW02_869b50, "0x869b50 W02 STENCILREFMASK both", "native-gnm:db" },
    { 0x869f50, (void*)&GnmW02_869f50, "0x869f50 W02 DB_DEPTH_CLEAR", "native-gnm:db" },
    { 0x869fb0, (void*)&GnmW02_869fb0, "0x869fb0 W02 DB_STENCIL_CLEAR", "native-gnm:db" },
    { 0x869ee0, (void*)&GnmW02_869ee0, "0x869ee0 W02 DB_DEPTH_BOUNDS", "native-gnm:db" },
    { 0x86dd20, (void*)&GnmW02_86dd20, "0x86dd20 W02 DB_RENDER_CONTROL", "native-gnm:db" },
    { 0x86be10, (void*)&GnmW02_86be10, "0x86be10 W02 DB_RENDER_OVERRIDE", "native-gnm:db" },
    { 0x86be70, (void*)&GnmW02_86be70, "0x86be70 W02 DB_RENDER_OVERRIDE2", "native-gnm:db" },
    { 0x86de50, (void*)&GnmW02_86de50, "0x86de50 W02 DB_COUNT_CONTROL", "native-gnm:db" },
    { 0x869e20, (void*)&GnmW02_869e20, "0x869e20 W02 DB_EQAA", "native-gnm:db" },
    { 0x8699f0, (void*)&GnmW02_8699f0, "0x8699f0 W02 DB_ALPHA_TO_MASK", "native-gnm:db" },
    { 0x86bb90, (void*)&GnmW02_86bb90, "0x86bb90 W02 VGT_INSTANCE_STEP_RATE_0", "native-gnm:db" },
    { 0x86c4d0, (void*)&GnmW02_86c4d0, "0x86c4d0 W02 VGT_GS_MODE (+STRMOUT_CONFIG)", "native-gnm:db" },
    { 0x86c570, (void*)&GnmW02_86c570, "0x86c570 W02 VGT_GS_ONCHIP_CONTROL", "native-gnm:db" },
    { 0x86c8a0, (void*)&GnmW02_86c8a0, "0x86c8a0 W02 ENABLE_PRIMITIVE_ID", "native-gnm:db" },
    { 0x86d080, (void*)&GnmW02_86d080, "0x86d080 W02 indexed context pair", "native-gnm:db" },
    { 0x86d100, (void*)&GnmW02_86d100, "0x86d100 W02 VGT_STRMOUT_BUFFER_CONFIG", "native-gnm:db" },
    { 0x86d730, (void*)&GnmW02_86d730, "0x86d730 W02 reg 0x2f9", "native-gnm:db" },
    { 0x86d9e0, (void*)&GnmW02_86d9e0, "0x86d9e0 W02 PA_SC window scissor pair", "native-gnm:db" },
    { 0x86da90, (void*)&GnmW02_86da90, "0x86da90 W02 PA_SC generic scissor pair", "native-gnm:db" },
#endif
