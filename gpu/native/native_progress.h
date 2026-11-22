// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. Progress notification (W1, SOTC_CP_EVENTS, N2-94 plan) for code
// that has no Liverpool pointer (Vulkan scheduler priority thread, video-out driver). Liverpool
// installs its NotifyProgress here at construction; Notify() is a no-op until then.
#pragma once

#include <atomic>

namespace Native::Progress {

using NotifyFn = void (*)(void* ctx);

struct Hook {
    std::atomic<NotifyFn> fn{nullptr};
    std::atomic<void*> ctx{nullptr};
};

inline Hook& GetHook() {
    static Hook hook;
    return hook;
}

inline void SetNotifier(NotifyFn fn, void* ctx) {
    GetHook().ctx.store(ctx, std::memory_order_release);
    GetHook().fn.store(fn, std::memory_order_release);
}

/// A value a GPU queue may be waiting on changed: wake the command processor if it sleeps.
inline void Notify() {
    if (const NotifyFn fn = GetHook().fn.load(std::memory_order_acquire)) {
        fn(GetHook().ctx.load(std::memory_order_acquire));
    }
}

} // namespace Native::Progress
