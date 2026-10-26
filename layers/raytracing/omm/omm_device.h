// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>

namespace vel::omm {

struct DeviceBackend;
struct DeviceBackendCreateInfo {
    VkPhysicalDevice physical_device;
    VkDevice device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;  // Downstream, never the layer's dispatcher.
    const VkAllocationCallbacks* allocator;
    const uint32_t* build_code;
    size_t build_code_size;  // Bytes, omm_build.comp; 96-byte push ABI.
    const uint32_t* copy_code;
    size_t copy_code_size;  // Bytes, omm_copy.comp; 32-byte push ABI.
};

struct MicromapStorage {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize capacity;
    VkDeviceAddress address;
};

// Experimental device component; these functions do not advertise an extension.
// The caller privately enables shaderInt64/BDA and translates native buffer flags.
VkResult device_backend_create(const DeviceBackendCreateInfo& info, DeviceBackend** backend);
void device_backend_destroy(DeviceBackend* backend);

// Track original application sizes/usage. accessible_size optionally describes
// native word padding added by the loader and must be >= original size. Bind
// notification follows a successful native BindBufferMemory[2]. No BDA query is
// made for an unbound buffer. Untrack before destroying its native handle.
VkResult device_track_buffer(DeviceBackend* backend, VkBuffer buffer, const VkBufferCreateInfo* original_info,
                             VkDeviceSize accessible_size = 0);
void device_buffer_bound(DeviceBackend* backend, VkBuffer buffer);
void device_untrack_buffer(DeviceBackend* backend, VkBuffer buffer);

VkResult device_create_micromap(DeviceBackend* backend, const VkMicromapCreateInfoEXT* info, const VkAllocationCallbacks* allocator,
                                VkMicromapEXT* micromap);
void device_destroy_micromap(DeviceBackend* backend, VkMicromapEXT micromap, const VkAllocationCallbacks* allocator);
VkResult device_get_build_sizes(DeviceBackend* backend, VkAccelerationStructureBuildTypeKHR build_type,
                                const VkMicromapBuildInfoEXT* info, VkMicromapBuildSizesInfoEXT* sizes);
VkResult device_micromap_storage(DeviceBackend* backend, VkMicromapEXT micromap, MicromapStorage* storage);

// All static validation succeeds before recording any command. Recording uses
// only a private compute pipeline and push constants; the layer's command-state
// wrapper MUST restore application compute/push state afterwards. No descriptor
// bindings, queues, waits, submissions, or CPU reads of GPU inputs are used.
VkResult device_cmd_build(DeviceBackend* backend, VkCommandBuffer command_buffer, uint32_t info_count,
                          const VkMicromapBuildInfoEXT* infos);
VkResult device_cmd_copy(DeviceBackend* backend, VkCommandBuffer command_buffer, const VkCopyMicromapInfoEXT* info);

// Public Vulkan serialization is not implemented yet, so every public version
// identity is incompatible. Do not confuse the internal reference blob identity
// with VkMicromapVersionInfoEXT's Vulkan-defined serialization header.
void device_get_compatibility(DeviceBackend* backend, const VkMicromapVersionInfoEXT* info,
                              VkAccelerationStructureCompatibilityKHR* compatibility);

}  // namespace vel::omm
