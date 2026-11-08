// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: real flight recorder on a small guest-sized host stack; no Vulkan or game.
#include "block_ledger.h"
#include "video_core/sotc_trace.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <thread>
#include <pthread.h>

namespace {
void* Record(void*) {
    SotcTrace::ThreadInit(); // allocate once, outside fault/ledger paths
    for (unsigned i = 0; i < 16; ++i) {
        const SotcBlockLedger::Scope scope{SotcBlockLedger::LibraryWait, i};
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return nullptr;
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && SotcTrace::kOn);
    std::filesystem::create_directories(argv[1]);
    SotcTrace::SetUserDir(argv[1]);
    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, 65536) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, &attr, Record, nullptr) == 0);
    pthread_attr_destroy(&attr);
    assert(pthread_join(thread, nullptr) == 0);
    assert(SotcTrace::RequestDump("ledger smoke test"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (SotcTrace::LastDump().serial == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    assert(SotcTrace::LastDump().serial != 0);
}
