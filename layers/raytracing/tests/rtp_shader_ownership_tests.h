/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Actual public shader hooks against device-scoped native object spies.
 */
namespace ShaderOwnership {
using Words = std::vector<uint32_t>;
using Key = std::pair<VkDevice, uint64_t>;
bool active = false;
thread_local bool fail_next_new = false;
thread_local unsigned bookkeeping_faults = 0;
bool fail_registration_after_native = false;
bool inline_admission = false;
const void *expected_inline = nullptr;
VkShaderModule forced_native{};
VkResult fail_create = VK_SUCCESS;
CallbackOwner *exhaust_after_create = nullptr;
const VkShaderModuleCreateInfo *expected_create = nullptr;
const VkAllocationCallbacks *expected_callbacks = nullptr;
unsigned create_calls = 0, destroy_calls = 0, compute_calls = 0, graphics_calls = 0, rewrite_calls = 0;
uintptr_t next_handle = 0x510000, next_layout_handle = 0x520000;
struct Allocation {
    VkAllocationCallbacks callbacks{};
    bool custom = false;
    void *cookie = nullptr;
};
struct NativeShader {
    Words words;
    std::vector<Allocation> references;
};
std::map<Key, NativeShader> modules;
std::set<Key> owned_layouts, owned_pipelines;
bool Compatible(const VkAllocationCallbacks *a, const Allocation &b) {
    return bool(a) == b.custom && (!a || (a->pfnAllocation == b.callbacks.pfnAllocation && a->pfnFree == b.callbacks.pfnFree &&
                                          a->pfnReallocation == b.callbacks.pfnReallocation));
}
VKAPI_ATTR VkResult VKAPI_CALL CreateNative(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                            VkShaderModule *out) {
    ++create_calls;
    CHECK(ci && out && ci->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    if (expected_create) {
        CHECK(a == expected_callbacks && ci->pNext == expected_create->pNext && ci->flags == expected_create->flags &&
              ci->codeSize == expected_create->codeSize && !std::memcmp(ci->pCode, expected_create->pCode, ci->codeSize));
    }
    *out = VK_NULL_HANDLE;
    if (fail_create != VK_SUCCESS) return fail_create;
    Words words(ci->pCode, ci->pCode + ci->codeSize / sizeof(uint32_t));
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    CHECK(tools.Validate(words));
    Allocation allocation;
    allocation.custom = a != nullptr;
    if (a) allocation.callbacks = *a;
    allocation.cookie =
        a ? a->pfnAllocation(a->pUserData, 16, alignof(uint64_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) : std::malloc(16);
    if (!allocation.cookie) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = forced_native ? forced_native : Handle<VkShaderModule>(next_handle++);
    auto &record = modules[{d, VKRT_HKEY(*out)}];
    CHECK(record.references.empty() || record.words == words);
    record.words = std::move(words);
    record.references.push_back(allocation);
    if (exhaust_after_create) exhaust_after_create->budget = 0;
    if (fail_registration_after_native) fail_next_new = true;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyNative(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a) {
    ++destroy_calls;
    auto found = modules.find({d, VKRT_HKEY(h)});
    CHECK(found != modules.end() && !found->second.references.empty());
    if (found == modules.end() || found->second.references.empty()) return;
    auto allocation = found->second.references.back();
    CHECK(Compatible(a, allocation));
    if (a)
        a->pfnFree(a->pUserData, allocation.cookie);
    else
        std::free(allocation.cookie);
    found->second.references.pop_back();
    if (found->second.references.empty()) modules.erase(found);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice d, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *,
                                            VkPipelineLayout *out) {
    *out = Handle<VkPipelineLayout>(next_layout_handle++);
    CHECK(owned_layouts.insert({d, VKRT_HKEY(*out)}).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice d, VkPipelineLayout h, const VkAllocationCallbacks *) {
    CHECK(owned_layouts.erase({d, VKRT_HKEY(h)}) == 1);
}
void CheckStage(VkDevice d, VkPipelineLayout layout, const VkPipelineShaderStageCreateInfo &stage) {
    CHECK(owned_layouts.count({d, VKRT_HKEY(layout)}));
    if (stage.module) {
        CHECK(modules.count({d, VKRT_HKEY(stage.module)}));
    } else {
        CHECK(inline_admission && stage.pNext == expected_inline);
        const auto *ci = static_cast<const VkShaderModuleCreateInfo *>(stage.pNext);
        CHECK(ci && ci->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
        CHECK(ci && tools.Validate(ci->pCode, ci->codeSize / sizeof(uint32_t)));
    }
    CHECK(stage.pName && !std::strcmp(stage.pName, "native_main"));
}
VKAPI_ATTR VkResult VKAPI_CALL Compute(VkDevice d, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo *ci,
                                       const VkAllocationCallbacks *, VkPipeline *out) {
    CHECK(count == 1);
    CheckStage(d, ci->layout, ci->stage);
    CHECK(ci->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);
    ++compute_calls;
    *out = Handle<VkPipeline>(next_handle++);
    CHECK(owned_pipelines.insert({d, VKRT_HKEY(*out)}).second);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Graphics(VkDevice d, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo *ci,
                                        const VkAllocationCallbacks *, VkPipeline *out) {
    CHECK(count == 1 && ci->stageCount == 1);
    CheckStage(d, ci->layout, ci->pStages[0]);
    CHECK(ci->pStages[0].stage == VK_SHADER_STAGE_VERTEX_BIT);
    ++graphics_calls;
    *out = Handle<VkPipeline>(next_handle++);
    CHECK(owned_pipelines.insert({d, VKRT_HKEY(*out)}).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice d, VkPipeline h, const VkAllocationCallbacks *) {
    CHECK(owned_pipelines.erase({d, VKRT_HKEY(h)}) == 1);
}
VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties *out) {
    *out = {};
    out->limits.maxPushConstantsSize = 256;
    out->limits.maxComputeWorkGroupCount[0] = 65535;
}
Words Source(bool raygen, bool mixed = false) {
    std::string source = "OpCapability Shader\n";
    if (raygen || mixed) source += "OpCapability RayTracingKHR\n";
    if (raygen || mixed) source += "OpExtension \"SPV_KHR_ray_tracing\"\n";
    source += "OpMemoryModel Logical GLSL450\n";
    source += raygen ? "OpEntryPoint RayGenerationKHR %main \"raygen\"\n"
                     : "OpEntryPoint GLCompute %main \"native_main\"\nOpEntryPoint Vertex %vertex \"native_main\"\n";
    if (mixed) source += "OpEntryPoint RayGenerationKHR %raygen \"raygen\"\n";
    if (!raygen) source += "OpExecutionMode %main LocalSize 1 1 1\n";
    source += "%void = OpTypeVoid\n%fn = OpTypeFunction %void\n";
    source += "%main = OpFunction %void None %fn\n%entry = OpLabel\n";
    source += "OpReturn\nOpFunctionEnd\n";
    if (!raygen) {
        source += "%vertex = OpFunction %void None %fn\n%v = OpLabel\n";
        source += "OpReturn\nOpFunctionEnd\n";
    }
    if (mixed) source += "%raygen = OpFunction %void None %fn\n%r = OpLabel\nOpReturn\nOpFunctionEnd\n";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    Words words;
    CHECK(tools.Assemble(source, &words) && tools.Validate(words));
    return words;
}
VkResult Rewrite(const VkShaderModuleCreateInfo *ci, VkShaderModuleCreateInfo *out, bool *query, bool *pipeline) {
    // This is an explicit adapter-only no-RayQuery source oracle. Actual RQ
    // rewriting is independently qualified by public GPU/CTS fixtures.
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    CHECK(tools.Validate(ci->pCode, ci->codeSize / sizeof(uint32_t)));
    if (inline_admission) CHECK(!ci->pNext);
    ++rewrite_calls;
    *out = *ci;
    *query = *pipeline = false;
    for (size_t p = 5; p < ci->codeSize / sizeof(uint32_t); p += ci->pCode[p] >> 16) {
        const auto op = ci->pCode[p] & 65535;
        *query |= op == spv::OpTypeRayQueryKHR;
        *pipeline |= op == spv::OpEntryPoint && ci->pCode[p + 1] >= spv::ExecutionModelRayGenerationKHR &&
                     ci->pCode[p + 1] <= spv::ExecutionModelCallableKHR;
    }
    return VK_SUCCESS;
}
struct Device {
    vkrt_instance_data instance{};
    vkrt_device_data data{};
    VkPipelineLayout layout{};
    explicit Device(uintptr_t id) {
        data.device = Handle<VkDevice>(id);
        data.physical_device = physical;
        data.instance_data = &instance;
        data.virtual_rt = true;
        instance.vtable.GetPhysicalDeviceProperties = Properties;
        data.vtable.CreateShaderModule = CreateNative;
        data.vtable.DestroyShaderModule = DestroyNative;
        data.vtable.CreatePipelineLayout = CreateLayout;
        data.vtable.DestroyPipelineLayout = DestroyLayout;
        data.vtable.CreateComputePipelines = Compute;
        data.vtable.CreateGraphicsPipelines = Graphics;
        data.vtable.DestroyPipeline = DestroyPipeline;
        vkrt_map_object(VKRT_HKEY(data.device), &data);
        CHECK(vkrt_rtp_init(&data, nullptr) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        auto make = reinterpret_cast<PFN_vkCreatePipelineLayout>(vkrt_rtp_proc("vkCreatePipelineLayout"));
        CHECK(make(data.device, &ci, nullptr, &layout) == VK_SUCCESS);
    }
    void Finish() {
        if (layout) {
            auto drop = reinterpret_cast<PFN_vkDestroyPipelineLayout>(vkrt_rtp_proc("vkDestroyPipelineLayout"));
            drop(data.device, layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
        vkrt_rtp_finish(&data);
    }
    ~Device() {
        Finish();
        vkrt_unmap_object(VKRT_HKEY(data.device));
    }
    VkShaderModule Make(const Words &words, const VkAllocationCallbacks *allocator = nullptr, const void *next = nullptr) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.pNext = next;
        ci.codeSize = words.size() * sizeof(uint32_t);
        ci.pCode = words.data();
        const auto saved = Snapshot(ci);
        expected_create = &ci;
        expected_callbacks = allocator;
        VkShaderModule result{};
        CHECK(vkrt_CreateShaderModule(data.device, &ci, allocator, &result) == VK_SUCCESS && result);
        CHECK(saved == Snapshot(ci));
        expected_create = nullptr;
        expected_callbacks = nullptr;
        return result;
    }
    void Drop(VkShaderModule module, const VkAllocationCallbacks *allocator = nullptr) {
        vkrt_DestroyShaderModule(data.device, module, allocator);
    }
    void Use(VkShaderModule module, const void *next = nullptr, bool accept = true) {
        const auto before_compute = compute_calls, before_graphics = graphics_calls;
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.layout = layout;
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                         next,
                         0,
                         VK_SHADER_STAGE_COMPUTE_BIT,
                         module,
                         "native_main",
                         nullptr};
        const auto saved_compute = Snapshot(compute);
        VkPipeline result{};
        auto make_compute = reinterpret_cast<PFN_vkCreateComputePipelines>(vkrt_rtp_proc("vkCreateComputePipelines"));
        auto drop = reinterpret_cast<PFN_vkDestroyPipeline>(vkrt_rtp_proc("vkDestroyPipeline"));
        auto status = make_compute(data.device, VK_NULL_HANDLE, 1, &compute, nullptr, &result);
        CHECK(status == (accept ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT) && bool(result) == accept &&
              compute_calls == before_compute + (accept ? 1 : 0) && saved_compute == Snapshot(compute));
        if (result) drop(data.device, result, nullptr);
        VkPipelineShaderStageCreateInfo vertex = compute.stage;
        vertex.stage = VK_SHADER_STAGE_VERTEX_BIT;
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.rasterizerDiscardEnable = VK_TRUE;
        raster.lineWidth = 1;
        VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        graphics.layout = layout;
        graphics.stageCount = 1;
        graphics.pStages = &vertex;
        graphics.pVertexInputState = &input;
        graphics.pInputAssemblyState = &assembly;
        graphics.pRasterizationState = &raster;
        graphics.renderPass = Handle<VkRenderPass>(0x514e);
        const auto saved_graphics = Snapshot(graphics);
        const auto saved_vertex = Snapshot(vertex);
        auto make_graphics = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(vkrt_rtp_proc("vkCreateGraphicsPipelines"));
        result = VK_NULL_HANDLE;
        status = make_graphics(data.device, VK_NULL_HANDLE, 1, &graphics, nullptr, &result);
        CHECK(status == (accept ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT) && bool(result) == accept &&
              graphics_calls == before_graphics + (accept ? 1 : 0) && saved_graphics == Snapshot(graphics) &&
              saved_vertex == Snapshot(vertex));
        if (result) drop(data.device, result, nullptr);
    }
};
struct Placement {
    void *memory = std::malloc(4096);
    unsigned allocations = 0, frees = 0;
    static VKAPI_ATTR void *VKAPI_CALL Allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
        auto &p = *static_cast<Placement *>(user);
        CHECK(p.memory && !p.allocations && size <= 4096 && !(uintptr_t(p.memory) % alignment) &&
              scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        ++p.allocations;
        return p.memory;
    }
    static VKAPI_ATTR void VKAPI_CALL Free(void *user, void *memory) {
        auto &p = *static_cast<Placement *>(user);
        CHECK(memory == p.memory && ++p.frees == 1);
        std::free(memory);
        p.memory = nullptr;
    }
    VkAllocationCallbacks Callbacks() {
        VkAllocationCallbacks a{};
        a.pUserData = this;
        a.pfnAllocation = Allocate;
        a.pfnReallocation = Reallocate;
        a.pfnFree = Free;
        return a;
    }
};
struct RegistrationAllocator {
    void *live[8]{};
    unsigned allocations = 0, frees = 0;
    static VKAPI_ATTR void *VKAPI_CALL Allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
        auto &a = *static_cast<RegistrationAllocator *>(user);
        CHECK(a.allocations < 8 && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        void *p = std::malloc(size);
        CHECK(p && !(uintptr_t(p) % alignment));
        a.live[a.allocations++] = p;
        return p;
    }
    static VKAPI_ATTR void VKAPI_CALL Free(void *user, void *memory) {
        auto &a = *static_cast<RegistrationAllocator *>(user);
        unsigned matches = 0;
        for (auto &p : a.live)
            if (p == memory) {
                p = nullptr;
                ++matches;
            }
        CHECK(memory && matches == 1);
        ++a.frees;
        std::free(memory);
    }
    VkAllocationCallbacks Callbacks() {
        VkAllocationCallbacks a{};
        a.pUserData = this;
        a.pfnAllocation = Allocate;
        a.pfnReallocation = Reallocate;
        a.pfnFree = Free;
        return a;
    }
};
void RawFirst() {
    Device first(0x501), second(0x502);
    auto raw = first.Make(Source(true));
    forced_native = raw;
    auto native = first.Make(Source(false));
    CHECK(native != raw);
    first.Use(native);
    // Identical bits on another device cannot consult the first device's map.
    auto other = second.Make(Source(false));
    second.Use(other);
    second.Drop(other);
    const auto before = destroy_calls;
    first.Drop(native);
    CHECK(destroy_calls == before + 1 && vkrt_rtp_debug_owns(&first.data, VK_OBJECT_TYPE_SHADER_MODULE, VKRT_HKEY(raw)));
    first.Drop(raw);
    forced_native = VK_NULL_HANDLE;
}
void NativeFirst() {
    Device device(0x503);
    Placement storage;
    CHECK(storage.memory);
    auto callbacks = storage.Callbacks();
    forced_native = Handle<VkShaderModule>(uintptr_t(storage.memory));
    auto native = device.Make(Source(false));
    auto raw = device.Make(Source(true), &callbacks);
    CHECK(raw != native);
    device.Use(native);
    const auto before = destroy_calls;
    device.Drop(raw, &callbacks);
    CHECK(destroy_calls == before && storage.allocations == 1 && storage.frees == 1);
    device.Use(native);
    device.Drop(native);
    forced_native = VK_NULL_HANDLE;
}
void NativeAliasesAndAllocators() {
    Device device(0x504);
    forced_native = Handle<VkShaderModule>(0x514a);
    auto a = device.Make(Source(false)), b = device.Make(Source(false));
    device.Drop(a);
    CHECK(modules.at({device.data.device, VKRT_HKEY(forced_native)}).references.size() == 1);
    device.Use(b);
    device.Drop(b);
    forced_native = VK_NULL_HANDLE;
    CallbackOwner creator, destroyer;
    auto create_callbacks = Callbacks(creator), destroy_callbacks = Callbacks(destroyer);
    VkShaderModuleValidationCacheCreateInfoEXT cache{VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT};
    cache.validationCache = Handle<VkValidationCacheEXT>(0x514b);
    auto native = device.Make(Source(false), &create_callbacks, &cache);
    auto raw = device.Make(Source(true), &create_callbacks);
    const auto creation_calls = creator.calls;
    // A and B are compatible malloc/free callbacks. Both contexts remain
    // physically alive; only B is permitted to service this Destroy command.
    creator.live = false;
    device.Drop(native, &destroy_callbacks);
    device.Drop(raw, &destroy_callbacks);
    CHECK(creator.allocations == creator.frees && creator.calls == creation_calls && destroyer.calls >= 3);
    creator.live = true;
}
void FailureAndFinish() {
    Device device(0x505);
    const auto words = Source(false);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * sizeof(uint32_t);
    ci.pCode = words.data();
    fail_create = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkShaderModule result = Handle<VkShaderModule>(0x514c);
    CHECK(vkrt_CreateShaderModule(device.data.device, &ci, nullptr, &result) == fail_create && !result);
    fail_create = VK_SUCCESS;
    CallbackOwner owner;
    auto callbacks = Callbacks(owner);
    exhaust_after_create = &owner;
    result = Handle<VkShaderModule>(0x514d);
    const auto status = vkrt_CreateShaderModule(device.data.device, &ci, &callbacks, &result);
    exhaust_after_create = nullptr;
    // If an implementation allocates its record before native creation, no
    // later object allocation may remain. Either outcome must be transactional.
    CHECK(status == VK_SUCCESS || status == VK_ERROR_OUT_OF_HOST_MEMORY);
    if (status == VK_SUCCESS)
        device.Drop(result, &callbacks);
    else
        CHECK(!result && modules.empty());
    CHECK(owner.allocations == owner.frees);
    owner.budget = -1;
    RegistrationAllocator registration_owner;
    auto registration_callbacks = registration_owner.Callbacks();
    const auto previous_faults = bookkeeping_faults, previous_destroys = destroy_calls;
    fail_registration_after_native = true;
    result = Handle<VkShaderModule>(0x514f);
    const auto registration_status = vkrt_CreateShaderModule(device.data.device, &ci, &registration_callbacks, &result);
    fail_registration_after_native = false;
    const bool consumed = !fail_next_new;
    fail_next_new = false;
    std::printf("Shader map OOM: consumed=%d faults=%u status=%d output=%p native=%zu destroys=%u/%u allocations=%u/%u\n", consumed,
                bookkeeping_faults - previous_faults, registration_status, (void *)result, modules.size(),
                destroy_calls - previous_destroys, 1u, registration_owner.allocations, registration_owner.frees);
    CHECK(consumed && bookkeeping_faults == previous_faults + 1 && registration_status == VK_ERROR_OUT_OF_HOST_MEMORY && !result &&
          modules.empty() && destroy_calls == previous_destroys + 1 && registration_owner.allocations == registration_owner.frees);
    if (registration_status == VK_SUCCESS) device.Drop(result, &registration_callbacks);
    device.Make(Source(false), &callbacks);
    device.Make(Source(true), &callbacks);
    device.Finish();
    CHECK(owner.allocations == owner.frees && modules.empty());
    // Keep a failing old implementation's fixture cleanup independent from
    // the production ownership map, so later tests do not inherit leaked spies.
    while (!modules.empty()) {
        auto key = modules.begin()->first;
        auto allocation = modules.begin()->second.references.back();
        DestroyNative(key.first, Handle<VkShaderModule>(key.second), allocation.custom ? &allocation.callbacks : nullptr);
    }
}
void NativePassThrough() {
    Device device(0x506);
    device.Finish();
    device.data.virtual_rt = false;
    const auto previous_rewrites = rewrite_calls;
    forced_native = Handle<VkShaderModule>(0x5150);
    auto module = device.Make(Source(false));
    CHECK(module == forced_native && rewrite_calls == previous_rewrites && !device.data.rtp);
    device.Drop(module);
    forced_native = VK_NULL_HANDLE;
}
void InlineAdmission() {
    Device device(0x507);
    for (unsigned kind = 0; kind < 2; ++kind) {
        const auto words = Source(false, kind == 1), saved_words = words;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo tail{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        tail.requiredSubgroupSize = 32;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.pNext = &tail;
        ci.codeSize = words.size() * sizeof(uint32_t);
        ci.pCode = words.data();
        const auto saved_ci = Snapshot(ci);
        const auto saved_tail = Snapshot(tail);
        inline_admission = true;
        expected_inline = &ci;
        device.Use(VK_NULL_HANDLE, &ci, kind == 0);
        expected_inline = nullptr;
        inline_admission = false;
        CHECK(words == saved_words && saved_ci == Snapshot(ci) && saved_tail == Snapshot(tail));
    }
}
void Run() {
    active = true;
    RawFirst();
    NativeFirst();
    NativeAliasesAndAllocators();
    FailureAndFinish();
    NativePassThrough();
    InlineAdmission();
    CHECK(modules.empty() && owned_layouts.empty() && owned_pipelines.empty());
    active = false;
    std::printf("Public shader ownership native creates/destroys %u/%u; compute/graphics %u/%u; rewrite spy %u; map OOM %u\n",
                create_calls, destroy_calls, compute_calls, graphics_calls, rewrite_calls, bookkeeping_faults);
}
}  // namespace ShaderOwnership
