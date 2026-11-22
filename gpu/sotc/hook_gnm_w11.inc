// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W11 (Constant engine, misc opcodes, markers); callbacks live in
// gpu/native/gnm/w11_ce_misc.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW11_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW11_869990, "0x869990 W11 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
extern "C" void GnmW11_86c000(void* cb, unsigned int value);
extern "C" void GnmW11_86a880(void* cb, unsigned long long addr, int size);
extern "C" void GnmW11_865f00(void* cb);
extern "C" void GnmW11_865f50(void* cb);
extern "C" void GnmW11_865ff0(void* cb, unsigned long long addr, unsigned int offset,
                              unsigned int num_dw);
extern "C" void GnmW11_866090(void* cb, unsigned int offset, unsigned long long src,
                              unsigned int count);
extern "C" unsigned long long GnmW11_866150(void* cb, unsigned int offset, unsigned int count);
extern "C" void GnmW11_865fa0(void* cb, unsigned int diff);
extern "C" void GnmW11_86f490(void* cb);
extern "C" void GnmW11_883f30(void* cb);
#else
{ 0x86c000, (void*)&GnmW11_86c000, "0x86c000 W11 NUM_INSTANCES", "native-gnm:ce" },
{ 0x86a880, (void*)&GnmW11_86a880, "0x86a880 W11 GET_LOD_STATS", "native-gnm:ce" },
{ 0x865f00, (void*)&GnmW11_865f00, "0x865f00 W11 INCREMENT_CE_COUNTER", "native-gnm:ce" },
{ 0x865f50, (void*)&GnmW11_865f50, "0x865f50 W11 INCREMENT_CE_COUNTER", "native-gnm:ce" },
{ 0x865ff0, (void*)&GnmW11_865ff0, "0x865ff0 W11 DUMP_CONST_RAM", "native-gnm:ce" },
{ 0x866090, (void*)&GnmW11_866090, "0x866090 W11 WRITE_CONST_RAM", "native-gnm:ce" },
{ 0x866150, (void*)&GnmW11_866150, "0x866150 W11 WRITE_CONST_RAM (payload space)", "native-gnm:ce" },
{ 0x865fa0, (void*)&GnmW11_865fa0, "0x865fa0 W11 WAIT_ON_DE_COUNTER_DIFF", "native-gnm:ce" },
{ 0x86f490, (void*)&GnmW11_86f490, "0x86f490 W11 WAIT_ON_CE_COUNTER", "native-gnm:ce" },
{ 0x883f30, (void*)&GnmW11_883f30, "0x883f30 W11 WAIT_ON_CE_COUNTER", "native-gnm:ce" },
#endif
