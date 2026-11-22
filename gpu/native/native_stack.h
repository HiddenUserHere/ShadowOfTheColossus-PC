// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Runs host C++ work on a large per-thread stack.
//
// The pass hooks are entered from guest job fibers whose stacks are 16-64 KiB, packed back to
// back without guard pages. The native bodies (texture cache lookups, Vulkan recording, the pass
// derivations, stdio traces) need far more and overflowed 'BPE Job Fiber 130 (64 KiB)' in run
// probe-passonly-30 (gap N2-63). Same technique as the loader's fault handler (src/probe.c
// call_on_stack): switch rsp to a committed 1 MiB host stack and point the TEB stack bounds at it
// for the duration, so stack probes and nested faults see a consistent stack.
#pragma once

#include <utility>

namespace Native {

using StackFn = long long (*)(void* arg);

/// Calls fn(arg) on this thread's large host stack (allocated on first use). Runs it in place
/// when the thread is already on that stack or when the stack cannot be allocated.
long long CallOnHostStack(StackFn fn, void* arg);

/// Convenience wrapper: runs a callable returning bool on the large host stack.
template <typename F>
bool RunOnHostStack(F&& f) {
    auto thunk = [](void* p) -> long long { return (*static_cast<F*>(p))() ? 1 : 0; };
    return CallOnHostStack(thunk, const_cast<void*>(static_cast<const void*>(&f))) != 0;
}

} // namespace Native
