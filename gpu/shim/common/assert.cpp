// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/assert_scope.h"
#include "common/logging/log.h"
#ifdef _WIN32
#include <windows.h>
#endif

thread_local bool Common::g_assert_throws = false;

void assert_fail_impl() {
    if (Common::g_assert_throws) {
        throw Common::AssertFailure{};
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
#ifdef _WIN32
    // sotc: raw return addresses; resolve offline with llvm-symbolizer against bb-probe.exe.
    void* frames[32];
    const USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (USHORT i = 0; i < count; ++i) {
        std::fprintf(stderr, "    #%u exe+0x%llx\n", unsigned(i),
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - base));
    }
    std::fflush(stderr);
#endif
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
