// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "omm_device.h"
#include <memory>

namespace vel::omm {
struct AccelerationBackend;
// Cell at RootCell+64 in the same owned allocation. Only build GPU commands
// publish state/native/root; creating or binding an AS never mutates a pending
// descriptor shadow. The app's AS construction dependencies cover its readers.
constexpr uint32_t kAccelerationIdentityMagic = 0x494d4d4f;  // LE "OMMI".
struct AccelerationIdentityCell {
    uint32_t magic, version, state, reserved;
    uint64_t native_address, root_cell_address;
};
static_assert(sizeof(AccelerationIdentityCell) == 32);
struct AccelerationRecording;
using AccelerationRecordingRef = std::shared_ptr<AccelerationRecording>;
struct AccelerationBackendCreateInfo {
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    DeviceBackend* micromaps{};
    const VkAllocationCallbacks* allocator{};
    const uint32_t* metadata_code{};
    size_t metadata_code_size{};
    const uint32_t* instances_code{};
    size_t instances_code_size{};
    const uint32_t* publish_code{};
    size_t publish_code_size{};
    // Layer-owned physical handles must use the matching downstream instance
    // dispatch; only standalone application fixtures may use loader trampolines.
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties{};
};
VkResult acceleration_backend_create(const AccelerationBackendCreateInfo&, AccelerationBackend**);
// All command/descriptor/submission lifetime tokens must be released first.
void acceleration_backend_destroy(AccelerationBackend*);
// Call immediately after native create success; failure requires native destroy.
// The native handle and native AS address are never substituted.
VkResult acceleration_track(AccelerationBackend*, VkAccelerationStructureKHR, const VkAccelerationStructureCreateInfoKHR*);
void acceleration_untrack(AccelerationBackend*, VkAccelerationStructureKHR);
// Stable reference is available before binding/construction and never queries a
// native address. This is the application descriptor resolver entry point.
VkResult acceleration_reference(AccelerationBackend*, VkAccelerationStructureKHR, VkDeviceAddress* identity_cell_address,
                                VkDeviceAddress* root_address, std::shared_ptr<void>* lifetime = nullptr);
// Diagnostic/native-address resolution requires tracked, bound backing storage.
VkResult acceleration_identity(AccelerationBackend*, VkAccelerationStructureKHR, VkDeviceAddress* native_address,
                               VkDeviceAddress* root_address, std::shared_ptr<void>* lifetime = nullptr);
VkResult acceleration_root_from_address(AccelerationBackend*, VkDeviceAddress native_address, VkDeviceAddress* root_address,
                                        std::shared_ptr<void>* lifetime = nullptr);
// DEVICE BUILD and UPDATE preserve the native operation and scratch sizes.
// UPDATE supplies a complete geometry/OMM description as required by Vulkan;
// omitting an association never means inheriting the source's old association.
// pGeometries and ppGeometries are copied; original application inputs are untouched.
VkResult acceleration_get_build_sizes(AccelerationBackend*, VkAccelerationStructureBuildTypeKHR,
                                      const VkAccelerationStructureBuildGeometryInfoKHR*, const uint32_t*,
                                      VkAccelerationStructureBuildSizesInfoKHR*);
// Complete static validation and all allocations precede the first command.
// Restoring app compute/push state is the layer's responsibility. Retain result
// throughout executable-CB lifetime and separately through each pending submission.
// Before releasing a completed submission's last token, collect it while all
// writers to its touched ASes are quiescent. Unsubmitted recordings may simply
// be discarded; only their own tokens retain unpublished body allocations.
VkResult acceleration_cmd_build(AccelerationBackend*, VkCommandBuffer, uint32_t, const VkAccelerationStructureBuildGeometryInfoKHR*,
                                const VkAccelerationStructureBuildRangeInfoKHR* const*, AccelerationRecordingRef*);
// Call only after downstream vkDeviceWaitIdle succeeds, with AS operations and
// queue submissions externally excluded. Reads GPU-published RootCells and drops
// obsolete AS body ownership; executable/pending tokens still retain their bodies.
// This explicit retirement point prevents permanent retain-until-device-destroy.
void acceleration_collect_idle(AccelerationBackend*);
// Finer retirement: caller proves every GPU writer to each AS touched by this
// recording has completed, and excludes concurrent writers/submissions while
// collecting. A single completed fence alone is insufficient if another writer
// to that AS remains pending. This permits bounded fence-driven reclamation.
void acceleration_collect_completed(AccelerationBackend*, const AccelerationRecordingRef&);
// Allocation-free identity comparison for submission retirement. A recording
// conservatively touches every destination AS whose root it can publish.
// UPDATE source retention is a native read lifetime, not a metadata writer:
// independent target bodies never dereference the source's private RootCell.
bool acceleration_recordings_overlap(const AccelerationRecordingRef&, const AccelerationRecordingRef&) noexcept;
}  // namespace vel::omm
