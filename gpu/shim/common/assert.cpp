// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include "video_core/sotc_trace.h" // sotc: crash report (FatalDump)
#include "common/assert.h"
#include "common/assert_scope.h"
#include "common/logging/log.h"
#ifdef _WIN32
#include <windows.h>
#endif

thread_local bool Common::g_assert_throws = false;

// sotc: crash report (bbgpu_set_fatal_hook): the loader's every-thread stack dump.
static std::atomic<void (*)()> g_fatal_hook{nullptr};
static std::atomic<bool> g_stopping{false};

extern "C" void bbgpu_set_fatal_hook(void (*hook)(void)) {
    g_fatal_hook.store(hook, std::memory_order_release);
}

extern "C" int bbgpu_trace_fatal_dump(const char* reason, unsigned window_ms, unsigned wait_ms) {
    return SotcTrace::FatalDump(reason, window_ms, wait_ms) ? 1 : 0;
}

void assert_fail_impl() {
    if (Common::g_assert_throws) {
        throw Common::AssertFailure{};
    }
    // sotc: a second thread failing meanwhile (a device lost stops several) waits for the first report.
    if (g_stopping.exchange(true)) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
#ifdef _WIN32
    // sotc: return addresses as exe offsets; sotc.py symbolizes them into the crash log copy
    // (tools/crash_symbolize.py, llvm-symbolizer against out/gpu/bb-probe.exe).
    void* frames[32];
    const USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "  stopping thread %lu\n", GetCurrentThreadId());
    for (USHORT i = 0; i < count; ++i) {
        std::fprintf(stderr, "    #%u exe+0x%llx\n", unsigned(i),
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - base));
    }
    std::fflush(stderr);
#endif
    // sotc: crash report: every thread's stack (the loader's dump), then the recorder's last 2 s (SOTC_TRACE).
    if (const auto hook = g_fatal_hook.load(std::memory_order_acquire)) {
        hook();
    }
    if (SotcTrace::FatalDump("GPU stop (assertion / device lost)", 2000, 4000)) {
        std::fprintf(stderr, "SOTC TRACE: crash window written: %s\n", SotcTrace::LastDump().path.c_str());
    }
    std::fflush(stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
