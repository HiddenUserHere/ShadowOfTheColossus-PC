// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of W13 (INDIRECT_BUFFER); callbacks in gpu/native/gnm/w13_ib.cpp.
// Included twice by gpu/sotc/hook_table.inc (declarations with SOTC_GNM_DECLS, rows otherwise).
#ifdef SOTC_GNM_DECLS
extern "C" __attribute__((sysv_abi)) void GnmW13_884c40(void* cb, unsigned long long address, unsigned long long size);
extern "C" __attribute__((sysv_abi)) void GnmW13_86fa00(void* cb, unsigned long long address, unsigned long long size);
#else
{ 0x884c40, (void*)&GnmW13_884c40, "0x884c40 W13 compute INDIRECT_BUFFER", "native-gnm:ib" },
{ 0x86fa00, (void*)&GnmW13_86fa00, "0x86fa00 W13 graphics INDIRECT_BUFFER", "native-gnm:ib" },
#endif
