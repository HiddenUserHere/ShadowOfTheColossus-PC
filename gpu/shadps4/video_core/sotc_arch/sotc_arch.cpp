// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: shared helpers of the draw architecture (sotc_arch.h). Each part implements its namespace in its own file.
#include <cstdlib>

#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch {

int SwitchMode(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return 0;
    }
    return v[0] == '2' ? 2 : v[0] == '1' ? 1 : 0;
}

} // namespace SotcArch
