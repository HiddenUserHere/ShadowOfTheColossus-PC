// SPDX-License-Identifier: GPL-2.0-or-later
#include "preparation_status.h"

#include <cassert>

int main() {
    SotcCache::PreparationStatus ready{};
    ready.last_request_ns = 1;
    ready.now_ns = 5'000'000'001LL;
    assert(ready.ReadyToFinalize()); // fully cached shaders must not hang forever at queued == 0
    auto busy = ready;
    busy.last_request_ns = 0;
    assert(!busy.ReadyToFinalize()); // never received any shader code
    busy = ready;
    --busy.now_ns;
    assert(!busy.ReadyToFinalize()); // loader can still announce another batch
    busy = ready;
    busy.queued = busy.completed = 200;
    assert(busy.ReadyToFinalize());
    busy.compute_builds = 1;
    assert(!busy.ReadyToFinalize()); // last translation done, driver compute build still running
    busy = ready;
    busy.translating = 1;
    assert(!busy.ReadyToFinalize());
    busy = ready;
    busy.backlog = 1;
    assert(!busy.ReadyToFinalize());
    busy = ready;
    busy.requests_waiting = true;
    assert(!busy.ReadyToFinalize());
    busy = ready;
    busy.deferred = true;
    assert(!busy.ReadyToFinalize());
    busy = ready;
    busy.queued = 1;
    assert(!busy.ReadyToFinalize());
}
