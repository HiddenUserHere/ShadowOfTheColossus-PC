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
// Packet: Type-3 NOP, 3 dwords: header 0xC0011000 (count 1, opcode 0x10), payload
// PM4CmdNop::PayloadType::SotcNativeOp, op id.
#pragma once

#include "common/types.h"
#include "common/unique_function.h"

namespace Native::StreamOps {

/// SOTC_NATIVE_STREAM_OPS=0 turns the mechanism off (callers then record immediately, the old
/// racy path, kept only for A/B). Default on.
bool Enabled();

/// Queues `op` and writes its NOP into the guest command buffer object `cb` (the GNM command
/// buffer the pass context's [0] points at: +0x08 end, +0x10 write cursor, +0x18 refill callback,
/// +0x20 its argument; layout as in the guest's own packet writers, e.g. 0x86be10). Returns false
/// and drops `op` when the packet cannot be written.
bool Emit(u64 cb, Common::UniqueFunction<void>&& op);

/// Runs and forgets the operation `id`. Called by the command processor's NOP handler on the
/// thread that records the draws. An unknown id (a command buffer replayed twice) is ignored.
void Run(u32 id);

} // namespace Native::StreamOps
