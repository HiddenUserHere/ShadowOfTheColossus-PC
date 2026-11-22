// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W06 (WRITE_DATA / DMA_DATA); callbacks live in
// gpu/native/gnm/w06_memory.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm").
#ifdef SOTC_GNM_DECLS
extern "C" void GnmW06_86a440(unsigned long long cb, unsigned long long dst, const unsigned* data,
                              int count, unsigned wr_confirm);
extern "C" void GnmW06_8663e0(unsigned long long cb, unsigned long long dst, const unsigned* data,
                              int count, unsigned wr_confirm);
extern "C" void GnmW06_883380(unsigned long long cb, unsigned long long dst, const unsigned* data,
                              int count, unsigned wr_confirm);
extern "C" void GnmW06_8834d0(unsigned long long cb, unsigned long long dst, const unsigned* data,
                              int count, unsigned engine, unsigned wr_confirm);
extern "C" void GnmW06_86a590(unsigned long long cb, unsigned long long dst, const unsigned* data,
                              int count, unsigned engine, unsigned wr_confirm);
extern "C" void GnmW06_884b70(unsigned long long cb, unsigned long long slot_end);
extern "C" void GnmW06_86a700(unsigned long long cb, unsigned p2, unsigned long long dst,
                              unsigned p4, unsigned long long src, unsigned p6, int p7);
extern "C" void GnmW06_883640(unsigned long long cb, unsigned p2, unsigned long long dst,
                              unsigned p4, unsigned long long src, unsigned p6, int p7);
extern "C" void GnmW06_86a7f0(unsigned long long cb, unsigned long long src, unsigned num_bytes);
// RELEASE_MEM (compute rings): 8 integer args, the last two on the stack (SysV).
extern "C" void GnmW06_8846f0(unsigned long long cb, unsigned p1, unsigned p2,
                              unsigned long long p3, unsigned p4, unsigned long long p5,
                              unsigned s1, unsigned s2);
extern "C" void GnmW06_884580(unsigned long long cb, unsigned p1, unsigned p2);
extern "C" void GnmW06_884610(unsigned long long cb, unsigned p1, unsigned p2,
                              unsigned long long p3, unsigned p4, unsigned long long p5,
                              unsigned s1, unsigned s2);
extern "C" void GnmW06_8848e0(unsigned long long cb, unsigned p1, unsigned long long p2,
                              unsigned p3, unsigned p4);
extern "C" void GnmW06_8849a0(unsigned long long cb, unsigned p1, unsigned long long p2,
                              unsigned p3, unsigned p4);
#else
    { 0x86a440, (void*)&GnmW06_86a440, "0x86a440 W06 WRITE_DATA", "native-gnm:memory" },
    { 0x8663e0, (void*)&GnmW06_8663e0, "0x8663e0 W06 WRITE_DATA", "native-gnm:memory" },
    { 0x883380, (void*)&GnmW06_883380, "0x883380 W06 WRITE_DATA", "native-gnm:memory" },
    { 0x8834d0, (void*)&GnmW06_8834d0, "0x8834d0 W06 WRITE_DATA", "native-gnm:memory" },
    { 0x86a590, (void*)&GnmW06_86a590, "0x86a590 W06 WRITE_DATA", "native-gnm:memory" },
    { 0x884b70, (void*)&GnmW06_884b70, "0x884b70 W06 WRITE_DATA fence", "native-gnm:memory" },
    { 0x86a700, (void*)&GnmW06_86a700, "0x86a700 W06 DMA_DATA", "native-gnm:memory" },
    { 0x883640, (void*)&GnmW06_883640, "0x883640 W06 DMA_DATA", "native-gnm:memory" },
    { 0x86a7f0, (void*)&GnmW06_86a7f0, "0x86a7f0 W06 DMA_DATA dropped", "native-gnm:memory" },
    { 0x8846f0, (void*)&GnmW06_8846f0, "0x8846f0 W06 RELEASE_MEM int 3", "native-gnm:memory" },
    { 0x884580, (void*)&GnmW06_884580, "0x884580 W06 RELEASE_MEM no address", "native-gnm:memory" },
    { 0x884610, (void*)&GnmW06_884610, "0x884610 W06 RELEASE_MEM int 2", "native-gnm:memory" },
    { 0x8848e0, (void*)&GnmW06_8848e0, "0x8848e0 W06 RELEASE_MEM label-4 int 3", "native-gnm:memory" },
    { 0x8849a0, (void*)&GnmW06_8849a0, "0x8849a0 W06 RELEASE_MEM label-4 int 2", "native-gnm:memory" },
#endif
