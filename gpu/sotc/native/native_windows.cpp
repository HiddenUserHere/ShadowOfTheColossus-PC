// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the bridge between the pool's windows and native.h (see native_windows.h).
#include "native_windows.h"

#include "native.h"

namespace SotcNativeWindows {

bool ModeOn() {
    return SotcNative::On();
}

bool RegisterRange(uint64_t base, uint64_t size, const char* name) {
    return SotcNative::RegisterWindow(base, size, name);
}

bool ImportBlock(uint64_t base, uint64_t size) {
    return SotcNative::ImportWindowBlock(base, size);
}

void DropBlock(uint64_t base, uint64_t size) {
    SotcNative::DropWindowBlock(base, size);
}

} // namespace SotcNativeWindows
