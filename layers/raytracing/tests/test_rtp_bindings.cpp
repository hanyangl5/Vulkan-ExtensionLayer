/* SPDX-License-Identifier: MIT
 * Public command entry points against a native spy. These checks exercise
 * dynamic maintenance6 layouts on machines without that optional NV feature.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
extern "C" {
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"
#include "vkrt_deferred.h"
#include "vkrt_rtp.h"
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
}
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <array>
#include <map>
#include <set>
#include <vector>
#include <cstdio>
#include <cstring>

namespace {
unsigned failures = 0, forwarded = 0, create_attempts = 0, fail_at = 0;
#define CHECK(value)                                                 \
    do {                                                             \
        if (!(value)) {                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                              \
        }                                                            \
    } while (0)
template <class H>
H Handle(uintptr_t value) {
    return (H)value;
}
template <class T>
std::array<unsigned char, sizeof(T)> Snapshot(const T &value) {
    std::array<unsigned char, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}
const VkDevice device = Handle<VkDevice>(1);
const VkCommandBuffer command = Handle<VkCommandBuffer>(2);
const VkPhysicalDevice physical = Handle<VkPhysicalDevice>(3);
struct NativeLayout {
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> pushes;
};
std::map<VkPipelineLayout, NativeLayout> layouts;
std::set<VkPipelineLayout> destroyed;
uintptr_t next_layout = 100;
VkShaderStageFlags expected_stages = 0;
const VkBindDescriptorSetsInfo *passthrough = nullptr;
VkBindDescriptorSetsInfo observed{};
std::vector<VkDescriptorSet> observed_sets;
std::vector<uint32_t> observed_offsets;
vkrt_device_data dev{};
vkrt_instance_data instance{};
vkrt_cmd_buffer_data cb{};
uintptr_t next_native = 10000;
unsigned native_creates = 0, fail_native_at = 0;
std::set<VkShaderModule> native_shaders;
std::set<VkPipeline> native_pipelines;
std::vector<VkPipelineCreateFlags> native_flags;
std::vector<VkPipelineCache> native_caches;
VKAPI_ATTR VkResult VKAPI_CALL CreateNativeShader(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                  VkShaderModule *out) {
    CHECK(d == device && !a && ci->codeSize && ci->pCode);
    *out = Handle<VkShaderModule>(next_native++);
    CHECK(native_shaders.insert(*out).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyNativeShader(VkDevice d, VkShaderModule shader, const VkAllocationCallbacks *a) {
    CHECK(d == device && !a && native_shaders.erase(shader) == 1);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateNativePipeline(VkDevice d, VkPipelineCache cache, uint32_t count,
                                                    const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                    VkPipeline *out) {
    CHECK(d == device && count == 1 && !a);
    CHECK(!(ci->flags & ~VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) && !ci->basePipelineHandle);
    CHECK(ci->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && native_shaders.count(ci->stage.module));
    CHECK(layouts.count(ci->layout) && layouts.at(ci->layout).pushes.size() == 1 && layouts.at(ci->layout).pushes[0].size == 8);
    *out = VK_NULL_HANDLE;
    native_flags.push_back(ci->flags);
    native_caches.push_back(cache);
    if (++native_creates == fail_native_at) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = Handle<VkPipeline>(next_native++);
    CHECK(native_pipelines.insert(*out).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyNativePipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks *a) {
    CHECK(d == device && !a && native_pipelines.erase(pipeline) == 1);
}
VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *out) {
    CHECK(p == physical);
    *out = {};
    out->limits.maxPushConstantsSize = 256;
    out->limits.maxComputeWorkGroupCount[0] = 65535;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci,
                                            const VkAllocationCallbacks *allocator, VkPipelineLayout *out) {
    CHECK(d == device && !allocator && !ci->pNext);
    *out = VK_NULL_HANDLE;
    if (++create_attempts == fail_at) return VK_ERROR_OUT_OF_HOST_MEMORY;
    NativeLayout value;
    if (ci->setLayoutCount) value.sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
    if (ci->pushConstantRangeCount)
        value.pushes.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
    for (auto range : value.pushes) CHECK(range.stageFlags == VK_SHADER_STAGE_COMPUTE_BIT);
    *out = Handle<VkPipelineLayout>(next_layout++);
    CHECK(layouts.emplace(*out, std::move(value)).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice d, VkPipelineLayout layout, const VkAllocationCallbacks *allocator) {
    CHECK(d == device && !allocator && layouts.erase(layout) == 1);
    CHECK(destroyed.insert(layout).second);
}
VKAPI_ATTR void VKAPI_CALL Bind2(VkCommandBuffer c, const VkBindDescriptorSetsInfo *info) {
    CHECK(c == command);
    ++forwarded;
    if (passthrough) {
        CHECK(info == passthrough);
        return;
    }
    CHECK(info->sType == VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO && !info->pNext);
    CHECK(info->layout && layouts.count(info->layout));
    CHECK(info->stageFlags == expected_stages);
    const auto &layout = layouts.at(info->layout);
    CHECK(layout.pushes.size() == 1 && layout.pushes[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT && layout.pushes[0].offset == 0 &&
          layout.pushes[0].size == 256);
    observed = *info;
    observed_sets.assign(info->pDescriptorSets, info->pDescriptorSets + info->descriptorSetCount);
    observed_offsets.assign(info->pDynamicOffsets, info->pDynamicOffsets + info->dynamicOffsetCount);
}
void Reset() {
    vkrt_rtp_reset_command(&cb);
    CHECK(layouts.empty());
    cb.record_result = VK_SUCCESS;
}
void PipelineDerivatives() {
    auto make_layout = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy_layout = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    auto create = (PFN_vkCreateRayTracingPipelinesKHR)vkrt_rtp_proc("vkCreateRayTracingPipelinesKHR");
    auto destroy = (PFN_vkDestroyPipeline)vkrt_rtp_proc("vkDestroyPipeline");
    auto handles = (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkrt_rtp_proc("vkGetRayTracingShaderGroupHandlesKHR");
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main"
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::vector<uint32_t> words;
    CHECK(tools.Assemble(source, &words) && tools.Validate(words));
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = words.size() * sizeof(uint32_t);
    shader_ci.pCode = words.data();
    VkShaderModule shader{};
    bool handled = false;
    CHECK(vkrt_rtp_create_shader(&dev, &shader_ci, nullptr, &shader, &handled) == VK_SUCCESS && handled);
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    CHECK(make_layout(device, &layout_ci, nullptr, &layout) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.module = shader;
    stage.pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR ci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    ci.stageCount = 1;
    ci.pStages = &stage;
    ci.groupCount = 1;
    ci.pGroups = &group;
    ci.maxPipelineRayRecursionDepth = 1;
    ci.layout = layout;
    ci.basePipelineIndex = -1;
    VkRayTracingPipelineCreateInfoKHR batch[] = {ci, ci};
    batch[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    batch[1].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    batch[1].basePipelineIndex = 0;
    const auto saved_batch = Snapshot(batch);
    VkPipeline pipelines[3]{};
    native_flags.clear();
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 2, batch, nullptr, pipelines) == VK_SUCCESS);
    CHECK(!std::memcmp(batch, saved_batch.data(), sizeof(batch)));
    ci.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    ci.basePipelineHandle = pipelines[1];
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines[2]) == VK_SUCCESS);
    CHECK(native_flags == std::vector<VkPipelineCreateFlags>({VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT, 0, 0}));
    destroy(device, pipelines[0], nullptr);
    CHECK(native_pipelines.size() == 2);
    std::array<uint8_t, 32> first{}, second{};
    CHECK(handles(device, pipelines[1], 0, 1, first.size(), first.data()) == VK_SUCCESS);
    CHECK(handles(device, pipelines[2], 0, 1, second.size(), second.data()) == VK_SUCCESS && first != second);
    destroy(device, pipelines[1], nullptr);
    destroy(device, pipelines[2], nullptr);
    CHECK(native_pipelines.empty() && native_shaders.empty());
    // Base fields are ignored unless DERIVATIVE is set. A real ordinary
    // pipeline still cannot be used as a base without ALLOW_DERIVATIVES.
    ci.flags = 0;
    ci.basePipelineHandle = Handle<VkPipeline>(77);
    ci.basePipelineIndex = 123;
    VkPipeline ordinary{};
    const auto cache = Handle<VkPipelineCache>(50000);
    CHECK(create(device, VK_NULL_HANDLE, cache, 1, &ci, nullptr, &ordinary) == VK_SUCCESS && native_caches.back() == cache);
    ci.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    ci.basePipelineHandle = ordinary;
    ci.basePipelineIndex = -1;
    VkPipeline rejected_base{};
    unsigned calls = native_creates;
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &rejected_base) == VK_ERROR_FEATURE_NOT_PRESENT &&
          !rejected_base && native_creates == calls);
    destroy(device, ordinary, nullptr);
    // Invalid provenance is rejected before native compilation. These are host
    // contract tests; native validation never receives malformed create infos.
    unsigned before = native_creates;
    for (int32_t index : {-2, 0, 1}) {
        ci.basePipelineHandle = VK_NULL_HANDLE;
        ci.basePipelineIndex = index;
        VkPipeline rejected = Handle<VkPipeline>(77);
        CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
              !rejected && native_creates == before);
    }
    ci.basePipelineIndex = -1;
    for (auto flag : {VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT,
                      VK_PIPELINE_CREATE_RAY_TRACING_SHADER_GROUP_HANDLE_CAPTURE_REPLAY_BIT_KHR}) {
        ci.flags = flag;
        VkPipeline rejected{};
        CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
              !rejected && native_creates == before);
    }
    // A failed earlier base must never become an invalid native base handle;
    // independent later elements still return their successful objects.
    VkRayTracingPipelineCreateInfoKHR partial[] = {batch[0], batch[1], batch[0]};
    fail_native_at = native_creates + 1;
    VkPipeline result[3]{};
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 3, partial, nullptr, result) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(!result[0] && !result[1] && result[2] && native_pipelines.size() == 1 && native_shaders.empty());
    fail_native_at = 0;
    destroy(device, result[2], nullptr);
    CHECK(vkrt_rtp_destroy_shader(&dev, shader));
    destroy_layout(device, layout, nullptr);
    CHECK(layouts.empty() && native_pipelines.empty() && native_shaders.empty());
}
void DescriptorDefinitions() {
    auto make = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    const auto a = Handle<VkDescriptorSetLayout>(2000), b = Handle<VkDescriptorSetLayout>(2001);
    VkDescriptorSetLayoutBinding binding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    definition.bindingCount = 1;
    definition.pBindings = &binding;
    auto track = [&](VkDescriptorSetLayout handle) {
        CHECK(vkrt_rtp_track_descriptor_layout(&dev, handle, &definition) == VK_SUCCESS);
    };
    auto layout = [&](VkDescriptorSetLayout set, VkPipelineLayoutCreateFlags flags = 0) {
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.flags = flags;
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &set;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push;
        VkPipelineLayout result{};
        CHECK(make(device, &ci, nullptr, &result) == VK_SUCCESS);
        return result;
    };
    std::vector<VkPipelineLayout> owned;
    auto keep = [&](VkDescriptorSetLayout set, VkPipelineLayoutCreateFlags flags = 0) {
        owned.push_back(layout(set, flags));
        return owned.back();
    };
    track(a);
    track(b);
    auto first = keep(a), second = keep(b);
    CHECK(vkrt_rtp_layout_compatible(&dev, first, second, 0));
    // Creation inputs and DSL handles can both disappear or be reused while
    // pipeline layouts retain their immutable definitions.
    binding.descriptorCount = 8;
    vkrt_rtp_forget_descriptor_layout(&dev, a);
    track(a);
    auto changed = keep(a);
    vkrt_rtp_forget_descriptor_layout(&dev, b);
    CHECK(vkrt_rtp_layout_compatible(&dev, first, second, 0));
    CHECK(!vkrt_rtp_layout_compatible(&dev, first, changed, 0));
    binding.descriptorCount = 4;
    track(a);
    auto independent = keep(a, VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT);
    CHECK(!vkrt_rtp_layout_compatible(&dev, first, independent, 0));
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    binding_flags.bindingCount = 1;
    binding_flags.pBindingFlags = &flags;
    definition.pNext = &binding_flags;
    track(a);
    auto flagged = keep(a);
    flags = 0;
    track(b);
    auto unflagged = keep(b);
    CHECK(!vkrt_rtp_layout_compatible(&dev, flagged, unflagged, 0));
    CHECK(vkrt_rtp_layout_compatible(&dev, first, unflagged, 0));
    definition.pNext = nullptr;
    VkSampler sampler = Handle<VkSampler>(3000);
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binding.descriptorCount = 1;
    binding.pImmutableSamplers = &sampler;
    track(a);
    track(b);
    auto sampler_a = keep(a), sampler_b = keep(b);
    CHECK(vkrt_rtp_layout_compatible(&dev, sampler_a, sampler_b, 0));
    sampler = Handle<VkSampler>(3001);
    track(b);
    auto other_sampler = keep(b);
    CHECK(!vkrt_rtp_layout_compatible(&dev, sampler_a, other_sampler, 0));
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    VkMutableDescriptorTypeListEXT list{2, types};
    VkMutableDescriptorTypeCreateInfoEXT mutable_info{VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
    mutable_info.mutableDescriptorTypeListCount = 1;
    mutable_info.pMutableDescriptorTypeLists = &list;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
    binding.pImmutableSamplers = nullptr;
    definition.pNext = &mutable_info;
    track(a);
    track(b);
    auto mutable_a = keep(a), mutable_b = keep(b);
    CHECK(vkrt_rtp_layout_compatible(&dev, mutable_a, mutable_b, 0));
    types[1] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    track(b);
    auto other_mutable = keep(b);
    CHECK(!vkrt_rtp_layout_compatible(&dev, mutable_a, other_mutable, 0));
    for (auto handle : owned) destroy(device, handle, nullptr);
    vkrt_rtp_forget_descriptor_layout(&dev, a);
    vkrt_rtp_forget_descriptor_layout(&dev, b);
    CHECK(layouts.empty());
}
}  // namespace
extern "C" VkDeviceAddress vkrt_as_registry_address(vkrt_device_data *data) {
    CHECK(data == &dev);
    return 0;
}
extern "C" bool vkrt_deferred_valid(vkrt_device_data *data, VkDeferredOperationKHR value) {
    CHECK(data == &dev);
    return !value;
}
int main() {
    instance.vtable.GetPhysicalDeviceProperties = Properties;
    dev.device = device;
    dev.physical_device = physical;
    dev.instance_data = &instance;
    dev.vtable.CreatePipelineLayout = CreateLayout;
    dev.vtable.DestroyPipelineLayout = DestroyLayout;
    dev.vtable.CmdBindDescriptorSets2 = Bind2;
    dev.vtable.CreateShaderModule = CreateNativeShader;
    dev.vtable.DestroyShaderModule = DestroyNativeShader;
    dev.vtable.CreateComputePipelinesInternal = CreateNativePipeline;
    dev.vtable.DestroyPipeline = DestroyNativePipeline;
    cb.cmd = command;
    cb.device_data = &dev;
    vkrt_map_object(VKRT_HKEY(device), &dev);
    vkrt_map_object(VKRT_HKEY(command), &cb);
    CHECK(vkrt_rtp_init(&dev, nullptr) == VK_SUCCESS);
    DescriptorDefinitions();
    PipelineDerivatives();
    const std::string bank_source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability PushConstantBanksNV
OpExtension "SPV_KHR_ray_tracing"
OpExtension "SPV_NV_push_constant_bank"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %pc
OpMemberDecorate %Block 0 Offset 0
OpDecorate %Block Block
OpDecorate %pc BankNV 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%Block = OpTypeStruct %uint
%ptr = OpTypePointer PushConstant %Block
%pc = OpVariable %ptr PushConstant
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    for (bool raygen : {true, false}) {
        auto source = bank_source;
        if (!raygen) {
            for (const std::string &line :
                 {std::string("OpCapability RayTracingKHR\n"), std::string("OpExtension \"SPV_KHR_ray_tracing\"\n")})
                source.erase(source.find(line), line.size());
            const std::string entry = "OpEntryPoint RayGenerationKHR %main \"main\" %pc";
            source.replace(source.find(entry), entry.size(),
                           "OpEntryPoint GLCompute %main \"main\" %pc\nOpExecutionMode %main LocalSize 1 1 1");
        }
        std::vector<uint32_t> words;
        CHECK(tools.Assemble(source, &words) && tools.Validate(words));
        const auto saved = words;
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = words.size() * sizeof(uint32_t);
        shader_info.pCode = words.data();
        VkShaderModule shader = Handle<VkShaderModule>(99);
        bool handled = false;
        VkResult result = vkrt_rtp_create_shader(&dev, &shader_info, nullptr, &shader, &handled);
        CHECK(result == (raygen ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS));
        CHECK(handled == raygen && shader == (raygen ? VK_NULL_HANDLE : Handle<VkShaderModule>(99)) && words == saved);
    }
    auto core = (PFN_vkCmdBindDescriptorSets2)vkrt_rtp_proc("vkCmdBindDescriptorSets2");
    auto khr = (PFN_vkCmdBindDescriptorSets2)vkrt_rtp_proc("vkCmdBindDescriptorSets2KHR");
    CHECK(core && khr);
    const VkDescriptorSetLayout set_layouts[] = {Handle<VkDescriptorSetLayout>(40), Handle<VkDescriptorSetLayout>(41)};
    VkDescriptorSetLayoutCreateInfo empty_definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    for (auto handle : set_layouts) CHECK(vkrt_rtp_track_descriptor_layout(&dev, handle, &empty_definition) == VK_SUCCESS);
    const VkPushConstantRange ranges[] = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256}, {VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4}};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = set_layouts;
    ci.pushConstantRangeCount = 2;
    ci.pPushConstantRanges = ranges;
    const auto saved_ci = Snapshot(ci);
    const VkDescriptorSet sets[] = {Handle<VkDescriptorSet>(50), Handle<VkDescriptorSet>(51)};
    const uint32_t offsets[] = {256, 512};
    VkBindDescriptorSetsInfo info{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};
    info.pNext = &ci;
    info.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    info.firstSet = 0;
    info.descriptorSetCount = 2;
    info.pDescriptorSets = sets;
    info.dynamicOffsetCount = 2;
    info.pDynamicOffsets = offsets;
    const auto saved_info = Snapshot(info);
    expected_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    core(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == 1);
    CHECK(!std::memcmp(&info, saved_info.data(), sizeof(info)) && !std::memcmp(&ci, saved_ci.data(), sizeof(ci)));
    CHECK(observed_sets == std::vector<VkDescriptorSet>(sets, sets + 2));
    CHECK(observed_offsets == std::vector<uint32_t>(offsets, offsets + 2));
    // The temporary public handle has been destroyed. The native layout used
    // for the captured binding remains live until command-buffer reset.
    CHECK(!destroyed.empty() && layouts.count(observed.layout));
    Reset();
    CHECK(destroyed.size() == next_layout - 100);

    unsigned before = forwarded;
    info.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    khr(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before && !layouts.empty());
    Reset();

    info.stageFlags = VK_SHADER_STAGE_ALL;
    expected_stages = VK_SHADER_STAGE_ALL;
    core(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before + 1);
    Reset();

    for (unsigned allocation = 1; allocation <= 3; ++allocation) {
        unsigned calls = forwarded;
        fail_at = create_attempts + allocation;
        info.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        core(command, &info);
        CHECK(cb.record_result == VK_ERROR_OUT_OF_HOST_MEMORY && forwarded == calls && layouts.empty());
        Reset();
    }
    fail_at = 0;

    struct Unknown {
        VkStructureType sType;
        const void *pNext;
        uint64_t sentinel;
    } unknown{(VkStructureType)0x7ffffffe, &ci, UINT64_C(0xabcdef1234567890)};
    const auto saved_unknown = Snapshot(unknown);
    info.pNext = &unknown;
    before = forwarded;
    khr(command, &info);
    CHECK(cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT && forwarded == before && layouts.empty());
    CHECK(!std::memcmp(&unknown, saved_unknown.data(), sizeof(unknown)));
    Reset();

    vkrt_rtp_finish(&dev);
    passthrough = &info;
    core(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before + 1 && !cb.rtp && layouts.empty());
    CHECK(!std::memcmp(&unknown, saved_unknown.data(), sizeof(unknown)));
    vkrt_unmap_object(VKRT_HKEY(command));
    vkrt_unmap_object(VKRT_HKEY(device));
    std::printf("RTP bindings/pipeline derivation host contracts %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
