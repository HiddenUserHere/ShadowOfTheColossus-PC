// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GNM boundary detours of worker W12 (Submit / flip); callbacks live in
// gpu/native/gnm/w12_submit.cpp. Included twice by gpu/sotc/hook_table.inc: with SOTC_GNM_DECLS
// defined (the extern "C" declarations, guest SysV signatures) and inside kRenderJobHookTable (the
// rows, stage "native-gnm"). Example:
//   decl: extern "C" void GnmW12_869990(void* cb, unsigned value);
//   row:  { 0x869990, (void*)&GnmW12_869990, "0x869990 W12 name", "native-gnm" },
#ifdef SOTC_GNM_DECLS
#else
#endif
