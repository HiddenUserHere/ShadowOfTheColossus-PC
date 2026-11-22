// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W05 (UConfig / index / predication / config / default hardware state); callbacks live in
// gpu/native/gnm/w05_uconfig.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm").
#ifdef SOTC_GNM_DECLS
extern "C" __attribute__((sysv_abi)) void GnmW05_86a900(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW05_86b5c0(void* cb, unsigned int p2,
                                                        unsigned int p3, int p4, unsigned int p5,
                                                        unsigned int p6);
extern "C" __attribute__((sysv_abi)) void GnmW05_86bd60(void* cb, unsigned char p2,
                                                        unsigned int p3, unsigned int p4,
                                                        unsigned char p5, int p6, int p7);
extern "C" __attribute__((sysv_abi)) void GnmW05_86fc30(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW05_86bed0(void* cb, unsigned int p2,
                                                        unsigned int p3);
extern "C" __attribute__((sysv_abi)) void GnmW05_86bf50(void* cb, unsigned long long p2);
extern "C" __attribute__((sysv_abi)) void GnmW05_86bfb0(void* cb, unsigned int value);
extern "C" __attribute__((sysv_abi)) void GnmW05_86e770(void* cb, unsigned long long p2,
                                                        unsigned int p3, unsigned int p4);
extern "C" __attribute__((sysv_abi)) void GnmW05_86e800(void* cb);
extern "C" __attribute__((sysv_abi)) void GnmW05_869430(void* cb, int p2, unsigned int p3);
extern "C" __attribute__((sysv_abi)) void GnmW05_883140(void* cb, int p2, unsigned int p3);
extern "C" __attribute__((sysv_abi)) void GnmW05_869530(void* cb, int p2, unsigned long long p3,
                                                        unsigned int p4);
extern "C" __attribute__((sysv_abi)) void GnmW05_883210(void* cb, int p2, unsigned long long p3,
                                                        unsigned int p4);
extern "C" __attribute__((sysv_abi)) void GnmW05_8693d0(void* cb, int p2, unsigned int p3, int p4);
extern "C" __attribute__((sysv_abi)) void GnmW05_869480(void* cb, int p2, unsigned int p3);
extern "C" __attribute__((sysv_abi)) void GnmW05_8839c0(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4, unsigned int p5);
extern "C" __attribute__((sysv_abi)) void GnmW05_883ac0(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4);
extern "C" __attribute__((sysv_abi)) void GnmW05_883bb0(void* cb, unsigned int p2, unsigned int p3,
                                                        unsigned int p4, unsigned int p5,
                                                        unsigned int p6);
extern "C" __attribute__((sysv_abi)) void GnmW05_86ad40(void* cb, unsigned int shader_type,
                                                        unsigned long long addr);
#else
{ 0x86a900, (void*)&GnmW05_86a900, "0x86a900 W05 SET_UCONFIG VGT_PRIMITIVE_TYPE", "native-gnm:uconfig" },
{ 0x86b5c0, (void*)&GnmW05_86b5c0, "0x86b5c0 W05 SET_UCONFIG 0x41d x3", "native-gnm:uconfig" },
{ 0x86bd60, (void*)&GnmW05_86bd60, "0x86bd60 W05 SET_UCONFIG 0x382 packed", "native-gnm:uconfig" },
{ 0x86fc30, (void*)&GnmW05_86fc30, "0x86fc30 W05 SET_UCONFIG 0x248", "native-gnm:uconfig" },
{ 0x86bed0, (void*)&GnmW05_86bed0, "0x86bed0 W05 INDEX_TYPE", "native-gnm:uconfig" },
{ 0x86bf50, (void*)&GnmW05_86bf50, "0x86bf50 W05 INDEX_BASE", "native-gnm:uconfig" },
{ 0x86bfb0, (void*)&GnmW05_86bfb0, "0x86bfb0 W05 INDEX_BUFFER_SIZE", "native-gnm:uconfig" },
{ 0x86e770, (void*)&GnmW05_86e770, "0x86e770 W05 SET_PREDICATION", "native-gnm:uconfig" },
{ 0x86e800, (void*)&GnmW05_86e800, "0x86e800 W05 SET_PREDICATION clear", "native-gnm:uconfig" },
{ 0x869430, (void*)&GnmW05_869430, "0x869430 W05 SET_CONFIG one", "native-gnm:uconfig" },
{ 0x883140, (void*)&GnmW05_883140, "0x883140 W05 SET_CONFIG one", "native-gnm:uconfig" },
{ 0x869530, (void*)&GnmW05_869530, "0x869530 W05 SET_CONFIG array", "native-gnm:uconfig" },
{ 0x883210, (void*)&GnmW05_883210, "0x883210 W05 SET_CONFIG array", "native-gnm:uconfig" },
{ 0x8693d0, (void*)&GnmW05_8693d0, "0x8693d0 W05 SET_UCONFIG indexed", "native-gnm:uconfig" },
{ 0x869480, (void*)&GnmW05_869480, "0x869480 W05 SET_UCONFIG one", "native-gnm:uconfig" },
{ 0x8839c0, (void*)&GnmW05_8839c0, "0x8839c0 W05 SET_UCONFIG 0x41d x3 (from 0x85c580)", "native-gnm:uconfig" },
{ 0x883ac0, (void*)&GnmW05_883ac0, "0x883ac0 W05 0xc0008c00 + SET_UCONFIG 0x342 / NOP", "native-gnm:uconfig" },
{ 0x883bb0, (void*)&GnmW05_883bb0, "0x883bb0 W05 NOP + SET_UCONFIG 0x342 + 0xc0048d02 / NOP", "native-gnm:uconfig" },
{ 0x86ad40, (void*)&GnmW05_86ad40, "0x86ad40 W05 SET_BASE indirect args", "native-gnm:uconfig" },
#endif
