// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Native GPU work placed in the guest's PM4 command stream.
//
// While the render is hybrid (some guest functions native, the rest still emitting PM4 that the
// command processor consumes later on its own threads), a native operation cannot record Vulkan
// commands from the guest thread that runs the hook:
//   - the hook runs while the guest is still BUILDING a command buffer that will be submitted
//     later, so "now" is before the packets that precede the hook in that same buffer;
//   - several job fibers build different command buffers at once, and the order that counts is
//     the submission order;
//   - the command processor records into the same VkCommandBuffer from its own thread (gap N2-64:
//     the NV driver fault was that race).
// So the hook queues the operation here and writes a PM4 NOP carrying its id into the guest
// command buffer at the point where the guest body would have emitted its packets. Real hardware
// skips a NOP; the port's command processor (liverpool.cpp, PM4 NOP handler) recognises the
// payload and runs the queued operation on its draw thread, in stream order with the emulated
// draws around it.
//
// Packet: Type-3 NOP: header 0xC0001000 | (body - 1) << 16 (opcode 0x10), payload
// PM4CmdNop::PayloadType::SotcNativeOp, op id (bit 31 = writes guest memory), then optional
// padding + payload space (EmitWithSpace).
#pragma once

#include "common/types.h"
#include "common/unique_function.h"

namespace Native::StreamOps {

/// Tag dword after the op id: the rest of the NOP body is a prefetch shadow of PM4 packets
/// (gnm_common.h Shadow), walked only by Liverpool::PrefetchComputeShaders.
inline constexpr u32 kShadowTag = 0x68750f10u;

/// SOTC_NATIVE_STREAM_OPS=0 turns the mechanism off (callers then record immediately, the old
/// racy path, kept only for A/B). Default on.
bool Enabled();

/// Queues `op` and writes its NOP into the guest command buffer object `cb` (the GNM command
/// buffer the pass context's [0] points at: +0x08 end, +0x10 write cursor, +0x18 refill callback,
/// +0x20 its argument; layout as in the guest's own packet writers, e.g. 0x86be10). Returns false
/// and drops `op` when the packet cannot be written.
///
/// `writes_memory`: the op writes guest memory from the CPU side (WRITE_DATA / DMA / label
/// writes). The command processor then publishes it so later guest-memory reads wait for it
/// (Liverpool::BackPublish / WaitBackWrites). It travels in bit 31 of the id dword.
///
/// `where`: DrawThread (default) runs the op on the thread that records the draws, in stream
/// order with them (Vulkan work: clears, fences). CommandProcessor runs it INLINE on the command
/// processor thread when the parser reaches the NOP, before any later packet: register/state
/// writes and draw/dispatch submission through the Liverpool native facade (NativeSetRegs,
/// NativeSubmitDraw, ...), so emulated packets that still follow see the state. It travels in
/// bit 30 of the id dword.
enum class Where : u8 { DrawThread, CommandProcessor };

bool Emit(u64 cb, Common::UniqueFunction<void>&& op, bool writes_memory = false,
          Where where = Where::DrawThread);

/// Like Emit, but the NOP also carries `body_dwords` dwords of payload space starting at an
/// address aligned to `align_bytes` (a power of two, >= 4); returns that address (0 on failure).
/// For emitters whose guest contract returns a pointer into the command buffer (e.g. the label
/// slot of 0x86d210): real hardware and the command processor both skip the NOP body.
u64 EmitWithSpace(u64 cb, Common::UniqueFunction<void>&& op, u32 body_dwords, u32 align_bytes,
                  bool writes_memory = false, Where where = Where::DrawThread);

/// For the sceGnm* driver functions (gnmdriver.cpp): the eboot wrapper has already reserved a
/// fixed slot of `size_dwords` dwords at `slot` and advanced its cursor, so the op's NOP must fill
/// exactly that slot (header count = size - 2, payload, id, zero padding). `size_dwords` >= 3.
bool EmitInSlot(u32* slot, u32 size_dwords, Common::UniqueFunction<void>&& op,
                bool writes_memory = false, Where where = Where::DrawThread);

/// A WAIT placed in the stream (gap N2-76): the command processor polls `predicate` at the NOP and,
/// while it is false, yields its coroutine exactly as the WAIT_REG_MEM handler does
/// (YIELD_WAIT_GFX / YIELD_WAIT_ASC), so the other queues keep running and can produce the awaited
/// value. A wait must never spin inside an ordinary op: that blocks the whole command processor
/// thread (run 44 hung at start-up that way). Payload PM4CmdNop::PayloadType::SotcNativeWait.
bool EmitWait(u64 cb, Common::UniqueFunction<bool>&& predicate);

/// The DE-side wait on the constant engine (WAIT_ON_CE_COUNTER semantics): the graphics parser
/// resumes the CE task until `predicate` holds, instead of yielding. Payload SotcNativeWaitCe.
bool EmitWaitCe(u64 cb, Common::UniqueFunction<bool>&& predicate);

/// EmitWait into a sceGnm* fixed slot of `size_dwords` (>= 3) dwords (padding zeroed).
bool EmitWaitInSlot(u32* slot, u32 size_dwords, Common::UniqueFunction<bool>&& predicate);

/// A native INDIRECT_BUFFER: the parser of the stream that reaches this NOP runs the buffer
/// [`address`, `address` + `size_dwords` * 4) as a nested task, exactly like IT_INDIRECT_BUFFER
/// (graphics: ProcessGraphics nested; compute: ProcessCompute<true> on the same queue). Five
/// dwords: NOP header, PM4CmdNop::PayloadType::SotcNativeIb, address lo, address hi, size.
bool EmitIb(u64 cb, u64 address, u32 size_dwords);

/// Polls wait `id_dword` (the NOP's third dword) on the command processor thread: true when the
/// predicate holds (the wait is then forgotten) or the id is unknown.
bool Poll(u32 id_dword);

/// Bit 31 of the id dword: the op writes guest memory (see Emit).
constexpr u32 kWritesMemoryBit = 0x80000000u;
/// Bit 30 of the id dword: run inline on the command processor thread (Where::CommandProcessor).
constexpr u32 kRunOnCpBit = 0x40000000u;
constexpr u32 kIdFlagMask = kWritesMemoryBit | kRunOnCpBit;

/// Runs and forgets the operation `id_dword` (the NOP's third dword, writes-memory bit
/// included). Called by the command processor's NOP handlers on the thread that records the
/// draws. An unknown id (a command buffer replayed twice) is ignored and counted.
void Run(u32 id_dword);

} // namespace Native::StreamOps
