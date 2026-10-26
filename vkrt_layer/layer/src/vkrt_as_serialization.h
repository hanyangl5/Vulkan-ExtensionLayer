/* SPDX-License-Identifier: MIT */
#ifndef VKRT_AS_SERIALIZATION_H
#define VKRT_AS_SERIALIZATION_H
#include <vulkan/vulkan.h>

/* Vulkan's public prefix is followed by a 16-byte-aligned canonical BVH4
 * payload. One relocation entry is emitted per live TLAS instance. */
#define VKRT_AS_SERIAL_HEADER_SIZE UINT64_C(56)
#define VKRT_AS_SERIAL_HANDLES_OFFSET UINT64_C(56)
#define VKRT_AS_SERIAL_FORMAT_VERSION 1u
static inline uint64_t vkrt_as_serialization_payload(uint32_t instance_count) {
    return (VKRT_AS_SERIAL_HEADER_SIZE + UINT64_C(8) * instance_count + UINT64_C(15)) & ~UINT64_C(15);
}
static inline uint64_t vkrt_as_serialization_size(uint64_t physical_size, uint32_t instance_count) {
    uint64_t payload = vkrt_as_serialization_payload(instance_count);
    return physical_size <= UINT64_MAX - payload ? payload + physical_size : 0;
}
struct vkrt_device_data;
struct vkrt_serialization_state;
struct vkrt_serialization_args {
    uint64_t source, destination, as_capacity;
    uint32_t destination_type, reserved;
    uint32_t version_data[8];
};
VkResult vkrt_as_serialization_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator);
void vkrt_as_serialization_finish(struct vkrt_device_data *dev);
void vkrt_as_serialization_compatibility(struct vkrt_device_data *dev, const VkAccelerationStructureVersionInfoKHR *info,
                                         VkAccelerationStructureCompatibilityKHR *compatibility);
void vkrt_cmd_serialize_as(struct vkrt_device_data *dev, VkCommandBuffer command,
                           const VkCopyAccelerationStructureToMemoryInfoKHR *info);
void vkrt_cmd_deserialize_as(struct vkrt_device_data *dev, VkCommandBuffer command,
                             const VkCopyMemoryToAccelerationStructureInfoKHR *info);
#endif
