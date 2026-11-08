// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: actual ledger + trace sink stub; no game, Vulkan or trace writer.
#include "block_ledger.h"
#include "video_core/sotc_stats.h"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

namespace {
unsigned spans = 0;
uint64_t last_key{}, last_flip{};
std::string last_name;
} // namespace

namespace SotcTrace {
void SpanIfReady(const char* name, uint64_t start, uint64_t end, uint64_t key, uint64_t flip) {
    assert(end >= start);
    ++spans;
    last_key = key;
    last_flip = flip;
    last_name = name;
}
} // namespace SotcTrace

int main() {
    SotcStats::tl_role = SotcStats::RoleCp;
    SotcStats::flip_count.store(42);
    {
        const SotcBlockLedger::Scope scope{SotcBlockLedger::LibraryWait, 0x1234};
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    if (SotcTrace::kOn) {
        assert(spans == 1 && last_key == 0x1234 && last_flip == 42);
        assert(last_name == "library demand");
    } else {
        assert(spans == 0);
    }
    const auto frame = SotcBlockLedger::TakeFrameText();
    assert(frame.empty() == !SotcBlockLedger::kOn);
    if (SotcBlockLedger::kOn) {
        assert(frame.find("library demand") != std::string::npos);
    }
    assert(SotcBlockLedger::TakeFrameText().empty()); // snapshot advances independently of the 5 s report
}
