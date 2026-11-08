// SPDX-License-Identifier: GPL-2.0-or-later
#include "manifest_capture.h"

#include <cassert>

int main() {
    Shader::Info info{};
    Shader::StageSpecialization stored{};
    stored.info = &info;
    stored.runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    stored.buffers.emplace_back();
    stored.bitset[0] = true;
    auto current = stored;
    assert(SotcCache::CanCaptureSpecialization(stored, current));
    current.bitset[0] = false;
    // Real shader matching accepts a null binding with identical zero format/stride fields.
    // A preparation recipe must distinguish it: the descriptor hints do not describe stored.
    assert(stored == current && current == stored);
    assert(!SotcCache::CanCaptureSpecialization(stored, current));
    current = stored;
    current.buffers[0].stride = 16;
    assert(!SotcCache::CanCaptureSpecialization(stored, current));
    current = stored;
    current.runtime_info.props.num_user_data = 1;
    assert(!SotcCache::CanCaptureSpecialization(stored, current));
}
