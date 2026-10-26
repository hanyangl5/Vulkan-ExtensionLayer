/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_rtp.h"
#include "vkrt_rt_capabilities.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
VkResult vkrt_query_command_reset(struct vkrt_cmd_buffer_data *cb) {
    (void)cb;
    return VK_SUCCESS;
}
void vkrt_query_forget_command_pool(struct vkrt_device_data *dev, VkCommandPool pool) {
    (void)dev;
    (void)pool;
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
static const VkCommandBuffer command = H(VkCommandBuffer, 1);
static const VkPipelineLayout public_layout = H(VkPipelineLayout, 2), native_layout = H(VkPipelineLayout, 3);
static const VkPushConstantsInfo *passthrough;
static struct vkrt_device_data dev;
static struct vkrt_cmd_buffer_data cb;
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
void vkrt_rtp_reset_command(struct vkrt_cmd_buffer_data *data) { CHECK(data == &cb); }
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
PFN_vkVoidFunction vkrt_rtp_proc(const char *name) {
    if (!strcmp(name, "vkCreatePipelineLayout")) return (PFN_vkVoidFunction)make_layout;
    if (!strcmp(name, "vkDestroyPipelineLayout")) return (PFN_vkVoidFunction)destroy_layout;
    return NULL;
}

int main(void) {
    dev.device = H(VkDevice, 4);
    dev.rtp = (struct vkrt_rtp_state *)(uintptr_t)1;
    dev.vtable.CmdPushConstants = push;
    dev.vtable.CmdPushConstants2 = push2;
    dev.vtable.CmdBindPipeline = bind_pipeline;
    dev.vtable.CmdBindShadersEXT = bind_shader;
    dev.vtable.ResetCommandBuffer = reset_native;
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
    vkrt_unmap_object(VKRT_HKEY(command));
    printf("Maintenance6 push snapshots/inline layouts/bank replay/ordinary passthrough %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
