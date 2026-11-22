// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. guest_bridge: the single translation unit that anchors the
// header-only typed views and holds the guest-base sanity check the native backend relies on.
#include <atomic>
#include <cstdio>

#include "native/guest_bridge.h"

// page_manager.cpp holds the image base probe.c publishes right after mapping the eboot.
extern "C" u64 bb_get_guest_image(void);

namespace Native::Guest {

u64 GuestImageBase() {
    return bb_get_guest_image();
}

u64 ImageQword(VAddr va) {
    const u64 base = GuestImageBase();
    if (base == 0) {
        // No image yet (the native renderer can exist before probe.c maps the eboot). Not silent:
        // warn once per process, return 0, and let the caller chain to the guest body.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "SOTC RENDER native: GuestImageBase()==0, image-anchored global %#llx "
                         "unread (the guest chain will serve the draw)\n",
                         static_cast<unsigned long long>(va));
        }
        return 0;
    }
    return *reinterpret_cast<const u64*>(base + va);
}

/// Logs the resolved root objects once, so a run can confirm the native backend and the emulated
/// path agree on which frame is being rendered. G's slot is image-anchored; R/F/M are runtime
/// (host) pointers read out of it.
void LogRootObjects() {
    const u64 r = RendererPtr();
    if (r == 0) {
        std::fprintf(stderr, "SOTC RENDER native: renderer not published yet\n");
        return;
    }
    const u64 f = FramePtr();
    const u64 m = TargetManagerPtr();
    std::fprintf(stderr, "SOTC RENDER native: G=%#llx R=%#llx F=%#llx M=%#llx\n",
                 static_cast<unsigned long long>(kGlobalRenderStruct),
                 static_cast<unsigned long long>(r), static_cast<unsigned long long>(f),
                 static_cast<unsigned long long>(m));
}

} // namespace Native::Guest
