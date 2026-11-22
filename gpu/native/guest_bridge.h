// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend (branch native/render-vk13). guest_bridge: typed, read-only views of
// the game's render structs. The native backend bypasses the statically-linked GNM library
// (docs/engine/native-render-seam.md) and reads the RenderJob's own state instead. Every offset
// here is proven in docs/engine/native-render-map-frame.md and native-render-map-pass.md.
//
// Guest memory lives at identical host addresses (src/runtime_memory.c: the "PC memory model"),
// so a guest VA is a plain pointer. All accessors take VAddr and return references/pointers into
// the game's own memory; nothing here writes.
#pragma once

#include "common/types.h"

namespace Native::Guest {

// -- Proven object model (docs/engine/native-render-map.md §1) -------------------------------
//   G = DAT_02edf140         global render/sync struct
//   R = *G                   renderer; R+0x40 = F
//   F = *(R+0x40)            frame object (RenderJob data); F+0xd98 = M
//   M = *(F+0xd98)           render-target / target manager
//   ctx                      job context, passed to every pass/draw
//   cb = *ctx                device / GNM cursor object
//
// Addressing: two kinds of guest address appear here. Guest heap pointers (the values stored in
// the render structs) are already host addresses — the "PC memory model" (src/runtime_memory.c)
// maps guest memory at its VA. Image-anchored globals (the DAT_02ee**** statics the decompiler
// names) are eboot image offsets: the loader (src/probe.c) relocates their slots to image+va, so
// reading one means dereferencing GuestImageBase() + va. GuestImageBase() is 0 until probe.c maps
// the image; callers must treat 0 as "not mapped yet" and fall back.

constexpr VAddr kGlobalRenderStruct = 0x2edf140; // DAT_02edf140, image-anchored (offset)

/// The mapped eboot image base (probe.c's `image`), or 0 before it is mapped. Set through
/// bb_set_guest_image (page_manager.cpp) right after the relocations are applied.
u64 GuestImageBase();

/// Reads one image-anchored qword global (DAT_02e*****: `va` is an image offset). Returns 0 while
/// the image is not mapped (no crash) and logs the first miss, so an unset base is never silent.
u64 ImageQword(VAddr va);

/// Global struct G: the renderer pointer lives at +0 (image-anchored).
inline u64 RendererPtr() {
    return ImageQword(kGlobalRenderStruct);
}

// Renderer R field offsets (proven).
namespace RendererOff {
constexpr u64 FrameObject = 0x40;    // R+0x40 = F
constexpr u64 ShadowConfig = 0x8f8;  // R+0x8f8 shadow-cascade config
constexpr u64 DrawTable = 0xd8;      // R+0xd8 draw table (G-buffer)
} // namespace RendererOff

inline u64 FramePtr() {
    return *reinterpret_cast<u64*>(RendererPtr() + RendererOff::FrameObject);
}

// Frame object F field offsets (proven).
namespace FrameOff {
constexpr u64 ConfigBlock = 0x280;  // 0xac8-byte per-frame config block
constexpr u64 ViewConfig = 0xb8;    // F+0xb8 view config (2nd arg of MainCamera)
constexpr u64 FovTan = 0xd88;       // fov tangent source
constexpr u64 ProjConstant = 0xd8c; // derived projection constant
constexpr u64 ProjOverride = 0xd90; // projection-override
constexpr u64 TargetManager = 0xd98; // F+0xd98 = M
constexpr u64 JobListBegin = 0xda8;
constexpr u64 JobListEnd = 0xdb0;
constexpr u64 ViewSwapFlag = 0x754;
} // namespace FrameOff

inline u64 TargetManagerPtr() {
    return *reinterpret_cast<u64*>(FramePtr() + FrameOff::TargetManager);
}

// Job context (ctx): an array of qwords (proven).
namespace ContextOff {
constexpr u64 Device = 0x00;         // [0] cb / device cursor object
constexpr u64 RenderContext = 0x10;  // [2] RC render context
constexpr u64 TargetManager = 0x18;  // [3] V
constexpr u64 CurrentTarget = 0x30;  // [6] current target
constexpr u64 ConfigFlags = 0x48;    // [9] config flags
constexpr u64 DrawTable = 0x60;      // [0xc] 0x50-byte draw table
constexpr u64 LabelSlots = 0xe8;     // [0x1d] label slot array
constexpr u64 PassSlot = 0xf4;       // +0xf4 pass slot k
constexpr u64 PassFlags = 0xf8;      // +0xf8 pass flag word
} // namespace ContextOff

inline u64 ContextDevice(u64 ctx) {
    return *reinterpret_cast<u64*>(ctx + ContextOff::Device);
}

// Device / GNM cursor object cb field offsets (proven).
namespace DeviceOff {
constexpr u64 End = 0x08;         // write-buffer end
constexpr u64 Cursor = 0x10;      // write cursor
constexpr u64 ReserveCb = 0x18;   // reserve callback
constexpr u64 ReserveArg = 0x20;  // callback arg
constexpr u64 Flags = 0x28;       // flags (bit0 = predication active)
constexpr u64 MaterialBlock = 0x2e8; // material block base (ctx+0x2e8)
constexpr u64 GeometryVtx = 0xc378;  // geometry / vertex-buffer object
constexpr u64 GeometryA = 0xc398;    // geometry descriptor A
constexpr u64 GeometryB = 0xc3a8;    // geometry descriptor B
constexpr u64 GeometryC = 0xc3b8;    // geometry descriptor C
constexpr u64 DrawKind = 0xc548;     // draw-kind selector
constexpr u64 VtxCacheDirty = 0xc8b0;
} // namespace DeviceOff

// Pass context (param_1 of pass/draw functions; *param_1 = cb) field offsets (proven).
namespace PassOff {
constexpr u64 Device = 0x00;      // [0] cb
constexpr u64 DirtyForce = 0x08;  // dirty/force word (bit0 forces all state writes)
constexpr u64 ColorDirty = 0x10;  // colour-dirty bitmask (one bit per RT)
constexpr u64 DepthDirty = 0x11;  // depth-dirty bit
constexpr u64 BlendDirty = 0x0c;
constexpr u64 StateDirty = 0x0d;
constexpr u64 DrawKind = 0x14;    // draw-kind/prim mode byte
constexpr u64 StageDescriptor = 0x18;
constexpr u64 StageDescriptor2 = 0x20;
constexpr u64 PrimitiveType = 0x1cc; // cached VGT_PRIMITIVE_TYPE
constexpr u64 InstanceVSharp = 0xa8; // instance V# descriptor
constexpr u64 PassSlot = 0xf4;
constexpr u64 IndexTypeCache = 0x1c8;     // +0x39: cached INDEX_TYPE (0 = reset, 0x93690:31)
constexpr u64 ShaderRecordCache = 0x1d8;  // +0x3b + i: per-stage shader-record cache slots
} // namespace PassOff

// Engine fullscreen-shader chain, resolved by the 0x93690 clear draw (all offsets proven,
// out/re/decomp/0009/00093690.c and native-render-map-pass.md §3.1). The root is an
// image-anchored global; every pointer below it is a runtime (host) address:
//   root+0x8a8 -> shader list; list+0x68 -> holder; holder+0x20 -> lazy loader slot (the byte at
//   holder+0x28 is its loaded flag); 0x14d7b0(loader) -> the shared fullscreen shader record
//   (in the host image; the guest reaches it through a guest address whose bias to the image is not
//   established); the record's +0x60 is the shader program the fullscreen rect draws with.
namespace EngineOff {
constexpr VAddr RootGlobals = 0x2ee3ce0; // DAT_02ee3ce0, image-anchored
constexpr u64 ShaderList = 0x8a8;        // root -> engine shader list pointer
constexpr u64 ShaderHolder = 0x68;       // list -> holder object
constexpr u64 HolderLoader = 0x20;       // holder -> lazy loader slot (0x95580 loads into it)
constexpr u64 HolderLoadedFlag = 0x28;   // holder -> byte loaded flag of the loader slot
// 0x14d7b0 returns the shader record the draw uses; the port needs no field of it besides the
// program, and the record's +0x19 type byte the 0xae150 binder tests is a GUEST (runtime) address.
constexpr u64 RecordProgram = 0x60;      // returned record -> shader program pointer
constexpr u64 RecordHostAddress = 0x19;  // returned record -> its guest address (low dword of the
                                         // pointer the guest caches; +0x19 is the record's type byte)
} // namespace EngineOff

// Pass descriptor (param_2 of pass functions) field offsets (proven).
namespace PassDescOff {
constexpr u64 Type = 0x62;          // type byte
constexpr u64 FlagsA = 0xf5;        // 0x93960 flags (bit0x20)
constexpr u64 FlagsB = 0x167;       // 0x94600 flags (bit0x20)
constexpr u64 ColorTargets = 0xe4;  // colour-target block (8 qwords)
constexpr u64 ColorTargetsAlt = 0x124;
constexpr u64 ViewportDims = 0x120; // low16 w, high16 h
constexpr u64 ViewportDimsB = 0x194;
constexpr u64 DepthTarget = 0x164;  // depth-target descriptor block
} // namespace PassDescOff

// Predicate flag helpers (SET_PREDICATION semantics: a memory value, not a query).
inline bool PredicationActive(u64 cb) {
    return (*reinterpret_cast<u8*>(cb + DeviceOff::Flags) & 1) != 0;
}

/// Logs the resolved root objects once (G/R/F/M); used to confirm the native backend and the
/// emulated path agree on the frame being rendered.
void LogRootObjects();

} // namespace Native::Guest
