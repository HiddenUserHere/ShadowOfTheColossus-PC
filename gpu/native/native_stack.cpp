// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. See native_stack.h.

#include "native/native_stack.h"

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
// rcx = fn, rdx = arg, r8 = top of the new stack (16-byte aligned). Calls fn(arg) with rsp moved
// to `top` (minus the 32-byte home area), then restores the caller's rsp. No unwind info: nothing
// may throw across it (the native bodies do not use C++ exceptions).
extern "C" long long sotc_native_call_on_stack(Native::StackFn fn, void* arg, void* top);
__asm__(".text\n.p2align 4\n.globl sotc_native_call_on_stack\nsotc_native_call_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n mov %r8,%rsp\n sub $32,%rsp\n"
        " mov %rcx,%r9\n mov %rdx,%rcx\n call *%r9\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif

namespace Native {

namespace {
constexpr std::size_t kHostStackSize = 1u << 20;
thread_local unsigned char* t_stack = nullptr;
} // namespace

long long CallOnHostStack(StackFn fn, void* arg) {
#ifdef _WIN32
    if (t_stack == nullptr) {
        t_stack = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kHostStackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (t_stack == nullptr) {
            return fn(arg);
        }
    }
    const auto sp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    const auto lo = reinterpret_cast<std::uintptr_t>(t_stack);
    if (sp >= lo && sp < lo + kHostStackSize) {
        return fn(arg); // nested: already on the host stack
    }
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    void* const base = tib->StackBase;
    void* const limit = tib->StackLimit;
    tib->StackBase = t_stack + kHostStackSize;
    tib->StackLimit = t_stack;
    const long long result = sotc_native_call_on_stack(fn, arg, t_stack + kHostStackSize);
    tib->StackBase = base;
    tib->StackLimit = limit;
    return result;
#else
    return fn(arg);
#endif
}

} // namespace Native
