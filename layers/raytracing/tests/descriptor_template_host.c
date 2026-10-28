/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Public template hooks and the real owning generated command schemas.
 */
#include "vkrt_descriptor_template.h"
#include "vkrt_replay_capture_generated.h"
#include "vkrt_layer.h"
#include <stdatomic.h>

#define H(T, n) ((T)(uintptr_t)(n))
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            exit(1);                                        \
        }                                                   \
    } while (0)
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static struct vkrt_device_data device;
static struct vkrt_cmd_buffer_data source, target;
static unsigned native_creates, native_destroys, native_updates, pushes, lookups;
static unsigned inline_creates, inline_destroys;
static bool replaying;
static uint8_t original[256];
static const VkPipelineLayout layout = H(VkPipelineLayout, 100);
struct allocation_state {
    unsigned calls, live, fail_after;
};
static VKAPI_ATTR void *VKAPI_CALL allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct allocation_state *state = user;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT && alignment >= _Alignof(void *));
    if (state->calls++ >= state->fail_after) return NULL;
    void *memory = malloc(size);
    if (memory) ++state->live;
    return memory;
}
static VKAPI_ATTR void VKAPI_CALL release(void *user, void *memory) {
    struct allocation_state *state = user;
    CHECK(memory && state->live);
    --state->live;
    free(memory);
}
static void check_writes(uint32_t count, const VkWriteDescriptorSet *writes, VkDescriptorSet destination) {
    CHECK(count == 4);
    for (uint32_t i = 0; i < count; ++i) CHECK(writes[i].dstSet == destination && writes[i].dstBinding == i);
    CHECK(writes[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && writes[0].descriptorCount == 2);
    CHECK(writes[0].pBufferInfo[0].buffer == H(VkBuffer, 51) && writes[0].pBufferInfo[0].offset == 123);
    CHECK(writes[0].pBufferInfo[1].buffer == H(VkBuffer, 52) && writes[0].pBufferInfo[1].offset == 456);
    const VkWriteDescriptorSetAccelerationStructureKHR *as = writes[1].pNext;
    CHECK(as && as->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR && !as->pNext);
    CHECK(as->accelerationStructureCount == 2 && as->pAccelerationStructures[0] == H(VkAccelerationStructureKHR, 88) &&
          as->pAccelerationStructures[1] == H(VkAccelerationStructureKHR, 89));
    const VkWriteDescriptorSetInlineUniformBlock *block = writes[2].pNext;
    CHECK(block && block->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK && !block->pNext);
    CHECK(block->dataSize == 8 && !memcmp(block->pData, "ABCDEFGH", 8));
    CHECK(writes[3].pImageInfo[0].sampler == H(VkSampler, 77));
}
static VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout native_layout,
                                       uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    CHECK(command == (replaying ? target.cmd : source.cmd) && point == VK_PIPELINE_BIND_POINT_COMPUTE && set == 2);
    CHECK(native_layout == (replaying ? H(VkPipelineLayout, 300) : layout));
    check_writes(count, writes, VK_NULL_HANDLE);
    ++pushes;
}
static VKAPI_ATTR void VKAPI_CALL update(VkDevice dev, uint32_t count, const VkWriteDescriptorSet *writes, uint32_t copies,
                                         const VkCopyDescriptorSet *copy) {
    CHECK(dev == device.device && !copies && !copy);
    check_writes(count, writes, H(VkDescriptorSet, 901));
    ++native_updates;
}
static VKAPI_ATTR void VKAPI_CALL push2(VkCommandBuffer command, const VkPushDescriptorSetInfo *info) {
    CHECK(command == source.cmd && info->stageFlags == VK_SHADER_STAGE_COMPUTE_BIT && info->set == 2);
    CHECK(info->pNext && ((const VkBaseInStructure *)info->pNext)->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    CHECK(!info->layout || info->layout == layout);
    check_writes(info->descriptorWriteCount, info->pDescriptorWrites, VK_NULL_HANDLE);
    ++pushes;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_inline(VkDevice dev, const VkPipelineLayoutCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkPipelineLayout *out) {
    CHECK(dev == device.device && !allocator && info->setLayoutCount == 3 && !info->pNext);
    CHECK(info->pSetLayouts[2] == H(VkDescriptorSetLayout, 53));
    *out = H(VkPipelineLayout, 101);
    ++inline_creates;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_inline(VkDevice dev, VkPipelineLayout handle, const VkAllocationCallbacks *allocator) {
    CHECK(dev == device.device && handle == H(VkPipelineLayout, 101) && !allocator);
    CHECK(vkrt_replay_journal_count(source.replay.journal) == 1);
    ++inline_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_create(VkDevice dev, const VkDescriptorUpdateTemplateCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkDescriptorUpdateTemplate *out) {
    CHECK(dev == device.device && info && !allocator);
    *out = H(VkDescriptorUpdateTemplate, 900);
    ++native_creates;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL native_destroy(VkDevice dev, VkDescriptorUpdateTemplate handle,
                                                 const VkAllocationCallbacks *allocator) {
    CHECK(dev == device.device && handle == H(VkDescriptorUpdateTemplate, 900) && !allocator);
    ++native_destroys;
}
static VKAPI_ATTR void VKAPI_CALL native_update_template(VkDevice dev, VkDescriptorSet set, VkDescriptorUpdateTemplate handle,
                                                         const void *data) {
    CHECK(dev == device.device && set == H(VkDescriptorSet, 901) && handle == H(VkDescriptorUpdateTemplate, 900) &&
          data == original);
    ++native_updates;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc(VkDevice dev, const char *name) {
    CHECK(dev == device.device);
    if (!strcmp(name, "vkCreateDescriptorUpdateTemplate")) return (PFN_vkVoidFunction)native_create;
    if (!strcmp(name, "vkDestroyDescriptorUpdateTemplate")) return (PFN_vkVoidFunction)native_destroy;
    if (!strcmp(name, "vkUpdateDescriptorSetWithTemplate")) return (PFN_vkVoidFunction)native_update_template;
    return NULL;
}
PFN_vkVoidFunction vkrt_device_direct_proc(struct vkrt_device_data *dev, const char *name) {
    CHECK(dev == &device);
    PFN_vkVoidFunction proc = vkrt_descriptor_template_proc(name);
    if (proc) return proc;
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR")) return (PFN_vkVoidFunction)push;
    if (!strcmp(name, "vkCmdPushDescriptorSet2KHR")) return (PFN_vkVoidFunction)push2;
    if (!strcmp(name, "vkUpdateDescriptorSets")) return (PFN_vkVoidFunction)update;
    if (!strcmp(name, "vkCreatePipelineLayout")) return (PFN_vkVoidFunction)create_inline;
    if (!strcmp(name, "vkDestroyPipelineLayout")) return (PFN_vkVoidFunction)destroy_inline;
    return NULL;
}
static PFN_vkVoidFunction resolve(void *user, const char *name) {
    return vkrt_device_direct_proc(((struct vkrt_cmd_buffer_data *)user)->device_data, name);
}
static VkResult capture_layout(void *user, VkPipelineLayout public_layout, VkPipelineLayout *identity) {
    CHECK(user == &source && (public_layout == layout || public_layout == H(VkPipelineLayout, 101)));
    *identity = H(VkPipelineLayout, 200);
    return VK_SUCCESS;
}
static VkResult emit_layout(void *user, const char *name, VkPipelineLayout identity, VkPipelineLayout *native_layout) {
    CHECK(user == &target && !strcmp(name, "vkCmdPushDescriptorSetKHR") && identity == H(VkPipelineLayout, 200));
    *native_layout = H(VkPipelineLayout, 300);
    return VK_SUCCESS;
}
struct vkrt_replay_context *vkrt_replay_lookup_context(VkCommandBuffer command) {
    ++lookups;
    return command == source.cmd ? &source.replay : command == target.cmd ? &target.replay : NULL;
}
static void fill_data(void) {
    memset(original, 0xcd, sizeof(original));
    const VkDescriptorBufferInfo buffers[] = {{H(VkBuffer, 51), 123, 2048}, {H(VkBuffer, 52), 456, 4096}};
    memcpy(original + 3, &buffers[0], sizeof(buffers[0]));
    memcpy(original + 40, &buffers[1], sizeof(buffers[1]));
    VkAccelerationStructureKHR handles[] = {H(VkAccelerationStructureKHR, 88), H(VkAccelerationStructureKHR, 89)};
    memcpy(original + 101, &handles[0], sizeof(handles[0]));
    memcpy(original + 114, &handles[1], sizeof(handles[1]));
    memcpy(original + 157, "ABCDEFGH", 8);
    const VkDescriptorImageInfo image = {H(VkSampler, 77), H(VkImageView, 0x7fffffff), (VkImageLayout)0x7fffffff};
    memcpy(original + 181, &image, sizeof(image));
}
int main(void) {
    device.device = H(VkDevice, 11);
    device.virtual_rt = true;
    device.vtable.GetDeviceProcAddr = get_proc;
    vkrt_map_object(VKRT_HKEY(device.device), &device);
    source = (struct vkrt_cmd_buffer_data){.cmd = H(VkCommandBuffer, 1), .device_data = &device};
    target = (struct vkrt_cmd_buffer_data){.cmd = H(VkCommandBuffer, 2), .device_data = &device, .replaying = true};
    source.replay = (struct vkrt_replay_context){.journal = vkrt_replay_journal_create(NULL, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND),
                                                 .resolve = resolve,
                                                 .user = &source,
                                                 .normalize_pipeline_layout = capture_layout,
                                                 .capture_descriptor_template = vkrt_descriptor_template_capture};
    target.replay = (struct vkrt_replay_context){.resolve = resolve, .user = &target, .emit_pipeline_layout = emit_layout};
    CHECK(source.replay.journal);
    vkrt_map_object(VKRT_HKEY(source.cmd), &source);
    vkrt_map_object(VKRT_HKEY(target.cmd), &target);
    PFN_vkCreateDescriptorUpdateTemplate create = (void *)vkrt_descriptor_template_proc("vkCreateDescriptorUpdateTemplateKHR");
    PFN_vkDestroyDescriptorUpdateTemplate destroy = (void *)vkrt_descriptor_template_proc("vkDestroyDescriptorUpdateTemplate");
    PFN_vkUpdateDescriptorSetWithTemplate update_template =
        (void *)vkrt_descriptor_template_proc("vkUpdateDescriptorSetWithTemplateKHR");
    CHECK(create && destroy && update_template);
    VkDescriptorUpdateTemplateEntry entries[] = {{0, 0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, 37},
                                                 {1, 0, 2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 101, 13},
                                                 {2, 0, 8, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 157, SIZE_MAX},
                                                 {3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 181, sizeof(VkDescriptorImageInfo)}};
    VkDescriptorUpdateTemplateCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                                                 .descriptorUpdateEntryCount = 4,
                                                 .pDescriptorUpdateEntries = entries,
                                                 .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
                                                 .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
                                                 .pipelineLayout = layout,
                                                 .set = 2};
    struct allocation_state owner = {.fail_after = UINT32_MAX};
    VkAllocationCallbacks allocator = {.pUserData = &owner, .pfnAllocation = allocate, .pfnFree = release};
    VkDescriptorUpdateTemplate handle;
    CHECK(create(device.device, &info, &allocator, &handle) == VK_SUCCESS && handle && owner.live && !native_creates);
    fill_data();
    entries[0].offset = 250; /* The creation metadata must have been copied. */
    ((PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkrt_replay_proc("vkCmdPushDescriptorSetWithTemplateKHR"))(source.cmd, handle,
                                                                                                           layout, 2, original);
    CHECK(pushes == 1 && source.record_result == VK_SUCCESS && vkrt_replay_journal_count(source.replay.journal) == 1);
    CHECK(!strcmp(vkrt_replay_entry_name(vkrt_replay_first(source.replay.journal)), "vkCmdPushDescriptorSetKHR"));
    destroy(device.device, handle, &allocator);
    CHECK(!owner.live && !native_destroys);
    memset(original, 0, sizeof(original));
    replaying = true;
    CHECK(vkrt_replay_emit(source.replay.journal, target.cmd, resolve, &target, NULL) == VK_SUCCESS && pushes == 2);
    replaying = false;
    vkrt_replay_journal_reset(source.replay.journal);
    entries[0].offset = 3;
    info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    CHECK(create(device.device, &info, &allocator, &handle) == VK_SUCCESS);
    fill_data();
    unsigned calls = owner.calls;
    owner.fail_after = calls;
    update_template(device.device, H(VkDescriptorSet, 901), handle, original);
    CHECK(native_updates == 1 && owner.calls == calls); /* Applying a void update must not allocate. */
    destroy(device.device, handle, &allocator);
    CHECK(!owner.live);
    owner.fail_after = UINT32_MAX;
    info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    CHECK(create(device.device, &info, &allocator, &handle) == VK_SUCCESS);
    VkPushDescriptorSetWithTemplateInfo push_info = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,
                                                     .descriptorUpdateTemplate = handle,
                                                     .layout = layout,
                                                     .set = 2,
                                                     .pData = original};
    ((PFN_vkCmdPushDescriptorSetWithTemplate2KHR)vkrt_replay_proc("vkCmdPushDescriptorSetWithTemplate2KHR"))(source.cmd,
                                                                                                             &push_info);
    CHECK(pushes == 3 && vkrt_replay_journal_count(source.replay.journal) == 1);
    vkrt_replay_journal_reset(source.replay.journal);
    /* An existing layout ignores the inline layout's descriptor arrays. */
    VkPipelineLayoutCreateInfo inline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = (void *)(uintptr_t)1};
    push_info.pNext = &inline_info;
    ((PFN_vkCmdPushDescriptorSetWithTemplate2KHR)vkrt_replay_proc("vkCmdPushDescriptorSetWithTemplate2KHR"))(source.cmd,
                                                                                                             &push_info);
    CHECK(pushes == 4 && !inline_creates && vkrt_replay_journal_count(source.replay.journal) == 1);
    vkrt_replay_journal_reset(source.replay.journal);
    VkDescriptorSetLayout inline_sets[] = {H(VkDescriptorSetLayout, 51), H(VkDescriptorSetLayout, 52),
                                           H(VkDescriptorSetLayout, 53)};
    inline_info.setLayoutCount = 3;
    inline_info.pSetLayouts = inline_sets;
    push_info.layout = VK_NULL_HANDLE;
    ((PFN_vkCmdPushDescriptorSetWithTemplate2KHR)vkrt_replay_proc("vkCmdPushDescriptorSetWithTemplate2KHR"))(source.cmd,
                                                                                                             &push_info);
    CHECK(pushes == 5 && inline_creates == 1 && inline_destroys == 1 && vkrt_replay_journal_count(source.replay.journal) == 1);
    destroy(device.device, handle, &allocator);
    memset(original, 0, sizeof(original));
    memset(inline_sets, 0, sizeof(inline_sets));
    replaying = true;
    CHECK(vkrt_replay_emit(source.replay.journal, target.cmd, resolve, &target, NULL) == VK_SUCCESS && pushes == 6);
    replaying = false;
    vkrt_replay_journal_reset(source.replay.journal);
    PFN_vkrt_capture_CmdPushDescriptorSetWithTemplate2KHR capture_template =
        (void *)vkrt_replay_capture_proc("vkCmdPushDescriptorSetWithTemplate2KHR");
    push_info.pData = (void *)(uintptr_t)1;
    unsigned before_lookups = lookups;
    CHECK(capture_template(&source.replay, &push_info) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(lookups == before_lookups && pushes == 6 &&
          vkrt_replay_journal_status(source.replay.journal) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_replay_journal_reset(source.replay.journal);
    for (unsigned fail = 0; fail < 10; ++fail) {
        owner = (struct allocation_state){.fail_after = fail};
        VkResult result = create(device.device, &info, &allocator, &handle);
        CHECK(result == VK_SUCCESS || result == VK_ERROR_OUT_OF_HOST_MEMORY);
        if (result == VK_SUCCESS) destroy(device.device, handle, &allocator);
        CHECK(!owner.live);
    }
    device.virtual_rt = false;
    fill_data();
    CHECK(create(device.device, &info, NULL, &handle) == VK_SUCCESS && handle == H(VkDescriptorUpdateTemplate, 900));
    update_template(device.device, H(VkDescriptorSet, 901), handle, original);
    destroy(device.device, handle, NULL);
    CHECK(native_creates == 1 && native_destroys == 1 && native_updates == 2);
    device.virtual_rt = true;
    owner = (struct allocation_state){.fail_after = UINT32_MAX};
    CHECK(create(device.device, &info, &allocator, &handle) == VK_SUCCESS);
    vkrt_descriptor_template_finish(&device);
    CHECK(!owner.live);
    vkrt_replay_journal_destroy(source.replay.journal);
    puts(
        "descriptor template host: owned metadata/unaligned strides/AS+inline payloads/capture "
        "normalization/destroy-before-replay/void update/no-native passthrough/OOM passed");
    return 0;
}
