// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W07 (EOP / EOS / WAIT_REG_MEM / ACQUIRE_MEM / EVENT_WRITE / labels); callbacks live in
// gpu/native/gnm/w07_sync.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW07_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW07_869990, "0x869990 W07 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
extern "C" unsigned long long GnmW07_86d210(unsigned long long cb, unsigned long long size_bytes,
                                            int log2_align);
extern "C" void GnmW07_86eb60(unsigned long long cb, unsigned int event_type, unsigned int flags,
                              unsigned long long addr, int data_sel, unsigned long long data,
                              unsigned int f7, unsigned int int_sel);
extern "C" void GnmW07_86edc0(unsigned long long cb, unsigned long long addr, unsigned int mask,
                              unsigned int func, unsigned int ref);
extern "C" void GnmW07_86eec0(unsigned long long cb, unsigned long long addr, unsigned int mask,
                              unsigned int ref);
extern "C" void GnmW07_86ced0(unsigned long long cb, unsigned int event_type);
extern "C" void GnmW07_86f1d0(unsigned long long cb, unsigned int p2, unsigned int p3, int p4);
extern "C" void GnmW07_86cf40(unsigned long long cb, unsigned int p2, unsigned int p3,
                              unsigned int p4, unsigned int p5, unsigned int p6, int p7);
extern "C" void GnmW07_86ee70(unsigned long long cb);
extern "C" void GnmW07_86f5e0(unsigned long long cb, unsigned int event_type,
                              unsigned long long addr, int value);
extern "C" unsigned long long GnmW07_883f80(unsigned long long cb, unsigned long long size_bytes,
                                            int log2_align);
extern "C" void GnmW07_86f4e0(unsigned long long cb);
extern "C" void GnmW07_86f3f0(unsigned long long cb, unsigned long long p2,
                              unsigned long long p3);
extern "C" void GnmW07_86cff0(unsigned long long cb, unsigned int addr, unsigned int mask,
                              unsigned int func, unsigned int ref);
extern "C" void GnmW07_86e9e0(unsigned long long cb, unsigned int func, unsigned int engine,
                              unsigned long long addr, unsigned int ref, unsigned int mask,
                              unsigned short poll);
extern "C" void GnmW07_884b00(unsigned long long cb, unsigned int func, unsigned int engine,
                              unsigned long long addr, unsigned int ref, unsigned int mask,
                              unsigned short poll);
extern "C" void GnmW07_884a60(unsigned long long cb, unsigned long long p2);
#else
    { 0x86d210, (void*)&GnmW07_86d210, "0x86d210 W07 label slot", "native-gnm:sync" },
    { 0x86eb60, (void*)&GnmW07_86eb60, "0x86eb60 W07 EVENT_WRITE_EOP", "native-gnm:sync" },
    { 0x86edc0, (void*)&GnmW07_86edc0, "0x86edc0 W07 WAIT_REG_MEM", "native-gnm:sync" },
    { 0x86eec0, (void*)&GnmW07_86eec0, "0x86eec0 W07 WAIT_REG_MEM PFP", "native-gnm:sync" },
    { 0x86ced0, (void*)&GnmW07_86ced0, "0x86ced0 W07 EVENT_WRITE", "native-gnm:sync" },
    { 0x86f1d0, (void*)&GnmW07_86f1d0, "0x86f1d0 W07 ACQUIRE_MEM+EVENT", "native-gnm:sync" },
    { 0x86cf40, (void*)&GnmW07_86cf40, "0x86cf40 W07 ACQUIRE_MEM", "native-gnm:sync" },
    { 0x86ee70, (void*)&GnmW07_86ee70, "0x86ee70 W07 PFP_SYNC_ME", "native-gnm:sync" },
    { 0x86f5e0, (void*)&GnmW07_86f5e0, "0x86f5e0 W07 EVENT_WRITE_EOS", "native-gnm:sync" },
    { 0x883f80, (void*)&GnmW07_883f80, "0x883f80 W07 label slot (queue 2)", "native-gnm:sync" },
    { 0x86f4e0, (void*)&GnmW07_86f4e0, "0x86f4e0 W07 INCREMENT_DE_COUNTER", "native-gnm:sync" },
    { 0x86f3f0, (void*)&GnmW07_86f3f0, "0x86f3f0 W07 NOP marker 0x68750012", "native-gnm:sync" },
    { 0x86cff0, (void*)&GnmW07_86cff0, "0x86cff0 W07 WAIT_REG_MEM register", "native-gnm:sync" },
    { 0x86e9e0, (void*)&GnmW07_86e9e0, "0x86e9e0 W07 WAIT_REG_MEM generic", "native-gnm:sync" },
    { 0x884b00, (void*)&GnmW07_884b00, "0x884b00 W07 WAIT_REG_MEM generic", "native-gnm:sync" },
    { 0x884a60, (void*)&GnmW07_884a60, "0x884a60 W07 WAIT_REG_MEM label", "native-gnm:sync" },
#endif
