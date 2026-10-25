/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"

/* Mock only the downstream dispatch; exercise the actual layer entry points. */
struct vkrt_global vkrt_global;
static unsigned failures, updates, allocations, buffer_creates, barriers, barriers2, submits, submits2;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define HANDLE(type, value) ((type)(uintptr_t)(value))
static const VkDevice device = HANDLE(VkDevice, 1);
static const VkWriteDescriptorSet *original_writes;
static const VkCopyDescriptorSet *original_copies;
static struct vkrt_virtual_as objects[3];
static unsigned update_mode;
static const VkCommandBuffer command = HANDLE(VkCommandBuffer, 2);
static const VkQueue queue = HANDLE(VkQueue, 3);
static const VkFence fence = HANDLE(VkFence, 4);

/* This dispatch boundary deliberately accepts no native AS usage/stage/access
 * bits, even when the host running the test has a hardware-RT driver. */
static const VkPipelineStageFlags2 native_stages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                                   VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                                   VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
static const VkAccessFlags2 native_accesses =
    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
static const VkBufferCreateInfo *original_buffer;
static const VkAllocationCallbacks *buffer_allocator;
static VkBufferUsageFlags2KHR expected_usage2;
static VkBufferUsageFlags expected_usage;
static const VkExternalMemoryBufferCreateInfo *external_buffer;
static const VkBaseInStructure *buffer_suffix;
static VkResult buffer_result;

static VKAPI_ATTR VkResult VKAPI_CALL mock_buffer(VkDevice d, const VkBufferCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    ++buffer_creates;
    REQUIRE(d == device && allocator == buffer_allocator);
    REQUIRE(info->flags == original_buffer->flags && info->size == original_buffer->size);
    REQUIRE(info->sharingMode == original_buffer->sharingMode);
    REQUIRE(info->queueFamilyIndexCount == original_buffer->queueFamilyIndexCount);
    REQUIRE(info->pQueueFamilyIndices == original_buffer->pQueueFamilyIndices);
    REQUIRE(info->usage == expected_usage);
    REQUIRE(!(info->usage & (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR)));
    unsigned flags2_count = 0, external_count = 0;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) {
            const VkBufferUsageFlags2CreateInfoKHR *u = (const void *)p;
            ++flags2_count;
            REQUIRE(u->usage == expected_usage2 && u->pNext == buffer_suffix);
        } else if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO) {
            ++external_count;
            REQUIRE(external_buffer && ((const VkExternalMemoryBufferCreateInfo *)p)->handleTypes == external_buffer->handleTypes);
        } else
            REQUIRE(p == buffer_suffix);
    }
    REQUIRE(flags2_count == (expected_usage2 ? 1u : 0u));
    REQUIRE(external_count == (external_buffer ? 1u : 0u));
    *buffer = HANDLE(VkBuffer, 71);
    return buffer_result;
}

static void buffer_usages(void) {
    uint32_t families[] = {2, 5};
    VkAllocationCallbacks allocator = {0};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 4096,
                               .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               .sharingMode = VK_SHARING_MODE_CONCURRENT,
                               .queueFamilyIndexCount = 2,
                               .pQueueFamilyIndices = families};
    VkBufferCreateInfo saved = info;
    VkBuffer buffer;
    original_buffer = &info;
    buffer_allocator = &allocator;
    expected_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    REQUIRE(vkrt_CreateBuffer(device, &info, &allocator, &buffer) == VK_SUCCESS);
    REQUIRE(buffer == HANDLE(VkBuffer, 71) && !memcmp(&info, &saved, sizeof(info)));

    info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    expected_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    REQUIRE(vkrt_CreateBuffer(device, &info, &allocator, &buffer) == VK_SUCCESS);
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    expected_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    REQUIRE(vkrt_CreateBuffer(device, &info, &allocator, &buffer) == VK_SUCCESS);

    VkBaseInStructure suffix_node = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkBufferUsageFlags2CreateInfoKHR flags2 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
        .pNext = &suffix_node,
        .usage = ((VkBufferUsageFlags2KHR)1 << 40) | VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_2_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR};
    VkExternalMemoryBufferCreateInfo prefix = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                               .pNext = &flags2,
                                               .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    info.pNext = &prefix;
    external_buffer = &prefix;
    buffer_suffix = &suffix_node;
    expected_usage2 = ((VkBufferUsageFlags2KHR)1 << 40) | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR |
                      VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR;
    VkBufferUsageFlags2CreateInfoKHR saved2 = flags2;
    saved = info;
    REQUIRE(vkrt_CreateBuffer(device, &info, &allocator, &buffer) == VK_SUCCESS);
    REQUIRE(!memcmp(&info, &saved, sizeof(info)) && !memcmp(&flags2, &saved2, sizeof(flags2)));
    REQUIRE(prefix.pNext == &flags2 && flags2.pNext == &suffix_node);
    buffer_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    REQUIRE(vkrt_CreateBuffer(device, &info, &allocator, &buffer) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    REQUIRE(!memcmp(&flags2, &saved2, sizeof(flags2)));
    buffer_result = VK_SUCCESS;
    /* A future/unknown prefix cannot be safely cloned. No partial request is
     * sent downstream and caller-owned storage remains intact. */
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .pNext = (const void *)&flags2};
    info.pNext = &unknown;
    REQUIRE(vkrt_CreateBuffer(device, &info, &allocator, &buffer) == VK_ERROR_EXTENSION_NOT_PRESENT);
    REQUIRE(buffer_creates == 5 && unknown.pNext == (const void *)&flags2);
}

static VKAPI_ATTR void VKAPI_CALL mock_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                               VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                               const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    ++barriers;
    REQUIRE(cmd == command && flags == VK_DEPENDENCY_BY_REGION_BIT);
    REQUIRE(!(src & native_stages) && !(dst & native_stages));
    REQUIRE(src == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT));
    REQUIRE(dst == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    REQUIRE(nm == 2 && nb == 1 && ni == 1);
    REQUIRE(m[0].srcAccessMask == (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT));
    REQUIRE(m[0].dstAccessMask == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT));
    REQUIRE(m[1].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT && m[1].dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
    REQUIRE(b->srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT && b->dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
    REQUIRE(b->buffer == HANDLE(VkBuffer, 91) && b->offset == 128 && b->size == 4096);
    REQUIRE(b->srcQueueFamilyIndex == 2 && b->dstQueueFamilyIndex == 5);
    REQUIRE(im->srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT && im->dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
    REQUIRE(im->image == HANDLE(VkImage, 92) && im->oldLayout == VK_IMAGE_LAYOUT_GENERAL);
    REQUIRE(im->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    REQUIRE(
        !((m[0].srcAccessMask | m[0].dstAccessMask | b->srcAccessMask | b->dstAccessMask | im->srcAccessMask | im->dstAccessMask) &
          native_accesses));
}

static const VkDependencyInfo *original_dependency;
static VKAPI_ATTR void VKAPI_CALL mock_barrier2(VkCommandBuffer cmd, const VkDependencyInfo *info) {
    ++barriers2;
    REQUIRE(cmd == command && info->pNext == original_dependency->pNext);
    REQUIRE(info->dependencyFlags == original_dependency->dependencyFlags);
    REQUIRE(info->memoryBarrierCount == 1 && info->bufferMemoryBarrierCount == 1 && info->imageMemoryBarrierCount == 1);
    const VkMemoryBarrier2 *m = info->pMemoryBarriers;
    const VkBufferMemoryBarrier2 *b = info->pBufferMemoryBarriers;
    const VkImageMemoryBarrier2 *im = info->pImageMemoryBarriers;
    REQUIRE(m->srcStageMask == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT));
    REQUIRE(m->dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    REQUIRE(m->srcAccessMask == (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT));
    REQUIRE(m->dstAccessMask == VK_ACCESS_2_SHADER_READ_BIT);
    REQUIRE(b->srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && b->dstStageMask == VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
    REQUIRE(b->srcAccessMask == VK_ACCESS_2_SHADER_READ_BIT && b->dstAccessMask == VK_ACCESS_2_INDEX_READ_BIT);
    REQUIRE(b->srcQueueFamilyIndex == 2 && b->dstQueueFamilyIndex == 5 && b->offset == 256 && b->size == 1024);
    REQUIRE(im->srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
            im->dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    REQUIRE(im->srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT && im->dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    REQUIRE(im->oldLayout == VK_IMAGE_LAYOUT_GENERAL && im->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    REQUIRE(!((m->srcStageMask | m->dstStageMask | b->srcStageMask | b->dstStageMask | im->srcStageMask | im->dstStageMask) &
              native_stages));
    REQUIRE(!((m->srcAccessMask | m->dstAccessMask | b->srcAccessMask | b->dstAccessMask | im->srcAccessMask | im->dstAccessMask) &
              native_accesses));
}

static VkResult submit_result;
static const VkSubmitInfo *original_submit;
static VKAPI_ATTR VkResult VKAPI_CALL mock_submit(VkQueue q, uint32_t count, const VkSubmitInfo *info, VkFence f) {
    ++submits;
    REQUIRE(q == queue && f == fence && count == 2);
    REQUIRE(info[0].pWaitDstStageMask[0] == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    REQUIRE(info[0].pWaitDstStageMask[1] == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
    REQUIRE(info[1].pWaitDstStageMask[0] == VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (unsigned i = 0; i < count; ++i) {
        REQUIRE(info[i].pNext == original_submit[i].pNext && info[i].pWaitSemaphores == original_submit[i].pWaitSemaphores);
        REQUIRE(info[i].pCommandBuffers == original_submit[i].pCommandBuffers &&
                info[i].pSignalSemaphores == original_submit[i].pSignalSemaphores);
        for (unsigned j = 0; j < info[i].waitSemaphoreCount; ++j) REQUIRE(!(info[i].pWaitDstStageMask[j] & native_stages));
    }
    return submit_result;
}

static const VkSubmitInfo2 *original_submit2;
static VKAPI_ATTR VkResult VKAPI_CALL mock_submit2(VkQueue q, uint32_t count, const VkSubmitInfo2 *info, VkFence f) {
    ++submits2;
    REQUIRE(q == queue && f == fence && count == 2);
    REQUIRE(info[0].pWaitSemaphoreInfos[0].stageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    REQUIRE(info[0].pSignalSemaphoreInfos[0].stageMask == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT));
    REQUIRE(info[1].pWaitSemaphoreInfos[0].stageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    REQUIRE(info[1].pSignalSemaphoreInfos[0].stageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    for (unsigned i = 0; i < count; ++i) {
        REQUIRE(info[i].pNext == original_submit2[i].pNext && info[i].flags == original_submit2[i].flags);
        REQUIRE(info[i].pCommandBufferInfos == original_submit2[i].pCommandBufferInfos);
        const VkSemaphoreSubmitInfo *w = info[i].pWaitSemaphoreInfos, *s = info[i].pSignalSemaphoreInfos;
        REQUIRE(w->semaphore == original_submit2[i].pWaitSemaphoreInfos->semaphore && w->value == 35 && w->deviceIndex == 2);
        REQUIRE(s->semaphore == original_submit2[i].pSignalSemaphoreInfos->semaphore && s->value == 41 && s->deviceIndex == 3);
        REQUIRE(!(w->stageMask & native_stages) && !(s->stageMask & native_stages));
    }
    return submit_result;
}

static void synchronization(struct vkrt_device_data *dev) {
    dev->vtable.CmdPipelineBarrier = mock_barrier;
    dev->vtable.CmdPipelineBarrier2 = mock_barrier2;
    dev->vtable.QueueSubmit = mock_submit;
    dev->vtable.QueueSubmit2 = mock_submit2;
    struct vkrt_cmd_buffer_data cb = {.cmd = command, .device_data = dev};
    vkrt_map_object(VKRT_HKEY(command), &cb);
    vkrt_map_object(VKRT_HKEY(queue), dev);
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)vkrt_sync_proc("vkCmdPipelineBarrier");
    PFN_vkCmdPipelineBarrier2 barrier2 = (PFN_vkCmdPipelineBarrier2)vkrt_sync_proc("vkCmdPipelineBarrier2KHR");
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)vkrt_sync_proc("vkQueueSubmit");
    PFN_vkQueueSubmit2 submit2 = (PFN_vkQueueSubmit2)vkrt_sync_proc("vkQueueSubmit2KHR");
    REQUIRE(barrier && barrier2 && submit && submit2);
    REQUIRE(vkrt_sync_proc("vkCmdPipelineBarrier2") == vkrt_sync_proc("vkCmdPipelineBarrier2KHR"));
    REQUIRE(vkrt_sync_proc("vkQueueSubmit2") == vkrt_sync_proc("vkQueueSubmit2KHR"));
    REQUIRE(vkrt_sync_proc("vkUnsupported") == NULL);
    VkMemoryBarrier m[2] = {{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_UNIFORM_READ_BIT},
                            {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
    VkBufferMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                               .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                               .srcQueueFamilyIndex = 2,
                               .dstQueueFamilyIndex = 5,
                               .buffer = HANDLE(VkBuffer, 91),
                               .offset = 128,
                               .size = 4096};
    VkImageMemoryBarrier im = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .srcAccessMask = b.srcAccessMask,
                               .dstAccessMask = b.dstAccessMask,
                               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               .image = HANDLE(VkImage, 92)};
    VkMemoryBarrier saved_m[2];
    memcpy(saved_m, m, sizeof(m));
    VkBufferMemoryBarrier saved_b = b;
    VkImageMemoryBarrier saved_im = im;
    barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 2, m, 1, &b, 1, &im);
    REQUIRE(barriers == 1 && !memcmp(m, saved_m, sizeof(m)) && !memcmp(&b, &saved_b, sizeof(b)) &&
            !memcmp(&im, &saved_im, sizeof(im)));

    VkMemoryBarrier2 m2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .srcAccessMask = m[0].srcAccessMask,
                           .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    VkBufferMemoryBarrier2 b2 = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                                 .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                                 .dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT,
                                 .srcQueueFamilyIndex = 2,
                                 .dstQueueFamilyIndex = 5,
                                 .buffer = HANDLE(VkBuffer, 91),
                                 .offset = 256,
                                 .size = 1024};
    VkImageMemoryBarrier2 im2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 .oldLayout = im.oldLayout,
                                 .newLayout = im.newLayout,
                                 .image = im.image};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                            .memoryBarrierCount = 1,
                            .pMemoryBarriers = &m2,
                            .bufferMemoryBarrierCount = 1,
                            .pBufferMemoryBarriers = &b2,
                            .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = &im2};
    VkMemoryBarrier2 saved_m2 = m2;
    VkBufferMemoryBarrier2 saved_b2 = b2;
    VkImageMemoryBarrier2 saved_im2 = im2;
    VkDependencyInfo saved_dep = dep;
    original_dependency = &dep;
    barrier2(command, &dep);
    REQUIRE(barriers2 == 1 && !memcmp(&dep, &saved_dep, sizeof(dep)) && !memcmp(&m2, &saved_m2, sizeof(m2)));
    REQUIRE(!memcmp(&b2, &saved_b2, sizeof(b2)) && !memcmp(&im2, &saved_im2, sizeof(im2)));
    dev->vtable.CmdPipelineBarrier2 = NULL;
    barrier2(command, &dep);
    REQUIRE(barriers2 == 1 && cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);

    VkPipelineStageFlags wait[3] = {VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkPipelineStageFlags saved_wait[3];
    memcpy(saved_wait, wait, sizeof(wait));
    VkSemaphore semaphores[] = {HANDLE(VkSemaphore, 51), HANDLE(VkSemaphore, 52)};
    VkSubmitInfo infos[2] = {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .waitSemaphoreCount = 2,
                              .pWaitSemaphores = semaphores,
                              .pWaitDstStageMask = wait,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &command,
                              .signalSemaphoreCount = 1,
                              .pSignalSemaphores = semaphores},
                             {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .waitSemaphoreCount = 1,
                              .pWaitSemaphores = semaphores,
                              .pWaitDstStageMask = wait + 2}};
    VkSubmitInfo saved_infos[2];
    memcpy(saved_infos, infos, sizeof(infos));
    original_submit = infos;
    REQUIRE(submit(queue, 2, infos, fence) == VK_SUCCESS);
    submit_result = VK_ERROR_DEVICE_LOST;
    REQUIRE(submit(queue, 2, infos, fence) == VK_ERROR_DEVICE_LOST);
    REQUIRE(submits == 2 && !memcmp(saved_wait, wait, sizeof(wait)) && !memcmp(saved_infos, infos, sizeof(infos)));

    VkSemaphoreSubmitInfo waits[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = semaphores[0],
                                       .value = 35,
                                       .stageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                       .deviceIndex = 2},
                                      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = semaphores[1],
                                       .value = 35,
                                       .stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       .deviceIndex = 2}};
    VkSemaphoreSubmitInfo signals[2] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = semaphores[0],
         .value = 41,
         .stageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR | VK_PIPELINE_STAGE_2_COPY_BIT,
         .deviceIndex = 3},
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = semaphores[1],
         .value = 41,
         .stageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
         .deviceIndex = 3}};
    VkSubmitInfo2 infos2[2] = {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                .waitSemaphoreInfoCount = 1,
                                .pWaitSemaphoreInfos = waits,
                                .signalSemaphoreInfoCount = 1,
                                .pSignalSemaphoreInfos = signals},
                               {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                .waitSemaphoreInfoCount = 1,
                                .pWaitSemaphoreInfos = waits + 1,
                                .signalSemaphoreInfoCount = 1,
                                .pSignalSemaphoreInfos = signals + 1}};
    VkSemaphoreSubmitInfo saved_waits[2], saved_signals[2];
    memcpy(saved_waits, waits, sizeof(waits));
    memcpy(saved_signals, signals, sizeof(signals));
    VkSubmitInfo2 saved_infos2[2];
    memcpy(saved_infos2, infos2, sizeof(infos2));
    original_submit2 = infos2;
    submit_result = VK_SUCCESS;
    REQUIRE(submit2(queue, 2, infos2, fence) == VK_SUCCESS);
    submit_result = VK_ERROR_DEVICE_LOST;
    REQUIRE(submit2(queue, 2, infos2, fence) == VK_ERROR_DEVICE_LOST);
    REQUIRE(submits2 == 2 && !memcmp(saved_waits, waits, sizeof(waits)) && !memcmp(saved_signals, signals, sizeof(signals)));
    REQUIRE(!memcmp(saved_infos2, infos2, sizeof(infos2)));
    dev->vtable.QueueSubmit2 = NULL;
    REQUIRE(submit2(queue, 2, infos2, fence) == VK_ERROR_FEATURE_NOT_PRESENT && submits2 == 2);
    vkrt_unmap_object(VKRT_HKEY(command));
    vkrt_unmap_object(VKRT_HKEY(queue));
}

static VKAPI_ATTR void VKAPI_CALL mock_update(VkDevice d, uint32_t count,
   const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
   ++updates;
   REQUIRE(d == device);
   REQUIRE(copy_count == 1 && copies == original_copies);
   if (update_mode == 0) {
      REQUIRE(count == 3);
      REQUIRE(!memcmp(&writes[1], &original_writes[1], sizeof(writes[1])));
      for (unsigned i = 0; i < 3; i += 2) {
         REQUIRE(writes[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
         REQUIRE(writes[i].dstSet == original_writes[i].dstSet);
         REQUIRE(writes[i].dstBinding == original_writes[i].dstBinding);
         REQUIRE(writes[i].dstArrayElement == original_writes[i].dstArrayElement);
         REQUIRE(writes[i].descriptorCount == original_writes[i].descriptorCount);
         REQUIRE(!writes[i].pNext && !writes[i].pImageInfo && !writes[i].pTexelBufferView);
      }
      const unsigned indices[] = {0, 1, 2, 2, 0};
      for (unsigned j = 0; j < 5; ++j) {
         const VkDescriptorBufferInfo *b = j < 3 ? &writes[0].pBufferInfo[j] : &writes[2].pBufferInfo[j - 3];
         REQUIRE(b->buffer == objects[indices[j]].backing_buffer);
         REQUIRE(b->offset == objects[indices[j]].offset);
         REQUIRE(b->range == objects[indices[j]].size);
      }
   } else if (update_mode == 1) {
      REQUIRE(count == 1 && writes[0].descriptorCount == 2);
      REQUIRE(writes[0].pBufferInfo[0].buffer == VK_NULL_HANDLE);
      REQUIRE(writes[0].pBufferInfo[0].offset == 0);
      REQUIRE(writes[0].pBufferInfo[0].range == VK_WHOLE_SIZE);
      REQUIRE(writes[0].pBufferInfo[1].buffer == objects[1].backing_buffer);
   } else {
      REQUIRE(writes == original_writes);
   }
}

static const VkMemoryAllocateInfo *original_alloc;
static const VkMemoryDedicatedAllocateInfo *dedicated;
static const VkMemoryOpaqueCaptureAddressAllocateInfo *suffix;
static VkMemoryAllocateFlags expected_flags;
static uint32_t expected_mask;
static VkResult allocation_result;
static VKAPI_ATTR VkResult VKAPI_CALL mock_allocate(VkDevice d, const VkMemoryAllocateInfo *info,
   const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
   ++allocations;
   REQUIRE(d == device && allocator == NULL);
   REQUIRE(info->allocationSize == original_alloc->allocationSize);
   REQUIRE(info->memoryTypeIndex == original_alloc->memoryTypeIndex);
   unsigned flag_count = 0, dedicated_count = 0, suffix_count = 0;
   for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
      if (p->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
         const VkMemoryAllocateFlagsInfo *f = (const void *)p;
         ++flag_count;
         REQUIRE(f->flags == expected_flags && f->deviceMask == expected_mask);
      } else if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
         const VkMemoryDedicatedAllocateInfo *m = (const void *)p;
         ++dedicated_count;
         REQUIRE(dedicated && m->buffer == dedicated->buffer && m->image == dedicated->image);
      } else if (p->sType == VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO) {
         ++suffix_count;
         REQUIRE((const void *)p == (const void *)suffix);
      } else REQUIRE(false);
   }
   REQUIRE(flag_count == 1);
   REQUIRE(dedicated_count == (dedicated ? 1u : 0u));
   REQUIRE(suffix_count == (suffix ? 1u : 0u));
   *memory = HANDLE(VkDeviceMemory, 123);
   return allocation_result;
}

static void descriptors(void)
{
   VkAccelerationStructureKHR handles[5];
   for (unsigned i = 0; i < 3; ++i) {
      objects[i] = (struct vkrt_virtual_as){.owner = device,
         .backing_buffer = HANDLE(VkBuffer, 100 + i), .offset = 256 * (i + 1), .size = 512 * (i + 1)};
      handles[i] = HANDLE(VkAccelerationStructureKHR, 200 + i);
      vkrt_map_object(VKRT_HKEY(handles[i]), &objects[i]);
   }
   handles[3] = handles[2]; handles[4] = handles[0];
   VkWriteDescriptorSetAccelerationStructureKHR chains[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, .accelerationStructureCount = 3, .pAccelerationStructures = handles},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, .accelerationStructureCount = 2, .pAccelerationStructures = handles + 3}};
   VkDescriptorBufferInfo ordinary = {HANDLE(VkBuffer, 77), 16, 64};
   VkWriteDescriptorSet writes[3] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &chains[0], .dstSet = HANDLE(VkDescriptorSet, 9),
         .dstBinding = 7, .dstArrayElement = 2, .descriptorCount = 3, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ordinary},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &chains[1], .dstSet = HANDLE(VkDescriptorSet, 10),
         .dstBinding = 11, .dstArrayElement = 4, .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}};
   VkWriteDescriptorSet snapshot[3]; memcpy(snapshot, writes, sizeof(writes));
   VkCopyDescriptorSet copy = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .descriptorCount = 1};
   original_writes = writes; original_copies = &copy;
   vkrt_UpdateDescriptorSets(device, 3, writes, 1, &copy);
   REQUIRE(updates == 1 && !memcmp(snapshot, writes, sizeof(writes)));
   REQUIRE(chains[0].pAccelerationStructures == handles && chains[0].accelerationStructureCount == 3);

   update_mode = 1;
   handles[0] = VK_NULL_HANDLE;
   chains[0].accelerationStructureCount = writes[0].descriptorCount = 2;
   vkrt_UpdateDescriptorSets(device, 1, writes, 1, &copy);
   REQUIRE(updates == 2);
   /* Bad counts/handles must not be forwarded as zero-count or partial writes. */
   chains[0].accelerationStructureCount = 1;
   vkrt_UpdateDescriptorSets(device, 1, writes, 1, &copy);
   REQUIRE(updates == 2);
   chains[0].accelerationStructureCount = 2;
   handles[1] = HANDLE(VkAccelerationStructureKHR, 999);
   vkrt_UpdateDescriptorSets(device, 1, writes, 1, &copy);
   REQUIRE(updates == 2);
   update_mode = 2;
   original_writes = &writes[1];
   vkrt_UpdateDescriptorSets(device, 1, original_writes, 1, &copy);
   original_writes = NULL;
   vkrt_UpdateDescriptorSets(device, 0, NULL, 1, &copy);
   REQUIRE(updates == 4);
}

static void memory_flags(void)
{
   VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096, .memoryTypeIndex = 3};
   original_alloc = &info;
   expected_flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
   VkDeviceMemory memory;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(info.pNext == NULL && memory == HANDLE(VkDeviceMemory, 123));
   VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
   info.pNext = &flags;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(flags.flags == 0 && flags.pNext == NULL);

   VkMemoryOpaqueCaptureAddressAllocateInfo tail = {.sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
      .opaqueCaptureAddress = 0x123456789abcdef0ULL};
   VkMemoryDedicatedAllocateInfo prefix = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &flags, .buffer = HANDLE(VkBuffer, 456)};
   flags.flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
   flags.deviceMask = 3; flags.pNext = &tail;
   info.pNext = &prefix;
   dedicated = &prefix; suffix = &tail;
   expected_flags |= flags.flags; expected_mask = flags.deviceMask;
   VkMemoryAllocateFlagsInfo saved = flags;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(!memcmp(&saved, &flags, sizeof(flags)) && prefix.pNext == &flags && info.pNext == &prefix);
   allocation_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   REQUIRE(!memcmp(&saved, &flags, sizeof(flags)));
   allocation_result = VK_SUCCESS;
   flags.flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(allocations == 5);

   /* An unrecognized prefix cannot be copied safely: reject without changing
    * the caller's chain or forwarding a partially cloned allocation. */
   VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .pNext = (const void *)&flags};
   flags.flags = 0;
   prefix.pNext = &unknown;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_ERROR_EXTENSION_NOT_PRESENT);
   REQUIRE(allocations == 5 && flags.flags == 0 && prefix.pNext == &unknown);
   REQUIRE(unknown.pNext == (const void *)&flags);
}

int main(void)
{
   struct vkrt_device_data dev = {.device = device};
   dev.vtable.UpdateDescriptorSets = mock_update;
   dev.vtable.AllocateMemory = mock_allocate;
   dev.vtable.CreateBuffer = mock_buffer;
   vkrt_map_object(VKRT_HKEY(device), &dev);
   descriptors();
   memory_flags();
   buffer_usages();
   synchronization(&dev);
   printf("Host translation: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
