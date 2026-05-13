/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * framebuffer.h - Shared framebuffer helpers.
 */

#ifndef VKERNEL_FRAMEBUFFER_H
#define VKERNEL_FRAMEBUFFER_H

#include "types.h"
#include "vk.h"

namespace vk {
namespace framebuffer {

[[nodiscard]] inline auto byte_size(const vk_framebuffer_info_t& fb, usize& out_size) -> bool {
    out_size = 0;
    if (fb.valid == 0u || fb.base == 0u || fb.width == 0u || fb.height == 0u || fb.stride < fb.width) {
        return false;
    }

    const u64 pixels = static_cast<u64>(fb.stride) * static_cast<u64>(fb.height);
    const u64 bytes = pixels * sizeof(vk_u32);
    if (pixels == 0 || bytes == 0 || bytes > static_cast<u64>(~static_cast<usize>(0))) {
        return false;
    }

    out_size = static_cast<usize>(bytes);
    return true;
}

} // namespace framebuffer
} // namespace vk

#endif /* VKERNEL_FRAMEBUFFER_H */
