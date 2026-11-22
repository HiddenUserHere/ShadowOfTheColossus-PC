// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. hook_table.inc: the data table of RenderJob entry hooks. Included by
// gpu/sotc/hooks.cpp. Each row detours one guest function entry to a host callback, enabled by the
// SOTC_RENDER stage named in `stage` (or by "native", which enables every row).
//
// This is DATA, not code: adding a hook is a one-line edit here. The guest VAs are the RenderJob
// functions of docs/engine/native-render-map.md. The callbacks are the Phase 2 implementations; a
// row whose callback is not yet written is left out until its stage lands, so the build stays green.

#ifndef SOTC_HOOK_TABLE_INC
#define SOTC_HOOK_TABLE_INC

// The Phase 2 callbacks live in gpu/native/native_hooks.cpp (namespace Native, extern "C").
extern "C" void RenderJob_Clear_93690(void* pass);
extern "C" void RenderJob_DrawAuto_94ec0(void* pass, int mode, unsigned start_vertex,
                                         unsigned index_count);
extern "C" void RenderJob_DrawOffset_94d00(void* pass, int mode, unsigned long long a3,
                                           unsigned long long a4, unsigned index_offset,
                                           unsigned index_count);
extern "C" void RenderJob_DrawOffsetInst_94dd0(void* pass, int mode, unsigned long long a3,
                                               unsigned long long a4, unsigned index_offset,
                                               unsigned index_count, unsigned instances);
extern "C" void RenderJob_DrawIndirect_10add0(void* pass, const unsigned* pair,
                                              unsigned data_offset);
extern "C" void RenderJob_DrawIndexIndirect_db620(void* pass, const unsigned* pair,
                                                  unsigned data_offset);
extern "C" void RenderJob_Targets_92140(void* pass, void* targets);
extern "C" void RenderJob_MaterialBind_85fc10(void* material);
extern "C" unsigned long long RenderJob_OccluderLoop_11d3d0(void* unused, unsigned long long list,
                                                            unsigned long long* ctx, int group);
// Phase 2.6: pass functions (gpu/native/native_pass_callbacks.cpp). Derivations run live and are
// traced; the emission gate (NativePassEmissionReady) is closed until 0x93690 resolves natively.
extern "C" void NativePass_93960(void* pass, unsigned long long desc);
extern "C" void NativePass_94600(void* pass, unsigned long long desc);
extern "C" void NativePass_93ef0(void* pass, unsigned long long bound,
                                 unsigned long long target, unsigned char flags);
// Phase 2.8: shadows (gpu/native/native_shadow_callbacks.cpp). Derivations run live; the commit
// runs only when the module reports no gate reason, otherwise the callback chains.
extern "C" void NativeShadow_cc9b0(void* renderer, void* shadow_cfg, void* unused, void* ctx);
extern "C" void NativeShadow_c59d0(void* renderer, void* view_cfg, void* ctx, void* draw_table);
extern "C" void NativeShadow_93ef0(void* pass, unsigned long long bound,
                                   unsigned long long target, unsigned char flags);
extern "C" void NativeShadow_93d70(void* pass, unsigned long long view_desc);
// Phase 2.10: table / block writers (gpu/native/native_tables_callbacks.cpp). The signatures are
// the guest's own SysV ones: 0x95320 returns the bump-allocated table address (bind doc §2.7), so
// it returns u64 here as well; 0x123810's 5th argument (inst) is the r9 pointer.
extern "C" unsigned long long NativeTables_95320(unsigned* src, unsigned mask, unsigned tag);
extern "C" void NativeTables_ae150(void* binder, unsigned long long table);
extern "C" void NativeTables_123cc0(void* rec2, void* ctx, int index, char flag);
extern "C" void NativeTables_123810(void* renderable, void* dest, int index, char flag,
                                    const unsigned* inst);
extern "C" void NativeTables_ca6d0(void* r, void* f, void* ctx);
// Phase 2.9: MainCamera (0xcaed0) orchestrator (gpu/native/native_camera_callbacks.cpp).
extern "C" void NativeCamera_caed0(void* renderer, void* view_cfg, void* ctx);
// Phase 2.11/2.12: the frame body and the per-frame reset/fence
// (gpu/native/native_frame_callbacks.cpp). Both derive the frame structure read-only and chain.
extern "C" void NativeFrame_cefa0(void* renderer);
extern "C" void NativeFrame_90ea0(void* owner, void* backend);

struct RenderJobHookEntry {
    u64 va;                 // guest VA (image offset) of the function entry
    void* callback;         // host C++ replacement
    const char* name;       // diagnostic
    const char* stage;      // SOTC_RENDER stage that enables it ("native" = all)
};

// clang-format off
static const RenderJobHookEntry kRenderJobHookTable[] = {
    // Phase 2.1: clear / fullscreen rect draw object (docs/engine/native-render-map.md §5).
    { 0x93690, (void*)&RenderJob_Clear_93690, "0x93690 Clear_93690", "native-clear" },
    // Phase 2.2: indexed/auto draw objects (docs/engine/native-render-map-draws.md).
    { 0x94ec0, (void*)&RenderJob_DrawAuto_94ec0, "0x94ec0 DrawAuto", "native-draw" },
    { 0x94d00, (void*)&RenderJob_DrawOffset_94d00, "0x94d00 DrawIndexOffset", "native-draw" },
    { 0x94dd0, (void*)&RenderJob_DrawOffsetInst_94dd0, "0x94dd0 DrawIndexOffset+Instances", "native-draw" },
    // Phase 2.3: indirect draw objects.
    { 0x10add0, (void*)&RenderJob_DrawIndirect_10add0, "0x10add0 DrawIndirect", "native-indirect" },
    { 0xdb620, (void*)&RenderJob_DrawIndexIndirect_db620, "0xdb620 DrawIndexIndirect", "native-indirect" },
    // Phase 2.5: render-target set binder (docs/engine/native-render-map-bind.md §3). The
    // SetRenderTarget helper 0x85cbc0 is internal to this flow (callers: 0x92140 and the pass
    // functions 0x93960/0x94600 only), so the 0x92140 hook covers it.
    { 0x92140, (void*)&RenderJob_Targets_92140, "0x92140 TargetSet", "native-rt" },
    // Phase 2.4: the material/shader bind (docs/engine/native-render-map-bind.md §2). The draw
    // objects (0x94ec0 family above) are its call sites; this row additionally traces the bind
    // function itself. Both chain to the guest body until the pipeline fill side lands (gap
    // N2-10); the derivation runs and is traceable via SOTC_RENDER_VERBOSE.
    { 0x85fc10, (void*)&RenderJob_MaterialBind_85fc10, "0x85fc10 MaterialBind_85fc10", "native-bind" },
    // Phase 2.7: the per-sort-group draw loop (docs/engine/native-render-map-cull-camera-shadow.md
    // §1/§2). The body stages each record through the engine helpers and issues the draw; the
    // native replacement derives the group read-only, gates on the pipeline map (N2-10) and
    // issues the draws natively.
    { 0x11d3d0, (void*)&RenderJob_OccluderLoop_11d3d0, "0x11d3d0 OccluderLoop", "native-cull" },
    // Phase 2.6: the three pass functions (docs/engine/native-render-map-pass.md §2). Each derives
    // the phase structure (descriptor gates, dims, colour slots, depth block, label/EOP fence) and
    // chains to the guest body while the emission gate is closed.
    { 0x93960, (void*)&NativePass_93960, "0x93960 PassA", "native-pass" },
    { 0x94600, (void*)&NativePass_94600, "0x94600 PassB", "native-pass" },
    { 0x93ef0, (void*)&NativePass_93ef0, "0x93ef0 PassDepth", "native-pass" },
    // Phase 2.8: RenderShadows (5-cascade orchestrator), the G-buffer runner and the depth pass
    // object plus its 0x93d70 touch wrapper (native-render-map-cull-camera-shadow.md §2/§3).
    // 0x93ef0 has a single row (the pass stage above); NativeShadow_93ef0 is declared but not
    // listed. If it is ever added here, table order decides: the first matching row wins.
    { 0xcc9b0, (void*)&NativeShadow_cc9b0, "0xcc9b0 RenderShadows", "native-shadow" },
    { 0xc59d0, (void*)&NativeShadow_c59d0, "0xc59d0 GBufferRunner", "native-shadow" },
    { 0x93d70, (void*)&NativeShadow_93d70, "0x93d70 DepthTouch", "native-shadow" },
    // Phase 2.10: the per-draw table writers (0x95320 root table, 0xae150 user-data binder) and
    // the instance/object block writer (0x123cc0 thunk + the 0x123810 body it forwards to), plus
    // the per-view draw-table double-buffer copy 0xca6d0 before MainCamera. Each derives read-only
    // and chains: the helpers mutate guest state the emulated draws read back.
    { 0x95320, (void*)&NativeTables_95320, "0x95320 RootTable", "native-tables" },
    { 0xae150, (void*)&NativeTables_ae150, "0xae150 UserDataTable", "native-tables" },
    { 0x123cc0, (void*)&NativeTables_123cc0, "0x123cc0 InstanceBindThunk", "native-tables" },
    { 0x123810, (void*)&NativeTables_123810, "0x123810 InstanceBind", "native-tables" },
    { 0xca6d0, (void*)&NativeTables_ca6d0, "0xca6d0 DrawTables", "native-tables" },
    // Phase 2.9: MainCamera (0xcaed0), the pass-graph orchestrator
    // (native-render-map-cull-camera-shadow.md §4). The derivation is trace-only until the bodies
    // it orchestrates land (Phases 2.11/2.12); the callback always chains to the guest body.
    { 0xcaed0, (void*)&NativeCamera_caed0, "0xcaed0 MainCamera", "native-camera" },
    // Phase 2.11/2.12: the frame body (0xcefa0, RenderThread::EndFrameViewParams) and the
    // per-frame reset/fence (0x90ea0, RenderBackend_Frame). Both derive the frame structure
    // read-only (gpu/native/native_frame.cpp), trace it under SOTC_RENDER_VERBOSE=1 and chain to
    // the guest body while any frame step is emulated.
    { 0xcefa0, (void*)&NativeFrame_cefa0, "0xcefa0 FrameBody", "native-frame" },
    { 0x90ea0, (void*)&NativeFrame_90ea0, "0x90ea0 FrameFence", "native-frame" },
};
// clang-format on

#endif // SOTC_HOOK_TABLE_INC
