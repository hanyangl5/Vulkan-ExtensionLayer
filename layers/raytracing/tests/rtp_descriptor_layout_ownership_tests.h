/* SPDX-License-Identifier: MIT
 * Actual descriptor-layout hooks and real original-definition trackers.
 * The native spy aliases only the identical lowered definitions used here.
 */
namespace DescriptorLayoutOwnership {
struct NativeDefinition {
    VkDescriptorSetLayoutCreateFlags flags;
    VkDescriptorSetLayoutBinding binding;
    std::vector<ShaderOwnership::Allocation> references;
};
std::map<VkDescriptorSetLayout, NativeDefinition> definitions;
uintptr_t next_descriptor_layout = 0x710000;
bool alias_definitions = false;
unsigned native_creations = 0, native_retirements = 0;
VkResult native_failure = VK_SUCCESS;
CallbackOwner *exhaust_after_native = nullptr;
bool fail_bookkeeping = false;
const VkDescriptorSetLayoutCreateInfo *expected_source = nullptr;
const VkAllocationCallbacks *expected_allocator = nullptr;
const VkDescriptorSetAllocateInfo *expected_allocate = nullptr;
const VkDescriptorSetBindingReferenceVALVE *expected_reference = nullptr;
VkDescriptorSetLayout expected_native{};
bool native_passthrough = false, allocation_failure = false;
unsigned allocation_calls = 0, size_calls = 0, offset_calls = 0, host_mapping_calls = 0;
bool Equal(const NativeDefinition &a, const NativeDefinition &b) {
    return a.flags == b.flags && a.binding.binding == b.binding.binding && a.binding.descriptorType == b.binding.descriptorType &&
           a.binding.descriptorCount == b.binding.descriptorCount && a.binding.stageFlags == b.binding.stageFlags &&
           !a.binding.pImmutableSamplers && !b.binding.pImmutableSamplers;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateNative(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
                                            const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *out) {
    CHECK(d == device && ci && !(ci->flags & ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) && ci->bindingCount == 1);
    if (expected_source)
        CHECK(allocator == expected_allocator && ci->pNext == expected_source->pNext && ci->flags == expected_source->flags);
    const auto &b = ci->pBindings[0];
    CHECK(b.binding == 0 && b.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && b.descriptorCount == 1 &&
          b.stageFlags == VK_SHADER_STAGE_COMPUTE_BIT && !b.pImmutableSamplers);
    NativeDefinition value{ci->flags, b, {}};
    ++native_creations;
    *out = VK_NULL_HANDLE;
    if (native_failure != VK_SUCCESS) return native_failure;
    ShaderOwnership::Allocation reference;
    reference.custom = allocator != nullptr;
    if (allocator) reference.callbacks = *allocator;
    reference.cookie =
        allocator ? allocator->pfnAllocation(allocator->pUserData, 16, alignof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                  : std::malloc(16);
    if (!reference.cookie) return VK_ERROR_OUT_OF_HOST_MEMORY;
    value.references.push_back(reference);
    if (alias_definitions) {
        for (auto &entry : definitions) {
            if (Equal(entry.second, value)) {
                entry.second.references.push_back(reference);
                *out = entry.first;
                if (exhaust_after_native) exhaust_after_native->budget = 0;
                if (fail_bookkeeping) ShaderOwnership::fail_next_new = true;
                return VK_SUCCESS;
            }
        }
    }
    *out = Handle<VkDescriptorSetLayout>(next_descriptor_layout++);
    CHECK(definitions.emplace(*out, value).second);
    if (exhaust_after_native) exhaust_after_native->budget = 0;
    if (fail_bookkeeping) ShaderOwnership::fail_next_new = true;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyNative(VkDevice d, VkDescriptorSetLayout handle, const VkAllocationCallbacks *allocator) {
    CHECK(d == device);
    ++native_retirements;
    auto i = definitions.find(handle);
    CHECK(i != definitions.end() && !i->second.references.empty());
    if (i == definitions.end()) return;
    auto &references = i->second.references;
    CHECK(ShaderOwnership::Compatible(allocator, references.back()));
    if (allocator)
        allocator->pfnFree(allocator->pUserData, references.back().cookie);
    else
        std::free(references.back().cookie);
    references.pop_back();
    if (references.empty()) definitions.erase(i);
}
std::vector<uint8_t> Capture(VkDescriptorSetLayout layout) {
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &layout;
    void *bytes = nullptr;
    size_t size = 0;
    CHECK(vkrt_shader_interface_capture(&dev, &ci, &bytes, &size) == VK_SUCCESS && bytes && size >= 88);
    std::vector<uint8_t> value;
    if (bytes) value.assign(static_cast<uint8_t *>(bytes), static_cast<uint8_t *>(bytes) + size);
    vkrt_shader_interface_free_bytes(&dev, bytes);
    return value;
}
uint32_t Word(const std::vector<uint8_t> &wire, size_t offset) {
    if (offset > wire.size() || wire.size() - offset < 4) {
        CHECK(false);
        return 0;
    }
    return uint32_t(wire[offset]) | uint32_t(wire[offset + 1]) << 8 | uint32_t(wire[offset + 2]) << 16 |
           uint32_t(wire[offset + 3]) << 24;
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateNative(VkDevice d, const VkDescriptorSetAllocateInfo *ci, VkDescriptorSet *out) {
    ++allocation_calls;
    CHECK(d == device && expected_allocate && ci->descriptorPool == expected_allocate->descriptorPool &&
          ci->pNext == expected_allocate->pNext && ci->descriptorSetCount == 2 && ci->pSetLayouts[0] == expected_native &&
          ci->pSetLayouts[1] == expected_native);
    CHECK(!native_passthrough || ci == expected_allocate);
    for (uint32_t n = 0; n < ci->descriptorSetCount; ++n)
        out[n] = allocation_failure ? VK_NULL_HANDLE : Handle<VkDescriptorSet>(0x720000 + n);
    return allocation_failure ? VK_ERROR_OUT_OF_POOL_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL GetSizeNative(VkDevice d, VkDescriptorSetLayout layout, VkDeviceSize *size) {
    ++size_calls;
    CHECK(d == device && layout == expected_native && definitions.count(layout));
    *size = 128;
}
VKAPI_ATTR void VKAPI_CALL GetOffsetNative(VkDevice d, VkDescriptorSetLayout layout, uint32_t binding, VkDeviceSize *offset) {
    ++offset_calls;
    CHECK(d == device && layout == expected_native && definitions.count(layout) && binding == 0);
    *offset = 64;
}
VKAPI_ATTR void VKAPI_CALL GetHostMappingNative(VkDevice d, const VkDescriptorSetBindingReferenceVALVE *reference,
                                                VkDescriptorSetLayoutHostMappingInfoVALVE *mapping) {
    ++host_mapping_calls;
    CHECK(d == device && expected_reference && reference->descriptorSetLayout == expected_native &&
          reference->binding == expected_reference->binding && reference->pNext == expected_reference->pNext);
    CHECK(!native_passthrough || reference == expected_reference);
    mapping->descriptorOffset = 32;
    mapping->descriptorSize = 16;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeProc(VkDevice d, const char *name) {
    if (!std::strcmp(name, "vkAllocateDescriptorSets")) return reinterpret_cast<PFN_vkVoidFunction>(AllocateNative);
    if (!std::strcmp(name, "vkGetDescriptorSetLayoutSizeEXT")) return reinterpret_cast<PFN_vkVoidFunction>(GetSizeNative);
    if (!std::strcmp(name, "vkGetDescriptorSetLayoutBindingOffsetEXT"))
        return reinterpret_cast<PFN_vkVoidFunction>(GetOffsetNative);
    if (!std::strcmp(name, "vkGetDescriptorSetLayoutHostMappingInfoVALVE"))
        return reinterpret_cast<PFN_vkVoidFunction>(GetHostMappingNative);
    return ::NativeProc(d, name);
}
void OriginalDefinition(bool alias) {
    alias_definitions = alias;
    const auto make = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkCreateDescriptorSetLayout"));
    const auto drop = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkDestroyDescriptorSetLayout"));
    const auto make_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(vkrt_rtp_proc("vkCreatePipelineLayout"));
    const auto drop_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(vkrt_rtp_proc("vkDestroyPipelineLayout"));
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    const auto saved_ci = Snapshot(ci);
    const auto saved_a = Snapshot(binding);
    VkDescriptorSetLayout a{}, b{};
    CHECK(make && drop && make(device, &ci, nullptr, &a) == VK_SUCCESS && a);
    CHECK(Snapshot(ci) == saved_ci && Snapshot(binding) == saved_a);
    const auto wire_a = Capture(a);
    CHECK(Word(wire_a, 68) == VK_SHADER_STAGE_RAYGEN_BIT_KHR);
    VkPipelineLayoutCreateInfo pipeline_layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_ci.setLayoutCount = 1;
    pipeline_layout_ci.pSetLayouts = &a;
    VkPipelineLayout before{}, after{};
    CHECK(make_layout(device, &pipeline_layout_ci, nullptr, &before) == VK_SUCCESS && before);
    binding.stageFlags |= VK_SHADER_STAGE_MISS_BIT_KHR;
    const auto saved_b = Snapshot(binding);
    CHECK(make(device, &ci, nullptr, &b) == VK_SUCCESS && b);
    CHECK(a != b);
    CHECK(Snapshot(ci) == saved_ci && Snapshot(binding) == saved_b);
    CHECK(definitions.size() == (alias ? 1u : 2u));
    const auto wire_b = Capture(b);
    CHECK(Word(wire_b, 68) == (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR));
    CHECK(Capture(a) == wire_a);
    CHECK(make_layout(device, &pipeline_layout_ci, nullptr, &after) == VK_SUCCESS && after);
    CHECK(vkrt_rtp_layout_compatible(&dev, before, after, 0));
    for (auto layout : {before, after}) {
        const auto native = vkrt_rtp_native_layout(&dev, layout);
        CHECK(layouts.count(native) && layouts.at(native).sets.size() == 1 && definitions.count(layouts.at(native).sets[0]));
    }
    // A statically used resource makes the real trace adapter exercise the
    // original-definition compatibility check; an empty rgen would skip it.
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %buffer
OpDecorate %Block Block
OpMemberDecorate %Block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%Block = OpTypeStruct %uint
%ptr_block = OpTypePointer StorageBuffer %Block
%ptr_uint = OpTypePointer StorageBuffer %uint
%buffer = OpVariable %ptr_block StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%address = OpAccessChain %ptr_uint %buffer %zero
%value = OpLoad %uint %address
OpStore %address %value
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::vector<uint32_t> words;
    CHECK(tools.Assemble(source, &words) && tools.Validate(words));
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = words.size() * sizeof(uint32_t);
    shader_ci.pCode = words.data();
    VkShaderModule module{};
    CHECK(vkrt_CreateShaderModule(device, &shader_ci, nullptr, &module) == VK_SUCCESS && module);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.module = module;
    stage.pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR pipeline_ci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pipeline_ci.stageCount = 1;
    pipeline_ci.pStages = &stage;
    pipeline_ci.groupCount = 1;
    pipeline_ci.pGroups = &group;
    pipeline_ci.maxPipelineRayRecursionDepth = 1;
    pipeline_ci.layout = before;
    pipeline_ci.basePipelineIndex = -1;
    auto make_pipeline = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkrt_rtp_proc("vkCreateRayTracingPipelinesKHR"));
    auto drop_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(vkrt_rtp_proc("vkDestroyPipeline"));
    VkPipeline pipeline{};
    CHECK(make_pipeline(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &pipeline) == VK_SUCCESS && pipeline);
    CHECK(vkrt_rtp_bind_pipeline(&cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline));
    auto bind = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(vkrt_rtp_proc("vkCmdBindDescriptorSets"));
    auto trace = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkrt_rtp_proc("vkCmdTraceRaysKHR"));
    // The spy descriptor represents a set allocated from A and initialized
    // with a live SSBO. Capture does not execute GPU accesses or read its SBT.
    const VkDescriptorSet descriptor = Handle<VkDescriptorSet>(0x720000);
    bind(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, after, 0, 1, &descriptor, 0, nullptr);
    const VkStridedDeviceAddressRegionKHR raygen{0x10000, 32, 32}, empty{};
    trace(command, &raygen, &empty, &empty, &empty, 1, 1, 1);
    std::printf("Descriptor original stages alias=%u: public_a=%llu public_b=%llu native_defs=%zu trace=%d\n", unsigned(alias),
                static_cast<unsigned long long>(VKRT_HKEY(a)), static_cast<unsigned long long>(VKRT_HKEY(b)), definitions.size(),
                int(cb.record_result));
    CHECK(cb.record_result == VK_SUCCESS && cb.has_rtp_execution);
    vkrt_rtp_reset_command(&cb);
    cb.record_result = VK_SUCCESS;
    cb.has_rtp_execution = false;
    drop_pipeline(device, pipeline, nullptr);
    vkrt_DestroyShaderModule(device, module, nullptr);
    drop_layout(device, before, nullptr);
    drop_layout(device, after, nullptr);
    drop(device, a, nullptr);
    CHECK(!definitions.empty());
    CHECK(Capture(b) == wire_b);
    drop(device, b, nullptr);
    CHECK(definitions.empty());
}
void OriginalTypes() {
    const auto make = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkCreateDescriptorSetLayout"));
    const auto drop = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkDestroyDescriptorSetLayout"));
    alias_definitions = true;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                                         nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    VkDescriptorSetLayout a{}, b{};
    CHECK(make(device, &ci, nullptr, &a) == VK_SUCCESS && a);
    const auto wire_a = Capture(a);
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    CHECK(make(device, &ci, nullptr, &b) == VK_SUCCESS && b && definitions.size() == 1);
    CHECK(a != b);
    CHECK(Word(wire_a, 60) == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR && Capture(a) == wire_a);
    const auto wire_b = Capture(b);
    CHECK(Word(wire_b, 60) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    drop(device, a, nullptr);
    CHECK(Capture(b) == wire_b && definitions.size() == 1);
    drop(device, b, nullptr);
    CHECK(definitions.empty());
}
void CallbacksAndRollback() {
    const auto make = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkCreateDescriptorSetLayout"));
    const auto drop = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkDestroyDescriptorSetLayout"));
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    VkDescriptorBindingFlags flags = 0;
    VkDescriptorSetLayoutBindingFlagsCreateInfo chain{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    chain.bindingCount = 1;
    chain.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.pNext = &chain;
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    const auto saved_ci = Snapshot(ci);
    const auto saved_chain = Snapshot(chain);
    const auto saved_binding = Snapshot(binding);
    CallbackOwner creator, destroyer;
    auto a = Callbacks(creator), b = Callbacks(destroyer);
    expected_source = &ci;
    expected_allocator = &a;
    VkDescriptorSetLayout layout{};
    CHECK(make(device, &ci, &a, &layout) == VK_SUCCESS && layout);
    const auto create_calls = creator.calls;
    // Both callback contexts remain physically alive. Only B may be called by
    // this Destroy command, including retirement of the public OBJECT token.
    creator.live = false;
    drop(device, layout, &b);
    CHECK(creator.calls == create_calls && creator.allocations == creator.frees && destroyer.calls >= 2);
    creator.live = true;
    CHECK(definitions.empty() && Snapshot(ci) == saved_ci && Snapshot(chain) == saved_chain && Snapshot(binding) == saved_binding);
    native_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto before_failure = native_retirements;
    layout = VK_NULL_HANDLE;
    CHECK(make(device, &ci, &a, &layout) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !layout && definitions.empty() &&
          native_retirements == before_failure);
    native_failure = VK_SUCCESS;
    exhaust_after_native = &creator;
    layout = VK_NULL_HANDLE;
    const auto token_result = make(device, &ci, &a, &layout);
    exhaust_after_native = nullptr;
    creator.budget = -1;
    CHECK(token_result == VK_ERROR_OUT_OF_HOST_MEMORY && !layout && definitions.empty() && creator.allocations == creator.frees);
    if (token_result == VK_SUCCESS) drop(device, layout, &a);
    const auto prior_faults = ShaderOwnership::bookkeeping_faults;
    fail_bookkeeping = true;
    layout = VK_NULL_HANDLE;
    const auto bookkeeping_result = make(device, &ci, &a, &layout);
    fail_bookkeeping = false;
    ShaderOwnership::fail_next_new = false;
    CHECK(ShaderOwnership::bookkeeping_faults == prior_faults + 1 && bookkeeping_result == VK_ERROR_OUT_OF_HOST_MEMORY && !layout &&
          definitions.empty() && creator.allocations == creator.frees);
    if (bookkeeping_result == VK_SUCCESS) drop(device, layout, &a);
    std::printf("Descriptor callbacks/rollback: compatible destroy-B calls=%u; native/token/bookkeeping failures verified\n",
                destroyer.calls);
    expected_source = nullptr;
    expected_allocator = nullptr;
}
void NativeAdapters(bool passthrough) {
    const auto make = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkCreateDescriptorSetLayout"));
    const auto drop = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkDestroyDescriptorSetLayout"));
    native_passthrough = passthrough;
    auto saved_rtp = dev.rtp;
    if (passthrough) dev.rtp = nullptr;
    const bool saved_virtual = dev.virtual_rt;
    dev.virtual_rt = false;
    alias_definitions = false;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    VkDescriptorSetLayout layout{};
    CHECK(make(device, &ci, nullptr, &layout) == VK_SUCCESS && layout && definitions.size() == 1);
    expected_native = definitions.begin()->first;
    CHECK((layout == expected_native) == passthrough);
    const VkDescriptorSetLayout inputs[] = {layout, layout};
    const uint32_t counts[] = {0, 0};
    VkDescriptorSetVariableDescriptorCountAllocateInfo chain{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    chain.descriptorSetCount = 2;
    chain.pDescriptorCounts = counts;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.pNext = &chain;
    allocate.descriptorPool = Handle<VkDescriptorPool>(0x730000);
    allocate.descriptorSetCount = 2;
    allocate.pSetLayouts = inputs;
    expected_allocate = &allocate;
    const auto saved_allocate = Snapshot(allocate);
    const auto saved_inputs = Snapshot(inputs);
    const auto saved_chain = Snapshot(chain);
    auto call = reinterpret_cast<PFN_vkAllocateDescriptorSets>(vkrt_descriptor_sets_proc("vkAllocateDescriptorSets"));
    VkDescriptorSet sets[2]{};
    CHECK(call(device, &allocate, sets) == VK_SUCCESS && sets[0] == Handle<VkDescriptorSet>(0x720000) &&
          sets[1] == Handle<VkDescriptorSet>(0x720001));
    allocation_failure = true;
    CHECK(call(device, &allocate, sets) == VK_ERROR_OUT_OF_POOL_MEMORY && !sets[0] && !sets[1]);
    allocation_failure = false;
    CHECK(Snapshot(allocate) == saved_allocate && Snapshot(inputs) == saved_inputs && Snapshot(chain) == saved_chain);
    VkDescriptorSetBindingReferenceVALVE reference{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_BINDING_REFERENCE_VALVE};
    reference.descriptorSetLayout = layout;
    reference.binding = 0;
    expected_reference = &reference;
    const auto saved_reference = Snapshot(reference);
    VkDescriptorSetLayoutHostMappingInfoVALVE mapping{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_HOST_MAPPING_INFO_VALVE};
    auto host = reinterpret_cast<PFN_vkGetDescriptorSetLayoutHostMappingInfoVALVE>(
        vkrt_descriptor_sets_proc("vkGetDescriptorSetLayoutHostMappingInfoVALVE"));
    host(device, &reference, &mapping);
    CHECK(mapping.descriptorOffset == 32 && mapping.descriptorSize == 16 && Snapshot(reference) == saved_reference);
    drop(device, layout, nullptr);
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    CHECK(make(device, &ci, nullptr, &layout) == VK_SUCCESS && layout && definitions.size() == 1);
    expected_native = definitions.begin()->first;
    auto size = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(vkrt_descriptor_sets_proc("vkGetDescriptorSetLayoutSizeEXT"));
    auto offset = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
        vkrt_descriptor_sets_proc("vkGetDescriptorSetLayoutBindingOffsetEXT"));
    VkDeviceSize bytes = 0, binding_offset = 0;
    size(device, layout, &bytes);
    offset(device, layout, 0, &binding_offset);
    CHECK(bytes == 128 && binding_offset == 64);
    drop(device, layout, nullptr);
    CHECK(definitions.empty());
    dev.rtp = saved_rtp;
    dev.virtual_rt = saved_virtual;
    native_passthrough = false;
    expected_allocate = nullptr;
    expected_reference = nullptr;
}
void FinishCallbacks() {
    const auto make = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkCreateDescriptorSetLayout"));
    CallbackOwner owner;
    auto callbacks = Callbacks(owner);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    VkDescriptorSetLayout layout{};
    CHECK(make(device, &ci, &callbacks, &layout) == VK_SUCCESS && layout);
    vkrt_rtp_finish(&dev);
    CHECK(definitions.empty() && owner.allocations == owner.frees && owner.allocations >= 2);
    CHECK(vkrt_rtp_init(&dev, nullptr) == VK_SUCCESS);
}
void InterfacePublicationRollback() {
    vkrt_shader_interface_finish(&dev);
    const bool saved_custom = dev.queue_families_have_allocator;
    const auto saved_allocator = dev.queue_families_allocator;
    CallbackOwner device_owner, object_owner;
    device_owner.device = true;
    dev.queue_families_have_allocator = true;
    dev.queue_families_allocator = Callbacks(device_owner);
    VkDeviceCreateInfo device_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    CHECK(vkrt_shader_interface_init(&dev, &device_ci) == VK_SUCCESS);
    const auto make = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkCreateDescriptorSetLayout"));
    const auto drop = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(vkrt_descriptor_sets_proc("vkDestroyDescriptorSetLayout"));
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    auto callbacks = Callbacks(object_owner);
    exhaust_after_native = &device_owner;
    VkDescriptorSetLayout layout{};
    const auto status = make(device, &ci, &callbacks, &layout);
    exhaust_after_native = nullptr;
    device_owner.budget = -1;
    CHECK(status == VK_ERROR_OUT_OF_HOST_MEMORY && !layout && definitions.empty() &&
          object_owner.allocations == object_owner.frees);
    if (status == VK_SUCCESS) drop(device, layout, &callbacks);
    // The failed tracker publication owns neither a public token nor a native
    // reference. A retry can publish the same original definition normally.
    CHECK(make(device, &ci, &callbacks, &layout) == VK_SUCCESS && layout);
    drop(device, layout, &callbacks);
    vkrt_shader_interface_finish(&dev);
    CHECK(device_owner.allocations == device_owner.frees && object_owner.allocations == object_owner.frees && definitions.empty());
    dev.queue_families_have_allocator = saved_custom;
    dev.queue_families_allocator = saved_allocator;
}
void Run() {
    const auto saved_create = dev.vtable.CreateDescriptorSetLayout;
    const auto saved_destroy = dev.vtable.DestroyDescriptorSetLayout;
    const auto saved_get_proc = dev.vtable.GetDeviceProcAddr;
    const bool saved_virtual = dev.virtual_rt, saved_objects = dev.app_requested_shader_objects;
    dev.vtable.CreateDescriptorSetLayout = CreateNative;
    dev.vtable.DestroyDescriptorSetLayout = DestroyNative;
    dev.vtable.GetDeviceProcAddr = NativeProc;
    dev.virtual_rt = true;
    dev.app_requested_shader_objects = true;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    CHECK(vkrt_shader_interface_init(&dev, &ci) == VK_SUCCESS);
    OriginalDefinition(false);
    OriginalDefinition(true);
    OriginalTypes();
    CallbacksAndRollback();
    NativeAdapters(false);
    NativeAdapters(true);
    FinishCallbacks();
    InterfacePublicationRollback();
    CHECK(native_creations == native_retirements + 1 && definitions.empty());  // One deliberately failed native Create.
    CHECK(allocation_calls == 4 && size_calls == 2 && offset_calls == 2 && host_mapping_calls == 2);
    vkrt_shader_interface_finish(&dev);
    dev.virtual_rt = saved_virtual;
    dev.app_requested_shader_objects = saved_objects;
    dev.vtable.CreateDescriptorSetLayout = saved_create;
    dev.vtable.DestroyDescriptorSetLayout = saved_destroy;
    dev.vtable.GetDeviceProcAddr = saved_get_proc;
}
}  // namespace DescriptorLayoutOwnership
