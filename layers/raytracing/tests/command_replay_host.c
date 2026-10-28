/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Standalone CPU test. Compile with vkrt_replay.c and generated include path.
 */
#include "vkrt_replay.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE(T, x) ((T)(uintptr_t)(x))
static const VkCommandBuffer source_command = HANDLE(VkCommandBuffer, 1);
static const VkCommandBuffer target_command = HANDLE(VkCommandBuffer, 2);
static struct vkrt_replay_context source_context, target_context;
static unsigned record_calls, emit_calls, normalized, converted;
static VkPipelineLayout expected_layout;
static VkPipelineLayout emit_layout = HANDLE(VkPipelineLayout, 300);
static const void *original_array;
static bool replaying;
static const char *last_resolved;
struct VkCudaLaunchInfoNV;
static const void *expected_opaque_pointer;

struct vkrt_replay_context *vkrt_replay_lookup_context(VkCommandBuffer command) {
    return command == source_command ? &source_context : command == target_command ? &target_context : NULL;
}
static void called(VkCommandBuffer command) {
    assert(command == (replaying ? target_command : source_command));
    if (replaying)
        ++emit_calls;
    else
        ++record_calls;
}
static VkResult normalize_layout(void *user, VkPipelineLayout original, VkPipelineLayout *stable) {
    (void)user;
    assert(original == HANDLE(VkPipelineLayout, 100));
    ++normalized;
    *stable = HANDLE(VkPipelineLayout, 200);
    return VK_SUCCESS;
}
static VkResult convert_layout(void *user, const char *name, VkPipelineLayout recorded, VkPipelineLayout *forward) {
    (void)user;
    assert(recorded == HANDLE(VkPipelineLayout, 200));
    assert(!strcmp(name, "vkCmdPushConstants2KHR") || !strcmp(name, "vkCmdPushDescriptorSetKHR"));
    ++converted;
    *forward = emit_layout;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer command, const VkDependencyInfo *info) {
    called(command);
    assert(info->imageMemoryBarrierCount == 1);
    const VkImageMemoryBarrier2 *image = info->pImageMemoryBarriers;
    assert(image->image == HANDLE(VkImage, 9));
    const VkSampleLocationsInfoEXT *locations = image->pNext;
    assert(locations->sampleLocationsCount == 2);
    assert(locations->pSampleLocations[0].x == .25f && locations->pSampleLocations[1].y == .75f);
    if (replaying) assert(image != original_array);
}
static VKAPI_ATTR void VKAPI_CALL build(VkCommandBuffer command, uint32_t count,
                                        const VkAccelerationStructureBuildGeometryInfoKHR *infos,
                                        const VkAccelerationStructureBuildRangeInfoKHR *const *ranges) {
    called(command);
    assert(count == 1 && infos[0].geometryCount == 2);
    const VkAccelerationStructureGeometryKHR *triangles = infos[0].ppGeometries[0];
    const VkAccelerationStructureGeometryKHR *aabbs = infos[0].ppGeometries[1];
    assert(triangles->geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR);
    assert(triangles->geometry.triangles.vertexData.deviceAddress == 0x1000);
    assert(aabbs->geometryType == VK_GEOMETRY_TYPE_AABBS_KHR);
    assert(aabbs->geometry.aabbs.data.deviceAddress == 0x2000);
    assert(ranges[0][0].primitiveCount == 3 && ranges[0][1].primitiveCount == 7);
    if (replaying) assert(ranges[0] != original_array);
}
static VKAPI_ATTR void VKAPI_CALL build_indirect(VkCommandBuffer command, uint32_t count,
                                                 const VkAccelerationStructureBuildGeometryInfoKHR *infos,
                                                 const VkDeviceAddress *addresses, const uint32_t *strides,
                                                 const uint32_t *const *maximums) {
    called(command);
    assert(count == 1 && infos[0].geometryCount == 2);
    assert(addresses[0] == 0x4000 && strides[0] == 16);
    assert(maximums[0][0] == 5 && maximums[0][1] == 9);
}
static VKAPI_ATTR void VKAPI_CALL descriptors(VkCommandBuffer command, VkPipelineBindPoint bind, VkPipelineLayout layout,
                                              uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    called(command);
    (void)bind;
    (void)set;
    assert(layout == expected_layout && count == 2);
    assert(writes[0].pBufferInfo[0].offset == 123);
    const VkWriteDescriptorSetAccelerationStructureKHR *acceleration = writes[1].pNext;
    assert(acceleration->accelerationStructureCount == 2);
    assert(acceleration->pAccelerationStructures[1] == HANDLE(VkAccelerationStructureKHR, 89));
    if (replaying) {
        assert(!writes[0].pImageInfo && !writes[0].pTexelBufferView);
        assert(!writes[1].pImageInfo && !writes[1].pBufferInfo && !writes[1].pTexelBufferView);
    }
}
static VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer command, const VkPushConstantsInfo *info) {
    called(command);
    assert(info->layout == expected_layout && info->size == 4);
    assert(!memcmp(info->pValues, "ABCD", 4));
}
static VKAPI_ATTR void VKAPI_CALL blend(VkCommandBuffer command, const float values[4]) {
    called(command);
    assert(values[0] == 1 && values[1] == 2 && values[2] == 3 && values[3] == 4);
    if (replaying) assert(values != original_array);
}
static VKAPI_ATTR void VKAPI_CALL sample_mask(VkCommandBuffer command, VkSampleCountFlagBits samples, const VkSampleMask *mask) {
    called(command);
    assert(samples == VK_SAMPLE_COUNT_64_BIT && mask[0] == 0x1234 && mask[1] == 0x5678);
}
static VKAPI_ATTR void VKAPI_CALL label(VkCommandBuffer command, const VkDebugUtilsLabelEXT *info) {
    called(command);
    assert(!strcmp(info->pLabelName, "owned label"));
}
static VKAPI_ATTR void VKAPI_CALL execute(VkCommandBuffer command, uint32_t count, const VkCommandBuffer *children) {
    called(command);
    assert(count == 2);
    assert(children[0] == HANDLE(VkCommandBuffer, replaying ? 110 : 10));
    assert(children[1] == HANDLE(VkCommandBuffer, replaying ? 111 : 11));
}
static VKAPI_ATTR void VKAPI_CALL multi(VkCommandBuffer command, uint32_t count, const VkMultiDrawInfoEXT *draws,
                                        uint32_t instances, uint32_t first_instance, uint32_t stride) {
    called(command);
    (void)instances;
    (void)first_instance;
    assert(count == 2 && stride == 64);
    const VkMultiDrawInfoEXT *second = (const void *)((const unsigned char *)draws + stride);
    assert(draws->firstVertex == 12 && second->vertexCount == 34);
    if (replaying) assert(draws != original_array);
}
static VKAPI_ATTR void VKAPI_CALL cluster(VkCommandBuffer command, const VkClusterAccelerationStructureCommandsInfoNV *info) {
    called(command);
    assert(info->input.opType == VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV);
    assert(info->input.opInput.pTriangleClusters->maxClusterVertexCount == 1234);
    if (replaying) assert(info->input.opInput.pTriangleClusters != original_array);
}
static VKAPI_ATTR VkResult VKAPI_CALL gpa(VkCommandBuffer command, VkGpaSessionAMD session, const VkGpaSampleBeginInfoAMD *info,
                                          uint32_t *sample) {
    called(command);
    (void)session;
    (void)info;
    *sample = 77;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL opaque(VkCommandBuffer command, VkDescriptorUpdateTemplate update, VkPipelineLayout layout,
                                         uint32_t set, const void *data) {
    called(command);
    (void)update;
    (void)layout;
    (void)set;
    (void)data;
}
static VKAPI_ATTR void VKAPI_CALL ignore_barrier(VkCommandBuffer command, const VkDependencyInfo *info) {
    called(command);
    (void)info;
}
static VKAPI_ATTR void VKAPI_CALL update(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                         const void *data) {
    called(command);
    (void)buffer;
    (void)offset;
    (void)size;
    (void)data;
}
static VKAPI_ATTR void VKAPI_CALL unavailable_cuda(VkCommandBuffer command, const struct VkCudaLaunchInfoNV *info) {
    called(command);
    assert((const void *)info == expected_opaque_pointer);
}
static VKAPI_ATTR void VKAPI_CALL unavailable_graph(VkCommandBuffer command, VkDeviceAddress scratch, VkDeviceSize scratch_size,
                                                    VkDeviceAddress count_info) {
    called(command);
    assert(scratch == UINT64_C(0x123456789abcdef0) && scratch_size == UINT64_C(0x100000002) &&
           count_info == UINT64_C(0x777788889999aaaa));
}
static PFN_vkVoidFunction resolve(void *user, const char *name) {
    (void)user;
    last_resolved = name;
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) return (PFN_vkVoidFunction)barrier;
    if (!strcmp(name, "vkCmdPipelineBarrier2")) return (PFN_vkVoidFunction)ignore_barrier;
    if (!strcmp(name, "vkCmdBuildAccelerationStructuresKHR")) return (PFN_vkVoidFunction)build;
    if (!strcmp(name, "vkCmdBuildAccelerationStructuresIndirectKHR")) return (PFN_vkVoidFunction)build_indirect;
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR")) return (PFN_vkVoidFunction)descriptors;
    if (!strcmp(name, "vkCmdPushConstants2KHR")) return (PFN_vkVoidFunction)push;
    if (!strcmp(name, "vkCmdSetBlendConstants")) return (PFN_vkVoidFunction)blend;
    if (!strcmp(name, "vkCmdSetSampleMaskEXT")) return (PFN_vkVoidFunction)sample_mask;
    if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)label;
    if (!strcmp(name, "vkCmdExecuteCommands")) return (PFN_vkVoidFunction)execute;
    if (!strcmp(name, "vkCmdDrawMultiEXT")) return (PFN_vkVoidFunction)multi;
    if (!strcmp(name, "vkCmdBuildClusterAccelerationStructureIndirectNV")) return (PFN_vkVoidFunction)cluster;
    if (!strcmp(name, "vkCmdBeginGpaSampleAMD")) return (PFN_vkVoidFunction)gpa;
    if (!strcmp(name, "vkCmdPushDescriptorSetWithTemplateKHR")) return (PFN_vkVoidFunction)opaque;
    if (!strcmp(name, "vkCmdUpdateBuffer")) return (PFN_vkVoidFunction)update;
    if (!strcmp(name, "vkCmdCudaLaunchKernelNV")) return (PFN_vkVoidFunction)unavailable_cuda;
    if (!strcmp(name, "vkCmdDispatchGraphIndirectCountAMDX")) return (PFN_vkVoidFunction)unavailable_graph;
    return NULL;
}
static VkResult remap(void *user, VkCommandBuffer child, VkCommandBuffer *target) {
    (void)user;
    *target = HANDLE(VkCommandBuffer, (uintptr_t)child + 100);
    return VK_SUCCESS;
}
static VkResult remap_fail(void *user, VkCommandBuffer child, VkCommandBuffer *target) {
    (void)user;
    (void)child;
    (void)target;
    return VK_ERROR_UNKNOWN;
}
static void reset(void) {
    vkrt_replay_journal_reset(source_context.journal);
    replaying = false;
    expected_layout = HANDLE(VkPipelineLayout, 100);
    record_calls = emit_calls = normalized = converted = 0;
    target_context.emit_pipeline_layout = NULL;
}
static void replay(vkrt_replay_remap_child child) {
    replaying = true;
    assert(vkrt_replay_emit(source_context.journal, target_command, resolve, NULL, child) == VK_SUCCESS);
    replaying = false;
}
static const struct vkrt_replay_entry *entry_at(size_t index) {
    const struct vkrt_replay_entry *entry = vkrt_replay_first(source_context.journal);
    while (entry && index--) entry = vkrt_replay_next(entry);
    assert(entry);
    return entry;
}
static struct vkrt_replay_metadata metadata_at(size_t index) {
    struct vkrt_replay_metadata metadata;
    memset(&metadata, 0xff, sizeof(metadata));
    assert(vkrt_replay_entry_metadata(entry_at(index), &metadata) == VK_SUCCESS);
    return metadata;
}
static void test_begin(void) {
    VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    VkCommandBufferInheritanceRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                                                         .colorAttachmentCount = 2,
                                                         .pColorAttachmentFormats = formats};
    VkCommandBufferInheritanceInfo inheritance = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &rendering, .subpass = 3};
    VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
        .pInheritanceInfo = &inheritance};
    assert(vkrt_replay_journal_begin(source_context.journal, &info) == VK_SUCCESS);
    formats[0] = VK_FORMAT_UNDEFINED;
    inheritance.subpass = 99;
    const VkCommandBufferBeginInfo *owned = vkrt_replay_journal_begin_info(source_context.journal);
    assert(owned != &info && owned->pInheritanceInfo->subpass == 3);
    const VkCommandBufferInheritanceRenderingInfo *r = owned->pInheritanceInfo->pNext;
    assert(r != &rendering && r->pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM);
}
static void test_inherited_viewports(void) {
    VkViewport depths[] = {
        {.minDepth = .1f, .maxDepth = .3f}, {.minDepth = .2f, .maxDepth = .6f}, {.minDepth = .4f, .maxDepth = .9f}};
    VkCommandBufferInheritanceViewportScissorInfoNV viewports = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_VIEWPORT_SCISSOR_INFO_NV,
        .viewportScissor2D = VK_TRUE,
        .viewportDepthCount = 3,
        .pViewportDepths = depths};
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &viewports};
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                     .pInheritanceInfo = &inheritance};
    assert(vkrt_replay_journal_begin(source_context.journal, &info) == VK_SUCCESS);
    memset(depths, 0, sizeof(depths));
    const VkCommandBufferInheritanceViewportScissorInfoNV *owned =
        vkrt_replay_journal_begin_info(source_context.journal)->pInheritanceInfo->pNext;
    assert(owned->pViewportDepths != depths && owned->viewportDepthCount == 3);
    assert(owned->pViewportDepths[0].minDepth == .1f && owned->pViewportDepths[1].maxDepth == .6f &&
           owned->pViewportDepths[2].maxDepth == .9f);
    viewports.viewportScissor2D = VK_FALSE;
    viewports.viewportDepthCount = UINT32_MAX;
    viewports.pViewportDepths = (void *)(uintptr_t)1;
    assert(vkrt_replay_journal_begin(source_context.journal, &info) == VK_SUCCESS);
    owned = vkrt_replay_journal_begin_info(source_context.journal)->pInheritanceInfo->pNext;
    assert(!owned->viewportScissor2D && !owned->pViewportDepths);
}
static void test_inactive_rendering_inheritance(void) {
    VkCommandBufferInheritanceConditionalRenderingInfoEXT tail = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT};
    VkRenderingAttachmentLocationInfo locations = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO,
                                                   .pNext = &tail,
                                                   .colorAttachmentCount = UINT32_MAX,
                                                   .pColorAttachmentLocations = (void *)(uintptr_t)1};
    VkAttachmentSampleCountInfoAMD samples = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD,
                                              .pNext = &locations,
                                              .colorAttachmentCount = UINT32_MAX,
                                              .pColorAttachmentSamples = (void *)(uintptr_t)1};
    VkCommandBufferInheritanceRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                                                         .pNext = &samples,
                                                         .colorAttachmentCount = UINT32_MAX,
                                                         .pColorAttachmentFormats = (void *)(uintptr_t)1};
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &rendering};
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pInheritanceInfo = &inheritance};
    /* Both conditions independently make the three dynamic-rendering payloads
     * inactive. Keep the unrelated tail node, without inspecting the garbage. */
    for (unsigned variant = 0; variant != 2; ++variant) {
        info.flags = variant ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : 0;
        inheritance.renderPass = variant ? HANDLE(VkRenderPass, 5) : VK_NULL_HANDLE;
        assert(vkrt_replay_journal_begin(source_context.journal, &info) == VK_SUCCESS);
        const VkBaseInStructure *owned = vkrt_replay_journal_begin_info(source_context.journal)->pInheritanceInfo->pNext;
        assert(owned && owned->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT &&
               !owned->pNext);
    }
    /* Active dynamic-rendering inheritance owns every element of all arrays. */
    VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT};
    VkSampleCountFlagBits counts[] = {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT};
    uint32_t indices[] = {1, 0};
    info.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    inheritance.renderPass = VK_NULL_HANDLE;
    rendering.colorAttachmentCount = samples.colorAttachmentCount = locations.colorAttachmentCount = 2;
    rendering.pColorAttachmentFormats = formats;
    samples.pColorAttachmentSamples = counts;
    locations.pColorAttachmentLocations = indices;
    assert(vkrt_replay_journal_begin(source_context.journal, &info) == VK_SUCCESS);
    formats[1] = VK_FORMAT_UNDEFINED;
    counts[1] = VK_SAMPLE_COUNT_1_BIT;
    indices[1] = 99;
    const VkCommandBufferInheritanceRenderingInfo *owned_rendering =
        vkrt_replay_journal_begin_info(source_context.journal)->pInheritanceInfo->pNext;
    const VkAttachmentSampleCountInfoAMD *owned_samples = owned_rendering->pNext;
    const VkRenderingAttachmentLocationInfo *owned_locations = owned_samples->pNext;
    assert(owned_rendering->pColorAttachmentFormats[1] == VK_FORMAT_R32_SFLOAT &&
           owned_samples->pColorAttachmentSamples[1] == VK_SAMPLE_COUNT_4_BIT &&
           owned_locations->pColorAttachmentLocations[1] == 0);
    /* Unreviewed inactive schemas fail before touching their optional pointers. */
    VkRenderingInputAttachmentIndexInfo uncertain = {.sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
                                                     .pDepthInputAttachmentIndex = (void *)(uintptr_t)1};
    inheritance.pNext = &uncertain;
    info.flags = 0;
    assert(vkrt_replay_journal_begin(source_context.journal, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
}
static void test_barrier(void) {
    reset();
    VkSampleLocationEXT points[] = {{.25f, .5f}, {.5f, .75f}};
    VkSampleLocationsInfoEXT locations = {
        .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, .sampleLocationsCount = 2, .pSampleLocations = points};
    VkImageMemoryBarrier2 image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = &locations, .image = HANDLE(VkImage, 9)};
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image};
    original_array = &image;
    ((PFN_vkCmdPipelineBarrier2KHR)vkrt_replay_proc("vkCmdPipelineBarrier2KHR"))(source_command, &info);
    points[0].x = 0;
    image.image = VK_NULL_HANDLE;
    assert(vkrt_replay_journal_count(source_context.journal) == 1);
    assert(!strcmp(vkrt_replay_entry_name(vkrt_replay_first(source_context.journal)), "vkCmdPipelineBarrier2KHR"));
    struct vkrt_replay_metadata metadata = metadata_at(0);
    assert(metadata.kind == VKRT_REPLAY_BARRIER && metadata.data.sync.synchronization2 &&
           metadata.data.sync.dependency_info_count == 1 && metadata.data.sync.dependency_infos != &info &&
           metadata.data.sync.dependency_infos[0].pImageMemoryBarriers->image == HANDLE(VkImage, 9));
    replay(NULL);
    assert(!strcmp(last_resolved, "vkCmdPipelineBarrier2KHR"));
    assert(record_calls == 1 && emit_calls == 1);
}
static void test_build(void) {
    reset();
    VkAccelerationStructureGeometryKHR geometries[2] = {
        {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
         .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
         .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                .vertexData.deviceAddress = 0x1000}},
        {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
         .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
         .geometry.aabbs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                            .data.deviceAddress = 0x2000}}};
    const VkAccelerationStructureGeometryKHR *pointers[] = {&geometries[0], &geometries[1]};
    VkAccelerationStructureBuildGeometryInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .geometryCount = 2, .ppGeometries = pointers};
    VkAccelerationStructureBuildRangeInfoKHR range[] = {{.primitiveCount = 3}, {.primitiveCount = 7}};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {range};
    original_array = range;
    ((PFN_vkCmdBuildAccelerationStructuresKHR)vkrt_replay_proc("vkCmdBuildAccelerationStructuresKHR"))(source_command, 1, &info,
                                                                                                       ranges);
    VkDeviceAddress addresses[] = {0x4000};
    uint32_t strides[] = {16}, maximums[] = {5, 9};
    const uint32_t *maximum_rows[] = {maximums};
    ((PFN_vkCmdBuildAccelerationStructuresIndirectKHR)vkrt_replay_proc("vkCmdBuildAccelerationStructuresIndirectKHR"))(
        source_command, 1, &info, addresses, strides, maximum_rows);
    memset(geometries, 0, sizeof(geometries));
    memset(range, 0, sizeof(range));
    memset(addresses, 0, sizeof(addresses));
    memset(strides, 0, sizeof(strides));
    memset(maximums, 0, sizeof(maximums));
    struct vkrt_replay_metadata direct = metadata_at(0), indirect = metadata_at(1);
    assert(direct.kind == VKRT_REPLAY_AS_BUILD && direct.flags == VKRT_REPLAY_LOWERED);
    assert(direct.logical_stages == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR &&
           direct.physical_stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    assert(direct.data.as_build.count == 1 && direct.data.as_build.infos != &info &&
           direct.data.as_build.ranges[0][0].primitiveCount == 3);
    assert(indirect.kind == VKRT_REPLAY_AS_BUILD && (indirect.flags & VKRT_REPLAY_INDIRECT));
    assert(indirect.logical_stages == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR &&
           !(indirect.logical_stages & VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT));
    assert(indirect.data.as_build.indirect_addresses[0] == 0x4000 && indirect.data.as_build.max_primitive_counts[0][1] == 9);
    replay(NULL);
    assert(emit_calls == 2);
}
static void test_descriptors(void) {
    reset();
    VkDescriptorBufferInfo buffer = {.buffer = HANDLE(VkBuffer, 20), .offset = 123, .range = 456};
    VkAccelerationStructureKHR handles[] = {HANDLE(VkAccelerationStructureKHR, 88), HANDLE(VkAccelerationStructureKHR, 89)};
    VkWriteDescriptorSetAccelerationStructureKHR acceleration = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 2,
        .pAccelerationStructures = handles};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      .pBufferInfo = &buffer,
                                      .pImageInfo = (void *)(uintptr_t)1,
                                      .pTexelBufferView = (void *)(uintptr_t)1},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .pNext = &acceleration,
                                      .descriptorCount = 2,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                      .pBufferInfo = (void *)(uintptr_t)1,
                                      .pImageInfo = (void *)(uintptr_t)1,
                                      .pTexelBufferView = (void *)(uintptr_t)1}};
    ((PFN_vkCmdPushDescriptorSetKHR)vkrt_replay_proc("vkCmdPushDescriptorSetKHR"))(source_command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                                                   HANDLE(VkPipelineLayout, 100), 0, 2, writes);
    buffer.offset = 0;
    handles[1] = VK_NULL_HANDLE;
    target_context.emit_pipeline_layout = convert_layout;
    expected_layout = emit_layout;
    replay(NULL);
    assert(normalized == 1 && converted == 1);
}
static void test_layout_immutable(void) {
    reset();
    char bytes[4] = {'A', 'B', 'C', 'D'};
    VkPushConstantsInfo info = {
        .sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO, .layout = HANDLE(VkPipelineLayout, 100), .size = 4, .pValues = bytes};
    ((PFN_vkCmdPushConstants2KHR)vkrt_replay_proc("vkCmdPushConstants2KHR"))(source_command, &info);
    memset(bytes, 0, sizeof(bytes));
    target_context.emit_pipeline_layout = convert_layout;
    expected_layout = emit_layout;
    replay(NULL);
    emit_layout = HANDLE(VkPipelineLayout, 301);
    expected_layout = emit_layout;
    replay(NULL);
    assert(normalized == 1 && converted == 2 && emit_calls == 2);
}
static void test_fixed_array(void) {
    reset();
    float constants[] = {1, 2, 3, 4};
    original_array = constants;
    ((PFN_vkCmdSetBlendConstants)vkrt_replay_proc("vkCmdSetBlendConstants"))(source_command, constants);
    VkSampleMask mask[] = {0x1234, 0x5678};
    ((PFN_vkCmdSetSampleMaskEXT)vkrt_replay_proc("vkCmdSetSampleMaskEXT"))(source_command, VK_SAMPLE_COUNT_64_BIT, mask);
    char text[] = "owned label";
    VkDebugUtilsLabelEXT label_info = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = text};
    ((PFN_vkCmdBeginDebugUtilsLabelEXT)vkrt_replay_proc("vkCmdBeginDebugUtilsLabelEXT"))(source_command, &label_info);
    memset(constants, 0, sizeof(constants));
    memset(mask, 0, sizeof(mask));
    memset(text, 0, sizeof(text));
    replay(NULL);
}
static void test_children(void) {
    reset();
    VkCommandBuffer children[] = {HANDLE(VkCommandBuffer, 10), HANDLE(VkCommandBuffer, 11)};
    ((PFN_vkCmdExecuteCommands)vkrt_replay_proc("vkCmdExecuteCommands"))(source_command, 2, children);
    memset(children, 0, sizeof(children));
    struct vkrt_replay_metadata metadata = metadata_at(0);
    assert(metadata.kind == VKRT_REPLAY_EXECUTE_SECONDARIES && !metadata.logical_stages && !metadata.physical_stages);
    assert(metadata.data.children.count == 2 && metadata.data.children.commands != children &&
           metadata.data.children.commands[1] == HANDLE(VkCommandBuffer, 11));
    replay(remap);
    replay(remap);
    replaying = true;
    assert(vkrt_replay_emit(source_context.journal, target_command, resolve, NULL, remap_fail) == VK_ERROR_UNKNOWN);
    replaying = false;
    assert(emit_calls == 2);
}
static void test_stride_and_selector(void) {
    reset();
    union {
        max_align_t alignment;
        unsigned char bytes[128];
    } rows;
    memset(&rows, 0xa5, sizeof(rows));
    VkMultiDrawInfoEXT *first = (void *)rows.bytes;
    VkMultiDrawInfoEXT *second = (void *)(rows.bytes + 64);
    first->firstVertex = 12;
    second->vertexCount = 34;
    original_array = first;
    ((PFN_vkCmdDrawMultiEXT)vkrt_replay_proc("vkCmdDrawMultiEXT"))(source_command, 2, first, 1, 0, 64);
    memset(&rows, 0, sizeof(rows));
    replay(NULL);
    reset();
    VkClusterAccelerationStructureTriangleClusterInputNV triangles = {
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV, .maxClusterVertexCount = 1234};
    VkClusterAccelerationStructureCommandsInfoNV info = {
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,
        .input = {.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,
                  .opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV,
                  .opInput.pTriangleClusters = &triangles}};
    original_array = &triangles;
    ((PFN_vkCmdBuildClusterAccelerationStructureIndirectNV)vkrt_replay_proc("vkCmdBuildClusterAccelerationStructureIndirectNV"))(
        source_command, &info);
    triangles.maxClusterVertexCount = 0;
    struct vkrt_replay_metadata metadata;
    memset(&metadata, 0xff, sizeof(metadata));
    assert(vkrt_replay_entry_metadata(entry_at(0), &metadata) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(metadata.kind == VKRT_REPLAY_UNKNOWN && !metadata.logical_stages && !metadata.physical_stages && !metadata.flags);
    assert(vkrt_replay_journal_status(source_context.journal) == VK_SUCCESS);
    replay(NULL);
}

static void test_query_metadata(void) {
    reset();
    const VkQueryPool pool = HANDLE(VkQueryPool, 91);
    VkAccelerationStructureKHR structures[] = {HANDLE(VkAccelerationStructureKHR, 30), HANDLE(VkAccelerationStructureKHR, 31),
                                               HANDLE(VkAccelerationStructureKHR, 32)};
    ((PFN_vkCmdResetQueryPool)vkrt_replay_proc("vkCmdResetQueryPool"))(source_command, pool, 9, 3);
    ((PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkrt_replay_proc("vkCmdWriteAccelerationStructuresPropertiesKHR"))(
        source_command, 3, structures, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR, pool, 9);
    ((PFN_vkCmdCopyQueryPoolResults)vkrt_replay_proc("vkCmdCopyQueryPoolResults"))(
        source_command, pool, 9, 3, HANDLE(VkBuffer, 92), UINT64_C(0x100000004), 32,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    ((PFN_vkCmdWriteTimestamp2KHR)vkrt_replay_proc("vkCmdWriteTimestamp2KHR"))(source_command, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                               HANDLE(VkQueryPool, 93), 15);
    ((PFN_vkCmdBeginQueryIndexedEXT)vkrt_replay_proc("vkCmdBeginQueryIndexedEXT"))(source_command, HANDLE(VkQueryPool, 94), 5,
                                                                                   VK_QUERY_CONTROL_PRECISE_BIT, 2);
    ((PFN_vkCmdEndQueryIndexedEXT)vkrt_replay_proc("vkCmdEndQueryIndexedEXT"))(source_command, HANDLE(VkQueryPool, 94), 5, 2);
    memset(structures, 0, sizeof(structures));
    struct vkrt_replay_metadata reset_query = metadata_at(0), property = metadata_at(1), copy = metadata_at(2);
    assert(reset_query.kind == VKRT_REPLAY_QUERY_RESET && reset_query.data.query.pool == pool &&
           reset_query.data.query.first == 9 && reset_query.data.query.count == 3);
    assert(!reset_query.logical_stages && !reset_query.physical_stages && reset_query.flags == VKRT_REPLAY_QUERY_POOL_CONTEXT);
    assert(property.kind == VKRT_REPLAY_AS_PROPERTY && property.data.query.pool == pool && property.data.query.first == 9 &&
           property.data.query.count == 3 &&
           property.data.query.type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR &&
           property.data.query.acceleration_structures != structures &&
           property.data.query.acceleration_structures[2] == HANDLE(VkAccelerationStructureKHR, 32));
    assert(property.flags == (VKRT_REPLAY_LOWERED | VKRT_REPLAY_AS_COPY_CONTEXT) && !property.logical_stages &&
           property.physical_stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    assert(copy.kind == VKRT_REPLAY_QUERY_COPY && copy.logical_stages == VK_PIPELINE_STAGE_2_COPY_BIT &&
           copy.physical_stages == VK_PIPELINE_STAGE_2_COPY_BIT && copy.flags == VKRT_REPLAY_QUERY_POOL_CONTEXT);
    assert(copy.data.query.destination == HANDLE(VkBuffer, 92) && copy.data.query.offset == UINT64_C(0x100000004) &&
           copy.data.query.stride == 32 &&
           copy.data.query.result_flags ==
               (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT));
    struct vkrt_replay_metadata timestamp = metadata_at(3), begin = metadata_at(4), end = metadata_at(5);
    assert(timestamp.kind == VKRT_REPLAY_QUERY_TIMESTAMP && timestamp.data.query.stage_mask == VK_PIPELINE_STAGE_2_COPY_BIT &&
           timestamp.data.query.first == 15 && timestamp.data.query.count == 1 && !timestamp.logical_stages &&
           timestamp.flags == (VKRT_REPLAY_QUERY_POOL_CONTEXT | VKRT_REPLAY_RAW_SCOPES));
    assert(begin.kind == VKRT_REPLAY_QUERY_BEGIN && begin.data.query.index == 2 && begin.data.query.first == 5 &&
           begin.data.query.control_flags == VK_QUERY_CONTROL_PRECISE_BIT && !begin.logical_stages);
    assert(end.kind == VKRT_REPLAY_QUERY_END && end.data.query.index == 2 && end.data.query.first == 5 && !end.logical_stages);
}

static void test_virtual_work_metadata(void) {
    reset();
    VkCopyAccelerationStructureInfoKHR copy = {.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                                               .src = HANDLE(VkAccelerationStructureKHR, 31),
                                               .dst = HANDLE(VkAccelerationStructureKHR, 32),
                                               .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR};
    VkCopyAccelerationStructureToMemoryInfoKHR serialize = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR,
        .src = copy.src,
        .dst.deviceAddress = UINT64_C(0x100000008),
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR};
    VkCopyMemoryToAccelerationStructureInfoKHR deserialize = {
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR,
        .src.deviceAddress = UINT64_C(0x200000008),
        .dst = copy.dst,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR};
    ((PFN_vkCmdCopyAccelerationStructureKHR)vkrt_replay_proc("vkCmdCopyAccelerationStructureKHR"))(source_command, &copy);
    ((PFN_vkCmdCopyAccelerationStructureToMemoryKHR)vkrt_replay_proc("vkCmdCopyAccelerationStructureToMemoryKHR"))(source_command,
                                                                                                                   &serialize);
    ((PFN_vkCmdCopyMemoryToAccelerationStructureKHR)vkrt_replay_proc("vkCmdCopyMemoryToAccelerationStructureKHR"))(source_command,
                                                                                                                   &deserialize);
    VkStridedDeviceAddressRegionKHR regions[] = {{0x1000, 32, 32}, {0x2000, 32, 64}, {0x3000, 32, 64}, {0x4000, 32, 64}};
    ((PFN_vkCmdTraceRaysKHR)vkrt_replay_proc("vkCmdTraceRaysKHR"))(source_command, &regions[0], &regions[1], &regions[2],
                                                                   &regions[3], 4, 3, 2);
    ((PFN_vkCmdTraceRaysIndirectKHR)vkrt_replay_proc("vkCmdTraceRaysIndirectKHR"))(source_command, &regions[0], &regions[1],
                                                                                   &regions[2], &regions[3], UINT64_C(0x300000000));
    ((PFN_vkCmdTraceRaysIndirect2KHR)vkrt_replay_proc("vkCmdTraceRaysIndirect2KHR"))(source_command, UINT64_C(0x400000000));
    memset(&copy, 0, sizeof(copy));
    memset(&serialize, 0, sizeof(serialize));
    memset(&deserialize, 0, sizeof(deserialize));
    memset(regions, 0, sizeof(regions));
    struct vkrt_replay_metadata metadata = metadata_at(0);
    assert(metadata.kind == VKRT_REPLAY_AS_COPY && metadata.flags == (VKRT_REPLAY_LOWERED | VKRT_REPLAY_AS_COPY_CONTEXT) &&
           !metadata.logical_stages && metadata.physical_stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    assert(metadata.data.as_copy.kind == VKRT_REPLAY_AS_CLONE_COMPACT && metadata.data.as_copy.copy != &copy &&
           metadata.data.as_copy.copy->src == HANDLE(VkAccelerationStructureKHR, 31));
    metadata = metadata_at(1);
    assert(metadata.data.as_copy.kind == VKRT_REPLAY_AS_SERIALIZE && metadata.data.as_copy.serialize != &serialize &&
           metadata.data.as_copy.serialize->dst.deviceAddress == UINT64_C(0x100000008));
    metadata = metadata_at(2);
    assert(metadata.data.as_copy.kind == VKRT_REPLAY_AS_DESERIALIZE && metadata.data.as_copy.deserialize != &deserialize &&
           metadata.data.as_copy.deserialize->src.deviceAddress == UINT64_C(0x200000008));
    metadata = metadata_at(3);
    assert(metadata.kind == VKRT_REPLAY_TRACE && metadata.flags == VKRT_REPLAY_LOWERED &&
           metadata.logical_stages == VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR &&
           metadata.physical_stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && metadata.data.trace.width == 4 &&
           metadata.data.trace.height == 3 && metadata.data.trace.depth == 2 && metadata.data.trace.raygen != regions &&
           metadata.data.trace.hit->deviceAddress == 0x3000);
    metadata = metadata_at(4);
    assert(metadata.flags == (VKRT_REPLAY_LOWERED | VKRT_REPLAY_INDIRECT) &&
           metadata.logical_stages == (VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT) &&
           metadata.data.trace.indirect_address == UINT64_C(0x300000000));
    metadata = metadata_at(5);
    assert(metadata.kind == VKRT_REPLAY_TRACE && metadata.data.trace.indirect_address == UINT64_C(0x400000000) &&
           !metadata.data.trace.raygen);

    reset();
    const uint32_t bytes = 12;
    ((PFN_vkCmdUpdateBuffer)vkrt_replay_proc("vkCmdUpdateBuffer"))(source_command, HANDLE(VkBuffer, 35), 0, 4, &bytes);
    metadata = metadata_at(0);
    /* UpdateBuffer is in the pinned Clear Commands chapter, not Copy Commands. */
    assert(metadata.logical_stages == VK_PIPELINE_STAGE_2_CLEAR_BIT && metadata.physical_stages == VK_PIPELINE_STAGE_2_CLEAR_BIT);
    VkConditionalRenderingBeginInfoEXT conditional = {.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
                                                      .buffer = HANDLE(VkBuffer, 36),
                                                      .offset = 20,
                                                      .flags = VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT};
    VkBuffer counters[] = {HANDLE(VkBuffer, 37), HANDLE(VkBuffer, 38)};
    VkDeviceSize offsets[] = {16, 32};
    ((PFN_vkCmdBeginConditionalRenderingEXT)vkrt_replay_proc("vkCmdBeginConditionalRenderingEXT"))(source_command, &conditional);
    ((PFN_vkCmdEndConditionalRenderingEXT)vkrt_replay_proc("vkCmdEndConditionalRenderingEXT"))(source_command);
    ((PFN_vkCmdBeginTransformFeedbackEXT)vkrt_replay_proc("vkCmdBeginTransformFeedbackEXT"))(source_command, 1, 2, counters,
                                                                                             offsets);
    ((PFN_vkCmdEndTransformFeedbackEXT)vkrt_replay_proc("vkCmdEndTransformFeedbackEXT"))(source_command, 1, 2, counters, offsets);
    memset(&conditional, 0, sizeof(conditional));
    memset(counters, 0, sizeof(counters));
    memset(offsets, 0, sizeof(offsets));
    metadata = metadata_at(1);
    assert(metadata.kind == VKRT_REPLAY_CONDITIONAL_BEGIN && metadata.data.conditional.begin != &conditional &&
           metadata.data.conditional.begin->offset == 20 &&
           metadata.data.conditional.begin->flags == VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT);
    assert(metadata_at(2).kind == VKRT_REPLAY_CONDITIONAL_END);
    metadata = metadata_at(3);
    assert(metadata.kind == VKRT_REPLAY_TRANSFORM_FEEDBACK_BEGIN && metadata.data.transform_feedback.first == 1 &&
           metadata.data.transform_feedback.count == 2 && metadata.data.transform_feedback.counter_buffers != counters &&
           metadata.data.transform_feedback.counter_buffers[1] == HANDLE(VkBuffer, 38) &&
           metadata.data.transform_feedback.counter_offsets[1] == 32);
    assert(metadata_at(4).kind == VKRT_REPLAY_TRANSFORM_FEEDBACK_END);
}

static void test_sync_metadata(void) {
    reset();
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
    VkBufferMemoryBarrier buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                    .srcQueueFamilyIndex = 3,
                                    .dstQueueFamilyIndex = 5,
                                    .buffer = HANDLE(VkBuffer, 21),
                                    .offset = 24,
                                    .size = 48};
    VkImageMemoryBarrier image = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .image = HANDLE(VkImage, 22),
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 0, 4}};
    ((PFN_vkCmdPipelineBarrier)vkrt_replay_proc("vkCmdPipelineBarrier"))(
        source_command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &memory, 1, &buffer, 1, &image);
    memset(&memory, 0, sizeof(memory));
    memset(&buffer, 0, sizeof(buffer));
    memset(&image, 0, sizeof(image));
    struct vkrt_replay_metadata metadata = metadata_at(0);
    assert(metadata.kind == VKRT_REPLAY_BARRIER && metadata.flags == VKRT_REPLAY_RAW_SCOPES && !metadata.logical_stages &&
           !metadata.physical_stages && !metadata.data.sync.synchronization2);
    assert(metadata.data.sync.src_stage_mask ==
               (VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT) &&
           metadata.data.sync.dst_stage_mask == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT &&
           metadata.data.sync.dependency_flags == VK_DEPENDENCY_BY_REGION_BIT);
    assert(metadata.data.sync.memory_barriers != &memory &&
           metadata.data.sync.memory_barriers[0].srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT);
    assert(metadata.data.sync.buffer_barriers != &buffer && metadata.data.sync.buffer_barriers[0].offset == 24 &&
           metadata.data.sync.buffer_barriers[0].size == 48 && metadata.data.sync.buffer_barriers[0].srcQueueFamilyIndex == 3 &&
           metadata.data.sync.buffer_barriers[0].dstQueueFamilyIndex == 5);
    assert(metadata.data.sync.image_barriers != &image &&
           metadata.data.sync.image_barriers[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
           metadata.data.sync.image_barriers[0].subresourceRange.levelCount == 3);

    VkMemoryBarrier2 barriers[] = {{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR},
                                   {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                    .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                    .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo dependencies[] = {
        {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barriers[0]},
        {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barriers[1]}};
    VkEvent events[] = {HANDLE(VkEvent, 23), HANDLE(VkEvent, 24)};
    ((PFN_vkCmdWaitEvents2KHR)vkrt_replay_proc("vkCmdWaitEvents2KHR"))(source_command, 2, events, dependencies);
    ((PFN_vkCmdSetEvent2KHR)vkrt_replay_proc("vkCmdSetEvent2KHR"))(source_command, events[0], &dependencies[0]);
    ((PFN_vkCmdResetEvent2KHR)vkrt_replay_proc("vkCmdResetEvent2KHR"))(source_command, events[1], VK_PIPELINE_STAGE_2_CLEAR_BIT);
    memset(barriers, 0, sizeof(barriers));
    memset(dependencies, 0, sizeof(dependencies));
    memset(events, 0, sizeof(events));
    metadata = metadata_at(1);
    assert(metadata.kind == VKRT_REPLAY_EVENT_WAIT && metadata.data.sync.synchronization2 && metadata.data.sync.event_count == 2 &&
           metadata.data.sync.dependency_info_count == 2 && metadata.data.sync.events != events &&
           metadata.data.sync.events[1] == HANDLE(VkEvent, 24));
    const VkDependencyInfo *owned = metadata.data.sync.dependency_infos;
    assert(owned != dependencies && owned[0].pMemoryBarriers != &barriers[0] &&
           owned[0].pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_COPY_BIT &&
           owned[1].pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR &&
           owned[1].pMemoryBarriers[0].dstStageMask == VK_PIPELINE_STAGE_2_HOST_BIT);
    metadata = metadata_at(2);
    assert(metadata.kind == VKRT_REPLAY_EVENT_SET && metadata.data.sync.event == HANDLE(VkEvent, 23) &&
           metadata.data.sync.dependency_info_count == 1 &&
           metadata.data.sync.dependency_infos[0].pMemoryBarriers[0].dstAccessMask ==
               VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    metadata = metadata_at(3);
    assert(metadata.kind == VKRT_REPLAY_EVENT_RESET && metadata.data.sync.event == HANDLE(VkEvent, 24) &&
           metadata.data.sync.synchronization2 && metadata.data.sync.src_stage_mask == VK_PIPELINE_STAGE_2_CLEAR_BIT);
    VkEvent legacy_events[] = {HANDLE(VkEvent, 25), HANDLE(VkEvent, 26)};
    ((PFN_vkCmdWaitEvents)vkrt_replay_proc("vkCmdWaitEvents"))(source_command, 2, legacy_events, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, NULL, 0, NULL, 0, NULL);
    ((PFN_vkCmdSetEvent)vkrt_replay_proc("vkCmdSetEvent"))(source_command, legacy_events[0], VK_PIPELINE_STAGE_TRANSFER_BIT);
    ((PFN_vkCmdResetEvent)vkrt_replay_proc("vkCmdResetEvent"))(source_command, legacy_events[1],
                                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    memset(legacy_events, 0, sizeof(legacy_events));
    metadata = metadata_at(4);
    assert(metadata.kind == VKRT_REPLAY_EVENT_WAIT && !metadata.data.sync.synchronization2 &&
           metadata.data.sync.src_stage_mask == VK_PIPELINE_STAGE_TRANSFER_BIT &&
           metadata.data.sync.dst_stage_mask == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
           metadata.data.sync.events != legacy_events && metadata.data.sync.events[1] == HANDLE(VkEvent, 26));
    assert(metadata_at(5).kind == VKRT_REPLAY_EVENT_SET && metadata_at(5).data.sync.event == HANDLE(VkEvent, 25));
    assert(metadata_at(6).kind == VKRT_REPLAY_EVENT_RESET && metadata_at(6).data.sync.event == HANDLE(VkEvent, 26));
}

static void test_state_work_and_scope_metadata(void) {
    reset();
    ((PFN_vkCmdBindPipeline)vkrt_replay_proc("vkCmdBindPipeline"))(source_command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                                   HANDLE(VkPipeline, 50));
    VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkShaderEXT shaders[] = {HANDLE(VkShaderEXT, 51), HANDLE(VkShaderEXT, 52)};
    ((PFN_vkCmdBindShadersEXT)vkrt_replay_proc("vkCmdBindShadersEXT"))(source_command, 2, stages, shaders);
    ((PFN_vkCmdDispatchIndirect)vkrt_replay_proc("vkCmdDispatchIndirect"))(source_command, HANDLE(VkBuffer, 53), 16);
    ((PFN_vkCmdFillBuffer)vkrt_replay_proc("vkCmdFillBuffer"))(source_command, HANDLE(VkBuffer, 53), 0, 16, 0);
    ((PFN_vkCmdDrawIndexed)vkrt_replay_proc("vkCmdDrawIndexed"))(source_command, 3, 1, 0, 0, 0);
    VkClearValue clear = {.color.uint32 = {1, 2, 3, 4}};
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = HANDLE(VkRenderPass, 54),
                                  .framebuffer = HANDLE(VkFramebuffer, 55),
                                  .renderArea.extent = {4, 4},
                                  .clearValueCount = 1,
                                  .pClearValues = &clear};
    VkSubpassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
                                .contents = VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS};
    VkSubpassEndInfo end = {.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    ((PFN_vkCmdBeginRenderPass2KHR)vkrt_replay_proc("vkCmdBeginRenderPass2KHR"))(source_command, &pass, &begin);
    ((PFN_vkCmdNextSubpass2KHR)vkrt_replay_proc("vkCmdNextSubpass2KHR"))(source_command, &begin, &end);
    ((PFN_vkCmdEndRenderPass2KHR)vkrt_replay_proc("vkCmdEndRenderPass2KHR"))(source_command, &end);
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = HANDLE(VkImageView, 56),
                                       .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea.extent = {4, 4},
                                 .layerCount = 1,
                                 .colorAttachmentCount = 1,
                                 .pColorAttachments = &color};
    ((PFN_vkCmdBeginRenderingKHR)vkrt_replay_proc("vkCmdBeginRenderingKHR"))(source_command, &rendering);
    ((PFN_vkCmdEndRenderingKHR)vkrt_replay_proc("vkCmdEndRenderingKHR"))(source_command);
    uint32_t locations[] = {1, 0};
    VkRenderingAttachmentLocationInfo location_info = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO,
                                                       .colorAttachmentCount = 2,
                                                       .pColorAttachmentLocations = locations};
    ((PFN_vkCmdSetRenderingAttachmentLocationsKHR)vkrt_replay_proc("vkCmdSetRenderingAttachmentLocationsKHR"))(source_command,
                                                                                                               &location_info);
    memset(stages, 0, sizeof(stages));
    memset(shaders, 0, sizeof(shaders));
    memset(&pass, 0, sizeof(pass));
    memset(&clear, 0, sizeof(clear));
    memset(&begin, 0, sizeof(begin));
    memset(&end, 0, sizeof(end));
    memset(&color, 0, sizeof(color));
    memset(&rendering, 0, sizeof(rendering));
    struct vkrt_replay_metadata metadata = metadata_at(0);
    assert(metadata.kind == VKRT_REPLAY_STATE && metadata.data.state.kind == VKRT_REPLAY_STATE_PIPELINE &&
           metadata.data.state.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE &&
           metadata.data.state.pipeline == HANDLE(VkPipeline, 50) && !metadata.logical_stages);
    metadata = metadata_at(1);
    assert(metadata.kind == VKRT_REPLAY_STATE && metadata.data.state.kind == VKRT_REPLAY_STATE_SHADERS &&
           metadata.data.state.shader_count == 2 && metadata.data.state.shader_stages != stages &&
           metadata.data.state.shaders != shaders && metadata.data.state.shaders[1] == HANDLE(VkShaderEXT, 52) &&
           metadata.data.state.affected_stages == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT));
    metadata = metadata_at(2);
    assert(metadata.kind == VKRT_REPLAY_ACTION && metadata.flags == VKRT_REPLAY_INDIRECT &&
           metadata.logical_stages == (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) &&
           metadata.logical_stages == metadata.physical_stages);
    metadata = metadata_at(3);
    assert(metadata.kind == VKRT_REPLAY_ACTION && metadata.logical_stages == VK_PIPELINE_STAGE_2_CLEAR_BIT &&
           metadata.physical_stages == VK_PIPELINE_STAGE_2_CLEAR_BIT);
    metadata = metadata_at(4);
    assert(metadata.kind == VKRT_REPLAY_ACTION && metadata.flags == VKRT_REPLAY_GRAPHICS_CONTEXT && !metadata.logical_stages &&
           !metadata.physical_stages);
    metadata = metadata_at(5);
    assert(metadata.kind == VKRT_REPLAY_RENDER_BEGIN && metadata.data.render.kind == VKRT_REPLAY_RENDER_PASS &&
           metadata.data.render.render_pass != &pass && metadata.data.render.render_pass->renderPass == HANDLE(VkRenderPass, 54) &&
           metadata.data.render.render_pass->pClearValues[0].color.uint32[3] == 4 &&
           metadata.data.render.contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    assert(metadata_at(6).kind == VKRT_REPLAY_RENDER_NEXT && metadata_at(7).kind == VKRT_REPLAY_RENDER_END);
    metadata = metadata_at(8);
    assert(metadata.kind == VKRT_REPLAY_RENDER_BEGIN && metadata.data.render.kind == VKRT_REPLAY_DYNAMIC_RENDERING &&
           metadata.data.render.rendering != &rendering &&
           metadata.data.render.rendering->pColorAttachments[0].imageView == HANDLE(VkImageView, 56));
    assert(metadata_at(9).kind == VKRT_REPLAY_RENDER_END && metadata_at(9).data.render.kind == VKRT_REPLAY_DYNAMIC_RENDERING);
    metadata = metadata_at(10);
    assert(metadata.kind == VKRT_REPLAY_STATE && metadata.flags == VKRT_REPLAY_RENDER_LOCAL && !metadata.logical_stages);
    assert(vkrt_replay_entry_metadata(entry_at(0), NULL) == VK_ERROR_INITIALIZATION_FAILED);
    memset(&metadata, 0xff, sizeof(metadata));
    assert(vkrt_replay_entry_metadata(NULL, &metadata) == VK_ERROR_INITIALIZATION_FAILED && metadata.kind == VKRT_REPLAY_UNKNOWN);
}
static void test_rejected_inputs(void) {
    reset();
    ((PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkrt_replay_proc("vkCmdPushDescriptorSetWithTemplateKHR"))(
        source_command, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, (void *)(uintptr_t)1);
    assert(record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(vkrt_replay_emit(source_context.journal, target_command, resolve, NULL, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(emit_calls == 0);
    reset();
    VkBaseInStructure unknown = {.sType = (VkStructureType)0x7ffffffe};
    VkDependencyInfo info = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = &unknown};
    ((PFN_vkCmdPipelineBarrier2)vkrt_replay_proc("vkCmdPipelineBarrier2"))(source_command, &info);
    assert(record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    reset();
    VkSampleLocationsInfoEXT cycle = {.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT};
    cycle.pNext = &cycle;
    info.pNext = &cycle;
    ((PFN_vkCmdPipelineBarrier2)vkrt_replay_proc("vkCmdPipelineBarrier2"))(source_command, &info);
    assert(record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    reset();
    ((PFN_vkCmdUpdateBuffer)vkrt_replay_proc("vkCmdUpdateBuffer"))(source_command, VK_NULL_HANDLE, 0, UINT64_MAX,
                                                                   (void *)(uintptr_t)1);
    assert(record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_OUT_OF_HOST_MEMORY);
    reset();
    uint32_t sample = 0;
    VkGpaSampleBeginInfoAMD gpa_info = {.sType = VK_STRUCTURE_TYPE_GPA_SAMPLE_BEGIN_INFO_AMD};
    assert(((PFN_vkCmdBeginGpaSampleAMD)vkrt_replay_proc("vkCmdBeginGpaSampleAMD"))(source_command, VK_NULL_HANDLE, &gpa_info,
                                                                                    &sample) == VK_SUCCESS);
    assert(sample == 77 && record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
}
static void test_header_unavailable_commands(void) {
    reset();
    const char *names[] = {"vkCmdCudaLaunchKernelNV",
                           "vkCmdDispatchGraphAMDX",
                           "vkCmdDispatchGraphIndirectAMDX",
                           "vkCmdDispatchGraphIndirectCountAMDX",
                           "vkCmdInitializeGraphScratchMemoryAMDX",
                           "vkCmdRefreshObjectsKHR"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(vkrt_replay_proc(names[i]));
        assert(vkrt_replay_journal_status(source_context.journal) == VK_SUCCESS);
    }
    typedef void(VKAPI_PTR * cuda_proc)(VkCommandBuffer, const struct VkCudaLaunchInfoNV *);
    cuda_proc cuda = (cuda_proc)vkrt_replay_proc("vkCmdCudaLaunchKernelNV");
    expected_opaque_pointer = (void *)(uintptr_t)1;
    cuda(source_command, expected_opaque_pointer);
    assert(record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!strcmp(vkrt_replay_journal_error_command(source_context.journal), "vkCmdCudaLaunchKernelNV"));
    assert(vkrt_replay_emit(source_context.journal, target_command, resolve, NULL, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(emit_calls == 0);
    reset();
    struct vkrt_replay_journal *saved = source_context.journal;
    source_context.journal = NULL;
    expected_opaque_pointer = (void *)(uintptr_t)2;
    cuda(source_command, expected_opaque_pointer);
    source_context.journal = saved;
    assert(record_calls == 1 && vkrt_replay_journal_status(saved) == VK_SUCCESS && vkrt_replay_journal_count(saved) == 0);
    reset();
    typedef void(VKAPI_PTR * graph_proc)(VkCommandBuffer, VkDeviceAddress, VkDeviceSize, VkDeviceAddress);
    ((graph_proc)vkrt_replay_proc("vkCmdDispatchGraphIndirectCountAMDX"))(source_command, UINT64_C(0x123456789abcdef0),
                                                                          UINT64_C(0x100000002), UINT64_C(0x777788889999aaaa));
    assert(record_calls == 1 && vkrt_replay_journal_status(source_context.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!strcmp(vkrt_replay_journal_error_command(source_context.journal), "vkCmdDispatchGraphIndirectCountAMDX"));
}
struct allocation_state {
    unsigned live, calls, fail_after;
};
static VKAPI_ATTR void *VKAPI_CALL allocation(void *user, size_t size, size_t align, VkSystemAllocationScope scope) {
    struct allocation_state *s = user;
    (void)align;
    assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (s->calls++ >= s->fail_after) return NULL;
    void *p = malloc(size);
    if (p) ++s->live;
    return p;
}
static VKAPI_ATTR void VKAPI_CALL release(void *user, void *p) {
    struct allocation_state *s = user;
    if (p) {
        assert(s->live);
        --s->live;
        free(p);
    }
}
static void test_metadata_no_allocation(void) {
    struct vkrt_replay_journal *previous = source_context.journal;
    struct allocation_state state = {.fail_after = UINT32_MAX};
    VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = allocation, .pfnFree = release};
    source_context.journal = vkrt_replay_journal_create(&callbacks, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    assert(source_context.journal);
    ((PFN_vkCmdResetQueryPool)vkrt_replay_proc("vkCmdResetQueryPool"))(source_command, HANDLE(VkQueryPool, 81), 4, 7);
    const unsigned calls = state.calls;
    state.fail_after = calls;
    for (unsigned i = 0; i < 10; ++i) {
        struct vkrt_replay_metadata metadata = metadata_at(0);
        assert(metadata.kind == VKRT_REPLAY_QUERY_RESET && metadata.data.query.count == 7 && state.calls == calls);
    }
    assert(vkrt_replay_journal_status(source_context.journal) == VK_SUCCESS);
    vkrt_replay_journal_destroy(source_context.journal);
    assert(!state.live);
    source_context.journal = previous;
}
static void test_oom(void) {
    vkrt_replay_journal_destroy(source_context.journal);
    source_context.journal = NULL;
    for (unsigned fail = 0; fail != 10; ++fail) {
        struct allocation_state state = {.fail_after = fail};
        VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = allocation, .pfnFree = release};
        struct vkrt_replay_journal *journal = vkrt_replay_journal_create(&callbacks, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!journal) {
            assert(state.live == 0);
            continue;
        }
        source_context.journal = journal;
        float constants[] = {1, 2, 3, 4};
        original_array = constants;
        replaying = false;
        ((PFN_vkCmdSetBlendConstants)vkrt_replay_proc("vkCmdSetBlendConstants"))(source_command, constants);
        VkResult status = vkrt_replay_journal_status(journal);
        assert(status == VK_SUCCESS || status == VK_ERROR_OUT_OF_HOST_MEMORY);
        vkrt_replay_journal_reset(journal);
        assert(state.live == 1);
        vkrt_replay_journal_destroy(journal);
        assert(state.live == 0);
        source_context.journal = NULL;
    }
}
int main(void) {
    source_context = (struct vkrt_replay_context){.resolve = resolve, .normalize_pipeline_layout = normalize_layout};
    target_context = (struct vkrt_replay_context){.resolve = resolve};
    source_context.journal = vkrt_replay_journal_create(NULL, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    assert(source_context.journal);
    assert(!vkrt_replay_proc("vkNotACommand") && !vkrt_replay_proc(NULL));
    test_begin();
    test_inherited_viewports();
    test_inactive_rendering_inheritance();
    test_barrier();
    test_build();
    test_descriptors();
    test_layout_immutable();
    test_fixed_array();
    test_children();
    test_stride_and_selector();
    test_query_metadata();
    test_virtual_work_metadata();
    test_sync_metadata();
    test_state_work_and_scope_metadata();
    test_rejected_inputs();
    test_header_unavailable_commands();
    test_metadata_no_allocation();
    test_oom();
    puts(
        "command replay host: begin/chain/arrays/AS/descriptors/layout/children/stride/selector/stage facts/owned metadata/ABI "
        "blockers/rejections/OOM "
        "passed");
    return 0;
}
