#pragma once

#include "../types.h"

namespace vk {

struct block_device;

namespace fs {
namespace mounts {

struct partition_view {
    block_device* device = null;
    u64 start_lba = 0;
    u64 sector_count = 0;
};

using partition_mount_callback = bool (*)(const partition_view& partition, void* context);

auto mount_gpt_partition_index(u32 partition_index, partition_mount_callback callback, void* context) -> status_code;
auto mount_first_available(partition_mount_callback callback, void* context) -> status_code;

} // namespace mounts
} // namespace fs

} // namespace vk