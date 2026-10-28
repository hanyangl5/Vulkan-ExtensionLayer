/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_rtp.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_execution_replay.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
VkResult vkrt_descriptor_template_capture(struct vkrt_replay_context *context, const VkPushDescriptorSetWithTemplateInfo *info) {
    (void)context;
    (void)info;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult vkrt_query_command_reset(struct vkrt_cmd_buffer_data *cb) {
    (void)cb;
    return VK_SUCCESS;
}
#define H(type, value) ((type)(uintptr_t)(value))
#define CHECK(value)                                            \
    do {                                                        \
        if (!(value)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                         \
        }                                                       \
    } while (0)
static unsigned failures, calls, creates, destroys, captures;
static unsigned pool_creates, pool_destroys, source_allocations, source_frees, executes;
static VkResult pool_tracking_result;
static bool pool_tracked, recycle_pool_on_destroy;
static uint32_t tracked_family;
static VkCommandPoolCreateFlags tracked_flags;
static VkCommandPoolCreateInfo recycled_pool_info;
static VkResult recycled_pool_result;
static VkCommandPool recycled_pool;
static const VkCommandBuffer command = H(VkCommandBuffer, 1);
static const VkPipelineLayout public_layout = H(VkPipelineLayout, 2), native_layout = H(VkPipelineLayout, 3);
static const VkPushConstantsInfo *passthrough;
static struct vkrt_device_data dev;
static struct vkrt_cmd_buffer_data cb;
bool vkrt_execution_command_requires_replay(const struct vkrt_cmd_buffer_data *data) {
    return data && !data->replaying && (data->has_virtual_queries || data->has_rtp_execution);
}
VkResult vkrt_execution_track_command_pool(struct vkrt_device_data *data, VkCommandPool pool, uint32_t family,
                                           VkCommandPoolCreateFlags flags) {
    CHECK(data == &dev && pool == H(VkCommandPool, 18));
    if (!data->executions) return VK_SUCCESS;
    if (pool_tracking_result != VK_SUCCESS) return pool_tracking_result;
    if (pool_tracked) return tracked_family == family && tracked_flags == flags ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    pool_tracked = true;
    tracked_family = family;
    tracked_flags = flags;
    return VK_SUCCESS;
}
void vkrt_execution_forget_command_pool(struct vkrt_device_data *data, VkCommandPool pool) {
    CHECK(data == &dev && pool == H(VkCommandPool, 18));
    if (data->executions) pool_tracked = false;
}
bool vkrt_query_record_children(struct vkrt_cmd_buffer_data *data, uint32_t count, const VkCommandBuffer *children) {
    CHECK(data == &cb && count == 2 && children[0] == children[1]);
    return true;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_pool(VkDevice device, const VkCommandPoolCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkCommandPool *out) {
    CHECK(device == dev.device && info->sType == VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO && !allocator);
    *out = H(VkCommandPool, 18);
    ++pool_creates;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator) {
    CHECK(device == dev.device && pool == H(VkCommandPool, 18) && !allocator);
    ++pool_destroys;
    if (recycle_pool_on_destroy) {
        /* Simulate another host thread creating a new native pool after the
         * old native handle has been released, before Destroy returns. */
        recycle_pool_on_destroy = false;
        pool_tracking_result = VK_SUCCESS;
        PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)vkrt_command_proc("vkCreateCommandPool");
        recycled_pool_result = create(device, &recycled_pool_info, NULL, &recycled_pool);
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_sources(VkDevice device, const VkCommandBufferAllocateInfo *info,
                                                       VkCommandBuffer *out) {
    CHECK(device == dev.device && info->commandPool == H(VkCommandPool, 18) && info->commandBufferCount == 1);
    *out = H(VkCommandBuffer, 19);
    ++source_allocations;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL free_sources(VkDevice device, VkCommandPool pool, uint32_t count,
                                               const VkCommandBuffer *commands) {
    CHECK(device == dev.device && pool == H(VkCommandPool, 18) && count == 1 && commands[0] == H(VkCommandBuffer, 19));
    ++source_frees;
}
static VKAPI_ATTR void VKAPI_CALL execute_sources(VkCommandBuffer parent, uint32_t count, const VkCommandBuffer *children) {
    CHECK(parent == command && count == 2 && children[0] == H(VkCommandBuffer, 20) && children[0] == children[1]);
    ++executes;
}
struct observed_push {
    uint32_t bank, offset, value;
    VkShaderStageFlags stages;
};
static struct observed_push observed[16];
static unsigned shader_binds, pipeline_binds;
static VkShaderEXT observed_compute_shader;
static VkPipeline observed_compute_pipeline;
static VKAPI_ATTR void VKAPI_CALL bind_shader(VkCommandBuffer cmd, uint32_t count, const VkShaderStageFlagBits *stages,
                                              const VkShaderEXT *shaders) {
    CHECK(cmd == command);
    ++shader_binds;
    for (uint32_t i = 0; i < count; ++i)
        if (stages[i] == VK_SHADER_STAGE_COMPUTE_BIT) observed_compute_shader = shaders ? shaders[i] : VK_NULL_HANDLE;
}
static VKAPI_ATTR void VKAPI_CALL bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    CHECK(cmd == command);
    if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        ++pipeline_binds;
        observed_compute_pipeline = pipeline;
    }
}
static void observe(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                    const void *values, uint32_t bank) {
    CHECK(cmd == command && layout == native_layout && size == 4 && calls < 16);
    observed[calls++] = (struct observed_push){bank, offset, *(const uint32_t *)values, stages};
}
static VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                       uint32_t size, const void *values) {
    observe(cmd, layout, stages, offset, size, values, 0);
}
static VKAPI_ATTR void VKAPI_CALL push2(VkCommandBuffer cmd, const VkPushConstantsInfo *info) {
    if (passthrough) {
        CHECK(info == passthrough && cmd == command);
        ++calls;
        return;
    }
    const VkPushConstantBankInfoNV *bank = info->pNext;
    CHECK(bank && bank->sType == VK_STRUCTURE_TYPE_PUSH_CONSTANT_BANK_INFO_NV && !bank->pNext);
    observe(cmd, info->layout, info->stageFlags, info->offset, info->size, info->pValues, bank->bank);
}
static VKAPI_ATTR VkResult VKAPI_CALL make_layout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkPipelineLayout *out) {
    CHECK(device == dev.device && !allocator && !info->pNext && info->pushConstantRangeCount == 1);
    CHECK(info->pPushConstantRanges[0].size == 16 && info->pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_ALL);
    ++creates;
    *out = public_layout;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *allocator) {
    CHECK(device == dev.device && layout == public_layout && !allocator && calls);
    ++destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL reset_native(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
    CHECK(cmd == command && !flags);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL begin_native(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info) {
    CHECK(cmd == command && info->flags == VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    CHECK(info->pInheritanceInfo == H(const VkCommandBufferInheritanceInfo *, 1));
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL end_native(VkCommandBuffer cmd) {
    CHECK(cmd == command);
    return VK_SUCCESS;
}
void vkrt_rtp_reset_command(struct vkrt_cmd_buffer_data *data) { CHECK(data && data->device_data == &dev); }
bool vkrt_rtp_bind_pipeline(struct vkrt_cmd_buffer_data *data, VkPipelineBindPoint point, VkPipeline pipeline) {
    (void)data;
    (void)point;
    (void)pipeline;
    return false;
}
VkShaderStageFlags vkrt_rtp_push(struct vkrt_cmd_buffer_data *data, VkPipelineLayout layout, VkShaderStageFlags stages,
                                 uint32_t offset, uint32_t size, const void *values) {
    (void)offset;
    (void)size;
    (void)values;
    CHECK(data == &cb && layout == public_layout);
    ++captures;
    return stages == VK_SHADER_STAGE_ALL ? stages : stages & ~VKRT_RTP_SHADER_STAGES;
}
VkPipelineLayout vkrt_rtp_native_layout(struct vkrt_device_data *data, VkPipelineLayout layout) {
    CHECK(data == &dev && layout == public_layout);
    return native_layout;
}
VkPipelineLayout vkrt_rtp_command_native_layout(struct vkrt_cmd_buffer_data *data, VkPipelineLayout layout) {
    CHECK(data == &cb);
    return vkrt_rtp_native_layout(data->device_data, layout);
}
VkResult vkrt_rtp_replay_layout_capture(struct vkrt_cmd_buffer_data *data, VkPipelineLayout layout, VkPipelineLayout *identity) {
    CHECK(data == &cb && layout == public_layout);
    *identity = public_layout;
    return VK_SUCCESS;
}
VkResult vkrt_rtp_replay_layout_native(struct vkrt_cmd_buffer_data *data, VkPipelineLayout identity, VkPipelineLayout *native) {
    CHECK(data == &cb && identity == public_layout);
    *native = native_layout;
    return VK_SUCCESS;
}
PFN_vkVoidFunction vkrt_device_direct_proc(struct vkrt_device_data *data, const char *name) {
    CHECK(data == &dev);
    return vkrt_command_proc(name);
}
PFN_vkVoidFunction vkrt_rtp_proc(const char *name) {
    if (!strcmp(name, "vkCreatePipelineLayout")) return (PFN_vkVoidFunction)make_layout;
    if (!strcmp(name, "vkDestroyPipelineLayout")) return (PFN_vkVoidFunction)destroy_layout;
    return NULL;
}

static void test_command_pool_reuse(const VkCommandPoolCreateInfo *info) {
    PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)vkrt_command_proc("vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)vkrt_command_proc("vkDestroyCommandPool");
    dev.executions = (struct vkrt_execution_device *)(uintptr_t)1;
    pool_tracking_result = VK_SUCCESS;
    for (unsigned different = 0; different < 2; ++different) {
        VkCommandPool old_pool;
        CHECK(create(dev.device, info, NULL, &old_pool) == VK_SUCCESS && pool_tracked);
        recycled_pool_info = *info;
        if (different) {
            recycled_pool_info.queueFamilyIndex = 11;
            recycled_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        }
        recycle_pool_on_destroy = true;
        recycled_pool_result = VK_ERROR_UNKNOWN;
        recycled_pool = VK_NULL_HANDLE;
        destroy(dev.device, old_pool, NULL);
        CHECK(!recycle_pool_on_destroy && recycled_pool_result == VK_SUCCESS && recycled_pool == old_pool);
        CHECK(pool_tracked && tracked_family == recycled_pool_info.queueFamilyIndex && tracked_flags == recycled_pool_info.flags);
        if (recycled_pool) destroy(dev.device, recycled_pool, NULL);
        CHECK(!pool_tracked);
    }
    /* A failed track never owned a metadata entry. Its native rollback must
     * not forget the replacement pool created during that native Destroy. */
    pool_tracking_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    recycled_pool_info = *info;
    recycled_pool_info.queueFamilyIndex = 13;
    recycled_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    recycle_pool_on_destroy = true;
    VkCommandPool failed_pool = H(VkCommandPool, 99);
    CHECK(create(dev.device, info, NULL, &failed_pool) == VK_ERROR_OUT_OF_HOST_MEMORY && !failed_pool);
    CHECK(recycled_pool_result == VK_SUCCESS && pool_tracked && tracked_family == recycled_pool_info.queueFamilyIndex &&
          tracked_flags == recycled_pool_info.flags);
    if (recycled_pool) destroy(dev.device, recycled_pool, NULL);
    CHECK(!pool_tracked);
    dev.executions = NULL;
}

int main(void) {
    dev.device = H(VkDevice, 4);
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    dev.rtp = (struct vkrt_rtp_state *)(uintptr_t)1;
    dev.vtable.CmdPushConstants = push;
    dev.vtable.CmdPushConstants2 = push2;
    dev.vtable.CmdBindPipeline = bind_pipeline;
    dev.vtable.CmdBindShadersEXT = bind_shader;
    dev.vtable.ResetCommandBuffer = reset_native;
    dev.vtable.BeginCommandBuffer = begin_native;
    dev.vtable.EndCommandBuffer = end_native;
    dev.vtable.CreateCommandPool = create_pool;
    dev.vtable.DestroyCommandPool = destroy_pool;
    dev.vtable.AllocateCommandBuffers = allocate_sources;
    dev.vtable.FreeCommandBuffers = free_sources;
    dev.vtable.CmdExecuteCommands = execute_sources;
    vkrt_mutex_init(&dev.command_buffers_lock);
    cb.cmd = command;
    cb.device_data = &dev;
    cb.record_result = VK_SUCCESS;
    vkrt_map_object(VKRT_HKEY(command), &cb);
    PFN_vkCmdPushConstants2 command2 = (PFN_vkCmdPushConstants2)vkrt_command_proc("vkCmdPushConstants2");
    CHECK(command2 && (PFN_vkVoidFunction)command2 == vkrt_command_proc("vkCmdPushConstants2KHR"));
    PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)vkrt_command_proc("vkResetCommandBuffer");

    uint32_t value = 11;
    VkPushConstantsInfo info = {
        VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO, NULL, public_layout, VK_SHADER_STAGE_COMPUTE_BIT, 4, 4, &value};
    command2(command, &info);
    value = 22;
    VkPushConstantBankInfoNV bank = {VK_STRUCTURE_TYPE_PUSH_CONSTANT_BANK_INFO_NV, NULL, 2};
    VkPushConstantRange range = {VK_SHADER_STAGE_ALL, 0, 16};
    VkPipelineLayoutCreateInfo inline_layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, &bank, 0, 0, NULL, 1, &range};
    info.pNext = &inline_layout;
    info.layout = VK_NULL_HANDLE;
    info.stageFlags = VK_SHADER_STAGE_ALL;
    command2(command, &info);
    CHECK(creates == 1 && destroys == 1 && captures == 2 && calls == 2);
    CHECK(info.layout == VK_NULL_HANDLE && info.pNext == &inline_layout && inline_layout.pNext == &bank);
    CHECK(observed[0].value == 11 && observed[0].bank == 0 && observed[1].value == 22 && observed[1].bank == 2);

    value = 99;
    calls = 0;
    vkrt_replay_push_constants(&cb);
    CHECK(calls == 2 && observed[0].value == 11 && observed[1].value == 22);
    CHECK(observed[0].stages == VK_SHADER_STAGE_COMPUTE_BIT && observed[1].stages == VK_SHADER_STAGE_ALL);
    CHECK(reset(command, 0) == VK_SUCCESS && !cb.push_head && !cb.push_tail);

    info.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    calls = creates = 0;
    command2(command, &info);
    CHECK(cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT && !calls && !creates && !cb.push_head);
    CHECK(reset(command, 0) == VK_SUCCESS);

    PFN_vkCmdBindShadersEXT shaders = (PFN_vkCmdBindShadersEXT)vkrt_command_proc("vkCmdBindShadersEXT");
    PFN_vkCmdBindPipeline pipeline = (PFN_vkCmdBindPipeline)vkrt_command_proc("vkCmdBindPipeline");
    const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    VkShaderEXT objects[] = {H(VkShaderEXT, 71), H(VkShaderEXT, 72)};
    pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, H(VkPipeline, 73));
    shaders(command, 2, stages, objects);
    CHECK(cb.compute_shader_object && cb.bound_compute_shader == objects[1] && !cb.bound_compute_pipeline);
    objects[1] = H(VkShaderEXT, 74);
    vkrt_replay_compute_binding(&cb);
    CHECK(shader_binds == 2 && observed_compute_shader == H(VkShaderEXT, 72));
    shaders(command, 1, stages, NULL);  // Graphics-only updates leave compute state intact.
    vkrt_replay_compute_binding(&cb);
    CHECK(observed_compute_shader == H(VkShaderEXT, 72));
    shaders(command, 1, &stages[1], NULL);
    vkrt_replay_compute_binding(&cb);
    CHECK(cb.compute_shader_object && observed_compute_shader == VK_NULL_HANDLE);
    pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, H(VkPipeline, 75));
    vkrt_replay_compute_binding(&cb);
    CHECK(!cb.compute_shader_object && observed_compute_pipeline == H(VkPipeline, 75) && pipeline_binds == 3);
    CHECK(reset(command, 0) == VK_SUCCESS && !cb.compute_shader_object && !cb.bound_compute_shader && !cb.bound_compute_pipeline);

    /* An unrelated ordinary device must preserve even an unknown native chain,
     * with no private layout, state allocation or RT capture. */
    dev.rtp = NULL;
    const VkBaseInStructure unknown = {(VkStructureType)0x7ffffe00, NULL};
    info.pNext = &unknown;
    info.layout = public_layout;
    info.stageFlags = VK_SHADER_STAGE_ALL;
    passthrough = &info;
    unsigned previous_captures = captures;
    command2(command, &info);
    CHECK(calls == 1 && captures == previous_captures && !cb.push_head && cb.record_result == VK_SUCCESS);
    // Exercise the production lookup/context/lifecycle integration, including
    // immutable input capture before the translating push hook is invoked.
    passthrough = NULL;
    dev.rtp = (struct vkrt_rtp_state *)(uintptr_t)1;
    cb.replay.journal = vkrt_replay_journal_create(NULL, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    CHECK(cb.replay.journal);
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
                                            .pInheritanceInfo = H(const VkCommandBufferInheritanceInfo *, 1)};
    PFN_vkBeginCommandBuffer begin_command = (PFN_vkBeginCommandBuffer)vkrt_command_proc("vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end_command = (PFN_vkEndCommandBuffer)vkrt_command_proc("vkEndCommandBuffer");
    CHECK(begin_command(command, &begin) == VK_SUCCESS);
    CHECK(!vkrt_replay_journal_begin_info(cb.replay.journal)->pInheritanceInfo);
    value = 123;
    calls = 0;
    ((PFN_vkCmdPushConstants)vkrt_replay_proc("vkCmdPushConstants"))(command, public_layout, VK_SHADER_STAGE_COMPUTE_BIT, 4,
                                                                     sizeof(value), &value);
    CHECK(end_command(command) == VK_SUCCESS && calls == 1 && observed[0].value == 123);
    CHECK(vkrt_replay_journal_status(cb.replay.journal) == VK_SUCCESS && vkrt_replay_journal_count(cb.replay.journal) == 1);
    value = 999;
    struct vkrt_replay_journal *recording = cb.replay.journal;
    cb.replay.journal = NULL;
    cb.replaying = true;
    struct vkrt_replay_context *context = vkrt_replay_lookup_context(command);
    CHECK(vkrt_replay_emit(recording, command, context->resolve, context->user, NULL) == VK_SUCCESS);
    CHECK(cb.record_result == VK_SUCCESS && calls == 2 && observed[1].value == 123);
    CHECK(vkrt_replay_journal_count(recording) == 1);
    cb.replaying = false;
    cb.replay.journal = recording;
    CHECK(reset(command, 0) == VK_SUCCESS && !vkrt_replay_journal_count(recording) && !cb.push_head);
    /* RTP-only recordings make journal capture failures observable at End,
     * while an unrelated ordinary recording preserves the native result. */
    VkBaseInStructure unsupported = {.sType = (VkStructureType)0x7ffffe00};
    VkCommandBufferBeginInfo unsupported_begin = begin;
    unsupported_begin.pNext = &unsupported;
    CHECK(begin_command(command, &unsupported_begin) == VK_SUCCESS);
    CHECK(vkrt_replay_journal_status(recording) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(end_command(command) == VK_SUCCESS);
    CHECK(begin_command(command, &unsupported_begin) == VK_SUCCESS);
    cb.has_rtp_execution = true;
    CHECK(!cb.has_virtual_queries && end_command(command) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(reset(command, 0) == VK_SUCCESS && !cb.has_rtp_execution);
    CHECK(begin_command(command, &begin) == VK_SUCCESS);
    struct vkrt_cmd_buffer_data child = {
        .cmd = H(VkCommandBuffer, 20), .device_data = &dev, .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .has_rtp_execution = true};
    vkrt_map_object(VKRT_HKEY(child.cmd), &child);
    VkCommandBuffer children[] = {child.cmd, child.cmd};
    cb.native_active_queries = 1;
    ((PFN_vkCmdExecuteCommands)vkrt_replay_proc("vkCmdExecuteCommands"))(command, 2, children);
    CHECK(executes == 1 && cb.has_rtp_execution && !cb.has_virtual_queries && cb.record_result == VK_SUCCESS);
    cb.native_active_queries = 0;
    CHECK(end_command(command) == VK_SUCCESS && vkrt_replay_journal_count(recording) == 1);
    CHECK(reset(command, 0) == VK_SUCCESS && !cb.has_rtp_execution);
    vkrt_unmap_object(VKRT_HKEY(child.cmd));
    vkrt_replay_journal_destroy(recording);
    cb.replay.journal = NULL;
    cb.has_rtp_execution = true;
    CHECK(end_command(command) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(reset(command, 0) == VK_SUCCESS && !cb.has_rtp_execution);
    PFN_vkCreateCommandPool create_command_pool = (PFN_vkCreateCommandPool)vkrt_command_proc("vkCreateCommandPool");
    PFN_vkDestroyCommandPool destroy_command_pool = (PFN_vkDestroyCommandPool)vkrt_command_proc("vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)vkrt_command_proc("vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers free_commands = (PFN_vkFreeCommandBuffers)vkrt_command_proc("vkFreeCommandBuffers");
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = 7};
    VkCommandBufferAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    dev.virtual_rt = true;
    dev.executions = (struct vkrt_execution_device *)(uintptr_t)1;
    CHECK(!dev.queries && !dev.query_backend_enabled);
    pool_tracking_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(create_command_pool(dev.device, &pool_info, NULL, &allocation.commandPool) == VK_ERROR_OUT_OF_HOST_MEMORY &&
          !allocation.commandPool && pool_creates == 1 && pool_destroys == 1);
    pool_tracking_result = VK_SUCCESS;
    CHECK(create_command_pool(dev.device, &pool_info, NULL, &allocation.commandPool) == VK_SUCCESS);
    VkCommandBuffer allocated;
    CHECK(allocate(dev.device, &allocation, &allocated) == VK_SUCCESS);
    struct vkrt_cmd_buffer_data *state = VKRT_FIND(struct vkrt_cmd_buffer_data, allocated);
    CHECK(state && state->replay.journal && !state->has_virtual_queries && !state->has_rtp_execution);
    free_commands(dev.device, allocation.commandPool, 1, &allocated);
    destroy_command_pool(dev.device, allocation.commandPool, NULL);
    dev.executions = NULL;
    dev.virtual_rt = false;
    dev.shader_object_backend = true;
    pool_tracking_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(create_command_pool(dev.device, &pool_info, NULL, &allocation.commandPool) == VK_SUCCESS);
    CHECK(allocate(dev.device, &allocation, &allocated) == VK_SUCCESS);
    state = VKRT_FIND(struct vkrt_cmd_buffer_data, allocated);
    CHECK(state && !state->replay.journal);
    free_commands(dev.device, allocation.commandPool, 1, &allocated);
    destroy_command_pool(dev.device, allocation.commandPool, NULL);
    CHECK(source_allocations == 2 && source_frees == 2 && pool_creates == 3 && pool_destroys == 3);
    test_command_pool_reuse(&pool_info);
    vkrt_unmap_object(VKRT_HKEY(command));
    printf("Maintenance6 push snapshots/inline layouts/bank replay/ordinary passthrough/pool handle reuse %s\n",
           failures ? "FAIL" : "PASS");
    return failures != 0;
}
