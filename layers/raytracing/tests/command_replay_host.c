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
    replay(NULL);
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
    test_rejected_inputs();
    test_header_unavailable_commands();
    test_oom();
    puts(
        "command replay host: begin/chain/arrays/AS/descriptors/layout/children/stride/selector/ABI blockers/rejections/OOM "
        "passed");
    return 0;
}
