// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: read extents of unbounded read-only storage buffers (SOTC_READ_EXTENT=1, off by default).
//
// Guest V#s often say "unbounded" (num_records 0xffffffff). BindBuffers then binds a 64 MiB window from the base
// (SHADPS4_UNBOUNDED_BUFFER_MB) and the bind syncs every CPU-dirty page of it on the draw thread. On a fresh 16 MiB
// region that is all of it: compute shader 0xfeaef49e binding 7 copied 63.8 MiB in one bind, a 60-300 ms frame when
// the camera turns or an area loads (docs/re-backend/15-burst-stutters.md).
//
// For the listed (hash, binding) pairs of READ-ONLY bindings, the bound range is the window the rule proves the shader
// can read, instead of the 64 MiB window. Rules come from user/read_extent.txt (SOTC_READ_EXTENT_FILE overrides the
// path), one per line, bytes from the V# base (decimal or 0x...):
//
//     <hash> <binding> <offset> <fixed>     # comment
//
// The window is [0, offset + fixed). binding is the index in Info::buffers (the SPIR-V Binding of a compute shader).
// PROOF OBLIGATION of the person who lists a shader: for every permutation and every dispatch, no load of that
// binding falls outside the window. A larger window is always safe. A read past a shortened binding returns 0
// (robustBufferAccess2 is required, vk_instance.cpp), not a fault, so a wrong rule shows as wrong data, not a crash.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace SotcReadExtent {

inline const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_READ_EXTENT");
    return v && v[0] == '1';
}();

struct Rule {
    uint64_t hash;
    uint32_t binding;
    uint64_t end; ///< offset + fixed
};

inline const std::vector<Rule>& Rules() {
    static const std::vector<Rule> rules = [] {
        std::vector<Rule> out;
        if (!kEnabled) {
            return out;
        }
        const char* path = std::getenv("SOTC_READ_EXTENT_FILE");
        if (!path || !path[0]) {
            path = "user/read_extent.txt";
        }
        std::FILE* f = std::fopen(path, "r");
        if (!f) {
            std::fprintf(stderr, "SOTC read extent: no rule file %s, nothing narrowed\n", path);
            return out;
        }
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            if (char* hash = std::strchr(line, '#')) {
                *hash = 0;
            }
            unsigned long long v[4]{};
            int n = 0;
            for (char* p = line; n < 4;) {
                char* end = nullptr;
                const unsigned long long x = std::strtoull(p, &end, 0);
                if (end == p) {
                    break;
                }
                v[n++] = x;
                p = end;
            }
            if (n < 4 || v[3] == 0) {
                continue;
            }
            out.push_back({v[0], static_cast<uint32_t>(v[1]), v[2] + v[3]});
        }
        std::fclose(f);
        std::fprintf(stderr, "SOTC read extent: %zu rule(s) from %s\n", out.size(), path);
        return out;
    }();
    return rules;
}

/// The bound size of a read-only binding of `size` bytes: the rule's window when one is listed and smaller.
inline uint64_t Limit(uint64_t hash, uint32_t binding, uint64_t size) {
    for (const Rule& r : Rules()) {
        if (r.hash == hash && r.binding == binding) {
            return r.end < size ? r.end : size;
        }
    }
    return size;
}

} // namespace SotcReadExtent
