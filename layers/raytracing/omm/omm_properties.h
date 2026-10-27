// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "omm_device.h"
#include <memory>

namespace vel::omm {
struct PropertyBackend;
struct PropertyRecording;
using PropertyRecordingRef = std::shared_ptr<PropertyRecording>;
struct PropertyBackendCreateInfo {
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    DeviceBackend* micromaps{};
    const VkAllocationCallbacks* allocator{};
    const uint32_t* property_code{};
    size_t property_code_size{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties{};
};
// Planned public EXT serialization: the specified two UUIDs, then pointer-free
// canonical storage. Size queries reserve exactly this envelope; serialization
// and public compatibility remain separate, currently unsupported operations.
constexpr uint64_t kMicromapSerializationHeaderBytes = 2 * VK_UUID_SIZE;
VkResult properties_create(const PropertyBackendCreateInfo&, PropertyBackend**);
// Release executable-CB and pending-submission tokens before backend/device teardown.
void properties_destroy(PropertyBackend*);
bool properties_is_pool(PropertyBackend*, VkQueryPool) noexcept;
VkResult properties_create_pool(PropertyBackend*, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool*);
void properties_destroy_pool(PropertyBackend*, VkQueryPool, const VkAllocationCallbacks*);
VkResult properties_reset_host(PropertyBackend*, VkQueryPool, uint32_t first, uint32_t count);
VkResult properties_get_results(PropertyBackend*, VkQueryPool, uint32_t first, uint32_t count, size_t data_size, void* data,
                                VkDeviceSize stride, VkQueryResultFlags);
// Queue family is that of the command pool. Timestamp-less families currently
// fail closed rather than silently creating unavailable logical queries.
// All allocation/validation precedes command recording; the caller restores
// compute/push state and retains each token for CB and pending-submission life.
VkResult properties_cmd_reset(PropertyBackend*, VkCommandBuffer, uint32_t queue_family, VkQueryPool, uint32_t first, uint32_t count,
                              PropertyRecordingRef*);
VkResult properties_cmd_write(PropertyBackend*, VkCommandBuffer, uint32_t queue_family, uint32_t count, const VkMicromapEXT*,
                              VkQueryType, VkQueryPool, uint32_t first, PropertyRecordingRef*);
// Destination needs native SHADER_DEVICE_ADDRESS in addition to public
// TRANSFER_DST, solely for known-span validation in DeviceBackend. Formatting
// uses private storage; a final native transfer preserves public copy scopes.
VkResult properties_cmd_copy_results(PropertyBackend*, VkCommandBuffer, uint32_t queue_family, VkQueryPool, uint32_t first,
                                     uint32_t count, VkBuffer destination, VkDeviceSize offset, VkDeviceSize stride,
                                     VkQueryResultFlags, PropertyRecordingRef*);
}  // namespace vel::omm
