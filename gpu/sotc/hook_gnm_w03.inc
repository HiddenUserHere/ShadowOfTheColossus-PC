// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W03 (Context registers: raster / viewport / other); callbacks live in
// gpu/native/gnm/w03_ctx_raster.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW03_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW03_869990, "0x869990 W03 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
extern "C" void GnmW03_86d7b0(float zmin, float zmax, unsigned long long cb, int index,
                              const unsigned int* scale, const unsigned int* offset);
extern "C" void GnmW03_86d950(unsigned long long cb, unsigned int p2, int p3, unsigned int p4,
                              int p5);
extern "C" void GnmW03_86e1a0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86e410(float p1, float p2, unsigned long long cb);
extern "C" void GnmW03_86e480(float p1, float p2, unsigned long long cb);
extern "C" void GnmW03_86d160(unsigned long long cb, int value);
extern "C" void GnmW03_86cc20(unsigned long long cb, const unsigned int* p2, unsigned int p3);
extern "C" void GnmW03_86e2d0(unsigned long long cb, unsigned int p2, int p3);
extern "C" void GnmW03_86db40(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86ded0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86cbc0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86b6f0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86e4f0(unsigned long long cb, unsigned int p2, unsigned int p3);
extern "C" void GnmW03_86c650(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86c5f0(unsigned long long cb);
extern "C" void GnmW03_86e560(unsigned long long cb, unsigned int p2, unsigned int p3);
extern "C" void GnmW03_86a090(float p1, float p2, float p3, float p4,
                              unsigned long long cb);
extern "C" void GnmW03_86a010(unsigned long long cb, unsigned int p2, unsigned int p3);
extern "C" void GnmW03_86d890(unsigned long long cb, unsigned long long index, unsigned int p3,
                              unsigned int p4, unsigned int p5, unsigned int p6, int p7);
extern "C" void GnmW03_86e5e0(unsigned long long cb, const unsigned long long* p2);
extern "C" void GnmW03_86c6c0(unsigned long long cb, unsigned int p2, unsigned int p3,
                              unsigned int p4);
extern "C" void GnmW03_86c840(unsigned long long cb, unsigned long long p2);
extern "C" void GnmW03_86bc00(unsigned long long cb, const unsigned int* p2, int count);
extern "C" void GnmW03_86e3a0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86dba0(unsigned long long cb, unsigned int p2, unsigned int p3,
                              unsigned int p4);
extern "C" void GnmW03_86dc10(unsigned long long cb);
extern "C" void GnmW03_86dc70(unsigned long long cb, unsigned int p2, unsigned int p3,
                              unsigned int p4, unsigned int p5, unsigned int p6,
                              unsigned int p7);
extern "C" void GnmW03_86dd80(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86ddf0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86df30(unsigned long long cb, unsigned int index, float v0, float v1,
                              float v2, float v3);
extern "C" void GnmW03_86dfd0(unsigned long long cb, unsigned long long index, unsigned int p3,
                              unsigned int p4, unsigned int p5, unsigned int p6);
extern "C" void GnmW03_86e080(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86e0e0(unsigned long long cb, unsigned int p2);
extern "C" void GnmW03_86e140(unsigned long long cb, unsigned int p2);
#else
    { 0x86d7b0, (void*)&GnmW03_86d7b0, "0x86d7b0 W03 viewport n", "native-gnm:raster" },
    { 0x86d950, (void*)&GnmW03_86d950, "0x86d950 W03 scissor", "native-gnm:raster" },
    { 0x86e1a0, (void*)&GnmW03_86e1a0, "0x86e1a0 W03 PA_SU_SC_MODE_CNTL", "native-gnm:raster" },
    { 0x86e410, (void*)&GnmW03_86e410, "0x86e410 W03 poly offset front", "native-gnm:raster" },
    { 0x86e480, (void*)&GnmW03_86e480, "0x86e480 W03 poly offset back", "native-gnm:raster" },
    { 0x86d160, (void*)&GnmW03_86d160, "0x86d160 W03 reg 0x2d5", "native-gnm:raster" },
    { 0x86cc20, (void*)&GnmW03_86cc20, "0x86cc20 W03 multi 0x2d7/0x298/0x2ce/0x2ac", "native-gnm:raster" },
    { 0x86e2d0, (void*)&GnmW03_86e2d0, "0x86e2d0 W03 reg 0x281", "native-gnm:raster" },
    { 0x86db40, (void*)&GnmW03_86db40, "0x86db40 W03 reg 0x206", "native-gnm:raster" },
    { 0x86ded0, (void*)&GnmW03_86ded0, "0x86ded0 W03 reg 0x204", "native-gnm:raster" },
    { 0x86cbc0, (void*)&GnmW03_86cbc0, "0x86cbc0 W03 reg 0x2ab", "native-gnm:raster" },
    { 0x86b6f0, (void*)&GnmW03_86b6f0, "0x86b6f0 W03 reg 0x2dd", "native-gnm:raster" },
    { 0x86e4f0, (void*)&GnmW03_86e4f0, "0x86e4f0 W03 reg 0x292", "native-gnm:raster" },
    { 0x86c650, (void*)&GnmW03_86c650, "0x86c650 W03 reg 0x290", "native-gnm:raster" },
    { 0x86c5f0, (void*)&GnmW03_86c5f0, "0x86c5f0 W03 regs 0x290/0x2e5", "native-gnm:raster" },
    { 0x86e560, (void*)&GnmW03_86e560, "0x86e560 W03 reg 0x2f8", "native-gnm:raster" },
    { 0x86a090, (void*)&GnmW03_86a090, "0x86a090 W03 reg 0x2fa x4", "native-gnm:raster" },
    { 0x86a010, (void*)&GnmW03_86a010, "0x86a010 W03 reg 0x8d", "native-gnm:raster" },
    { 0x86d890, (void*)&GnmW03_86d890, "0x86d890 W03 reg pair 0x94+2n", "native-gnm:raster" },
    { 0x86e5e0, (void*)&GnmW03_86e5e0, "0x86e5e0 W03 block 0x2fe x16 + 0x2f5", "native-gnm:raster" },
    { 0x86c6c0, (void*)&GnmW03_86c6c0, "0x86c6c0 W03 regs 0x291/0x290", "native-gnm:raster" },
    { 0x86c840, (void*)&GnmW03_86c840, "0x86c840 W03 reg 0x20", "native-gnm:raster" },
    { 0x86bc00, (void*)&GnmW03_86bc00, "0x86bc00 W03 multi 0x191 count", "native-gnm:raster" },
    { 0x86e3a0, (void*)&GnmW03_86e3a0, "0x86e3a0 W03 reg 0x2de table", "native-gnm:raster" },
    { 0x86dba0, (void*)&GnmW03_86dba0, "0x86dba0 W03 regs 0xe8..0xea", "native-gnm:raster" },
    { 0x86dc10, (void*)&GnmW03_86dc10, "0x86dc10 W03 regs 0xeb/0xec constant", "native-gnm:raster" },
    { 0x86dc70, (void*)&GnmW03_86dc70, "0x86dc70 W03 regs 0xeb/0xec byte fields", "native-gnm:raster" },
    { 0x86dd80, (void*)&GnmW03_86dd80, "0x86dd80 W03 reg 0x293", "native-gnm:raster" },
    { 0x86ddf0, (void*)&GnmW03_86ddf0, "0x86ddf0 W03 reg 0x6", "native-gnm:raster" },
    { 0x86df30, (void*)&GnmW03_86df30, "0x86df30 W03 regs 0x16f+4n float x4", "native-gnm:raster" },
    { 0x86dfd0, (void*)&GnmW03_86dfd0, "0x86dfd0 W03 regs 0x84+2n pairs", "native-gnm:raster" },
    { 0x86e080, (void*)&GnmW03_86e080, "0x86e080 W03 reg 0x83", "native-gnm:raster" },
    { 0x86e0e0, (void*)&GnmW03_86e0e0, "0x86e0e0 W03 reg 0x2a5", "native-gnm:raster" },
    { 0x86e140, (void*)&GnmW03_86e140, "0x86e140 W03 reg 0x103", "native-gnm:raster" },
#endif
