// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace vel::omm {

struct DescriptorASIdentity {
    VkDeviceAddress native_address = 0;
    VkDeviceAddress root_cell_address = 0;
    std::shared_ptr<void> lifetime;
};
using DescriptorASResolver = VkResult (*)(void*, VkAccelerationStructureKHR, DescriptorASIdentity*);

struct DescriptorBackend;
struct DescriptorContext;
struct DescriptorAllocation;
struct DescriptorBackendCreateInfo {
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};  // Downstream only.
    const VkAllocationCallbacks* allocator{};
    DescriptorASResolver resolver{};
    void* resolver_user_data{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties{};
};
struct DescriptorContextRef {
    VkDeviceAddress address = 0;
    std::shared_ptr<const DescriptorContext> resource;
};

// Runtime BDA wire ABI v1; no application binding or push-constant allocation.
constexpr uint32_t kDescriptorContextMagic = 0x444d4d4f;  // LE "OMMD".
struct DescriptorContextHeader {
    uint32_t magic, version, binding_count, binding_stride;
    uint64_t bytes, reserved;
};
struct DescriptorContextBinding {
    uint32_t set, binding;
    uint64_t records_address, record_count, records_bytes;
};
struct DescriptorShadowRecord {
    uint64_t native_address, root_cell_address;
};
static_assert(sizeof(DescriptorContextHeader) == 32 && sizeof(DescriptorContextBinding) == 32 &&
              sizeof(DescriptorShadowRecord) == 16);

VkResult descriptor_backend_create(const DescriptorBackendCreateInfo&, DescriptorBackend**);
// Release all command-buffer/pipeline context references BEFORE destroying the
// native device. GPU resources can intentionally outlive this CPU tracker.
void descriptor_backend_destroy(DescriptorBackend*);
VkResult descriptor_track_layout(DescriptorBackend*, VkDescriptorSetLayout, const VkDescriptorSetLayoutCreateInfo*);
void descriptor_untrack_layout(DescriptorBackend*, VkDescriptorSetLayout);
VkResult descriptor_track_pool(DescriptorBackend*, VkDescriptorPool, const VkDescriptorPoolCreateInfo*);
void descriptor_untrack_pool(DescriptorBackend*, VkDescriptorPool);
void descriptor_reset_pool(DescriptorBackend*, VkDescriptorPool);
// Prepare BEFORE downstream vkAllocateDescriptorSets. This performs every CPU/
// GPU allocation, snapshots layout/variable-count definitions, and reserves map
// capacity accounting for all outstanding transactions, including other pools.
// On native failure abort; on native success commit the returned native handles.
// Commit/abort consume the token, allocate nothing, and never wait for the GPU.
// Backend and native pool must outlive the transaction. Native handles passed
// to commit must be non-null and unique, as guaranteed by valid native allocation.
VkResult descriptor_prepare_allocate_sets(DescriptorBackend*, const VkDescriptorSetAllocateInfo*, DescriptorAllocation**);
void descriptor_commit_allocate_sets(DescriptorAllocation*, const VkDescriptorSet*) noexcept;
void descriptor_abort_allocate_sets(DescriptorAllocation*) noexcept;
// Legacy component-test convenience, after successful downstream allocation.
// Application-facing allocation must use the transaction above for rollback on
// pools without FREE_DESCRIPTOR_SET. Non-AS bindings need no GPU storage.
VkResult descriptor_allocate_sets(DescriptorBackend*, const VkDescriptorSetAllocateInfo*, const VkDescriptorSet*);
void descriptor_free_sets(DescriptorBackend*, VkDescriptorPool, uint32_t, const VkDescriptorSet*);
// Call in Vulkan update order: all writes, then all copies. All shadow changes
// are validated before publication. Native descriptor storage is never read.
VkResult descriptor_update(DescriptorBackend*, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
// Slots are the actual logical set numbers; VK_NULL_HANDLE leaves a slot unbound.
// Context headers/tables never mutate. They retain their set allocations across
// pool reset/free; legal native command-buffer and descriptor lifetime rules
// still apply. Repeated live combinations reuse a weakly cached resource.
VkResult descriptor_build_context(DescriptorBackend*, const std::vector<VkDescriptorSet>&, DescriptorContextRef*);
// Checked CPU mirror for diagnostics/fixtures. Unwritten/out-of-range records
// fail explicitly. An explicitly written robustness2 NULL descriptor succeeds
// with native/root {0,0}, preserving native null traversal behavior.
VkResult descriptor_context_fetch(const DescriptorContextRef&, uint32_t, uint32_t, uint64_t, DescriptorASIdentity*);

// Publication contract: HOST_VISIBLE|HOST_COHERENT BDA storage is written before
// queue submission; Vulkan submission publishes preceding host writes. Only
// ordinary descriptor updates are supported. AS UPDATE_AFTER_BIND and
// UPDATE_UNUSED_WHILE_PENDING are rejected: a 16-byte pair has no assumed host/
// device atomicity. Descriptor templates, push descriptors, descriptor buffers,
// mutable AS bindings, and integer-address resolution require separate adapters.
// This tracker never publishes a mutable process-wide scene/context head.
}  // namespace vel::omm
