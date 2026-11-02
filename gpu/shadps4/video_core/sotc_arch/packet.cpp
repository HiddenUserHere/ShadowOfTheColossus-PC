// SPDX-FileCopyrightText: Copyright 2026 sotc-pc port
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_PACKET (ARCH-DRAW.md part 4): one push-constant packet per op. The layout is decided per
// pipeline by its profile (Pipeline::packet_range, vk_pipeline_common.h: PacketPushOffset and
// PacketPushBytes), and BindResources pushes the packet. This file holds the switch only.
#include "video_core/sotc_arch/sotc_arch.h"

namespace SotcArch::Packet {

bool On() {
    static const bool on = SwitchMode("SOTC_PACKET") != 0;
    return on;
}

} // namespace SotcArch::Packet
