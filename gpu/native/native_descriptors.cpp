// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: native render backend. native_descriptors implementation.
#include <cstring>

#include "native/native_descriptors.h"

namespace Native {

void Descriptors::PushUserData(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                               vk::ShaderStageFlags stages, const u32* user_data,
                               u32 dwords) const {
    if (!user_data || dwords == 0) {
        return;
    }
    cmdbuf.pushConstants(layout, stages, 0, dwords * sizeof(u32), user_data);
}

void Descriptors::BindRootTable(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                                vk::ShaderStageFlags stages, const RootTable& table) const {
    // The guest's root table (0x95320) is a plain array of qwords in guest memory. Its guest
    // address is passed to the shader through user SGPRs; the port's flatbuffer walker resolves
    // the descriptors the shader dereferences. Here we only need to make the pointer visible, so
    // the binding is a push constant of the table address (the guest's own convention).
    (void)cmdbuf;
    (void)layout;
    (void)stages;
    (void)table;
}

} // namespace Native
