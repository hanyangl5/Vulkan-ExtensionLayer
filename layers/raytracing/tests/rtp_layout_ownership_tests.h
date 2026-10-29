/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Public layout semantics survive legal equal-native-definition aliases.
 */
namespace LayoutOwnership {
void TwoDefinitionsAndAnnotations() {
    auto make = reinterpret_cast<PFN_vkCreatePipelineLayout>(vkrt_rtp_proc("vkCreatePipelineLayout"));
    auto drop = reinterpret_cast<PFN_vkDestroyPipelineLayout>(vkrt_rtp_proc("vkDestroyPipelineLayout"));
    VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    auto set = MakeDescriptorLayout(definition);
    VkPushConstantRange pushes[] = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32}, {VK_SHADER_STAGE_RAYGEN_BIT_KHR, 16, 4}};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = 2;
    ci.pPushConstantRanges = pushes;
    CallbackOwner creator, other;
    auto a = Callbacks(creator), b = Callbacks(other);
    alias_handle = Handle<VkPipelineLayout>(0x62001);
    alias_private = true;
    const auto first_ci = Snapshot(ci);
    const auto first_pushes = Snapshot(pushes);
    VkPipelineLayout first{}, second{}, third{};
    CHECK(make(device, &ci, &a, &first) == VK_SUCCESS && first);
    CHECK(Snapshot(ci) == first_ci && Snapshot(pushes) == first_pushes);
    const auto first_native = vkrt_rtp_native_layout(&dev, first);
    CHECK(first_native && layouts.count(first_native) && layouts.at(first_native).pushes[0].size == 32);
    pushes[1].offset = 32;
    const auto second_pushes = Snapshot(pushes);
    const auto result = make(device, &ci, &b, &second);
    CHECK(result == VK_SUCCESS && second && second != first);
    if (result != VK_SUCCESS) {
        drop(device, first, &a);
        DropDescriptorLayout(set);
        alias_handle = VK_NULL_HANDLE;
        alias_private = false;
        return;
    }
    CHECK(Snapshot(ci) == first_ci && Snapshot(pushes) == second_pushes);
    const auto second_native = vkrt_rtp_native_layout(&dev, second);
    CHECK(second_native == first_native && !vkrt_rtp_layout_compatible(&dev, first, second, 0));
    // Only the equal compute-range CIs alias. The retained RT-context layout
    // uses a different16-byte range and receives a distinct native identity.
    CHECK(layouts.size() > 1);
    CHECK(vkrt_debug_objects_init(&dev, nullptr, vkrt_rtp_debug_owns) == VK_SUCCESS);
    auto set_private = reinterpret_cast<PFN_vkSetPrivateData>(vkrt_debug_objects_proc("vkSetPrivateData"));
    auto get_private = reinterpret_cast<PFN_vkGetPrivateData>(vkrt_debug_objects_proc("vkGetPrivateData"));
    auto set_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkrt_debug_objects_proc("vkSetDebugUtilsObjectNameEXT"));
    const auto slot = Handle<VkPrivateDataSlot>(0x62002);
    CHECK(set_private && get_private && set_name);
    CHECK(set_private(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VKRT_HKEY(first), slot, 0x1111) == VK_SUCCESS);
    CHECK(set_private(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VKRT_HKEY(second), slot, 0x2222) == VK_SUCCESS);
    VkDebugUtilsObjectNameInfoEXT name{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    name.objectType = VK_OBJECT_TYPE_PIPELINE_LAYOUT;
    name.objectHandle = VKRT_HKEY(first);
    name.pObjectName = "first original RT range";
    CHECK(set_name(device, &name) == VK_SUCCESS);
    name.objectHandle = VKRT_HKEY(second);
    name.pObjectName = "second original RT range";
    CHECK(set_name(device, &name) == VK_SUCCESS);
    uint64_t value = 0;
    get_private(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VKRT_HKEY(first), slot, &value);
    CHECK(value == 0x1111);
    get_private(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VKRT_HKEY(second), slot, &value);
    CHECK(value == 0x2222);
    VkPipelineLayout identity_first{}, identity_second{};
    CHECK(vkrt_rtp_replay_layout_capture(&cb, first, &identity_first) == VK_SUCCESS && identity_first);
    CHECK(vkrt_rtp_replay_layout_capture(&cb, second, &identity_second) == VK_SUCCESS && identity_second &&
          identity_second != identity_first);
    const auto creator_calls = creator.calls;
    drop(device, first, &b);  // Compatible current-command userdata B also frees A's token.
    CHECK(creator.calls == creator_calls && creator.allocations == creator.frees);
    creator.live = false;  // Storage remains alive; later commands may use B only.
    get_private(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VKRT_HKEY(second), slot, &value);
    CHECK(value == 0x2222 && vkrt_rtp_native_layout(&dev, second) == second_native);
    pushes[1].offset = 16;
    CHECK(make(device, &ci, &b, &third) == VK_SUCCESS && third);
    get_private(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VKRT_HKEY(third), slot, &value);
    CHECK(value == 0);  // A new public object never inherits a shared native alias's sidecar.
    drop(device, third, &b);
    expected_push_bytes = 32;
    expected_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSet descriptor = Handle<VkDescriptorSet>(0x62003);
    auto bind = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(vkrt_rtp_proc("vkCmdBindDescriptorSets"));
    const auto before = forwarded;
    bind(command, VK_PIPELINE_BIND_POINT_COMPUTE, second, 0, 1, &descriptor, 0, nullptr);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before + 1 && observed.layout == second_native);
    drop(device, second, &b);
    const auto other_calls = other.calls;
    other.live = false;
    DropDescriptorLayout(set);
    vkrt_cmd_buffer_data execution{};
    execution.device_data = &dev;
    execution.cmd = command;
    execution.replaying = true;
    CHECK(vkrt_rtp_replay_layouts_inherit(&execution, &cb) == VK_SUCCESS);
    vkrt_rtp_reset_command(&cb);
    vkrt_map_object(VKRT_HKEY(command), &execution);
    bind(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity_first, 0, 1, &descriptor, 0, nullptr);
    CHECK(execution.record_result == VK_SUCCESS && observed.layout == first_native);
    bind(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity_second, 0, 1, &descriptor, 0, nullptr);
    CHECK(execution.record_result == VK_SUCCESS && observed.layout == second_native);
    vkrt_map_object(VKRT_HKEY(command), &cb);
    vkrt_rtp_reset_command(&execution);
    CHECK(layouts.empty() && creator.calls == creator_calls && other.calls == other_calls && other.allocations == other.frees);
    vkrt_debug_objects_finish(&dev);
    expected_push_bytes = 256;
    alias_handle = VK_NULL_HANDLE;
    alias_private = false;
}
void ConcurrentRetirement(const VkPipelineLayoutCreateInfo &ci, CallbackOwner &first, CallbackOwner &second,
                          const VkAllocationCallbacks &a, const VkAllocationCallbacks &b) {
    auto make = reinterpret_cast<PFN_vkCreatePipelineLayout>(vkrt_rtp_proc("vkCreatePipelineLayout"));
    auto drop = reinterpret_cast<PFN_vkDestroyPipelineLayout>(vkrt_rtp_proc("vkDestroyPipelineLayout"));
    alias_private = false;
    VkPipelineLayout survivor{};
    CHECK(make(device, &ci, &a, &survivor) == VK_SUCCESS);
    const auto references_before = NativeReferences();
    {
        std::lock_guard<std::mutex> lock(retirement_lock);
        gate_retirement = true;
        retirement_entered = release_retirement = creation_entered = false;
    }
    std::thread retiring([&] { drop(device, survivor, &a); });
    {
        std::unique_lock<std::mutex> lock(retirement_lock);
        CHECK(retirement_changed.wait_for(lock, std::chrono::seconds(5), [] { return retirement_entered; }));
    }
    VkPipelineLayout recreated{};
    VkResult recreated_result = VK_ERROR_UNKNOWN;
    bool started = false;
    std::thread creating([&] {
        {
            std::lock_guard<std::mutex> lock(retirement_lock);
            started = true;
            retirement_changed.notify_all();
        }
        recreated_result = make(device, &ci, &b, &recreated);
    });
    {
        std::unique_lock<std::mutex> lock(retirement_lock);
        CHECK(retirement_changed.wait_for(lock, std::chrono::seconds(5), [&] { return started; }));
        CHECK(!retirement_changed.wait_for(lock, std::chrono::milliseconds(100), [] { return creation_entered; }));
        release_retirement = true;
        retirement_changed.notify_all();
    }
    retiring.join();
    creating.join();
    gate_retirement = false;
    const auto first_calls = first.calls;
    first.live = false;
    const auto native = vkrt_rtp_native_layout(&dev, recreated);
    CHECK(recreated_result == VK_SUCCESS && recreated && native && layouts.count(native) &&
          NativeReferences() == references_before && vkrt_rtp_layout_compatible(&dev, recreated, recreated, 0));
    drop(device, recreated, &b);
    CHECK(!NativeReferences() && first.calls == first_calls && first.allocations == first.frees &&
          second.allocations == second.frees);
    first.live = true;
}
void Run() {
    CHECK(!dev.rtp && layouts.empty() && callback_allocations.empty());
    auto make = reinterpret_cast<PFN_vkCreatePipelineLayout>(vkrt_rtp_proc("vkCreatePipelineLayout"));
    auto drop = reinterpret_cast<PFN_vkDestroyPipelineLayout>(vkrt_rtp_proc("vkDestroyPipelineLayout"));
    CallbackOwner device_owner;
    device_owner.device = true;
    auto device_callbacks = Callbacks(device_owner);
    CHECK(vkrt_rtp_init(&dev, &device_callbacks) == VK_SUCCESS);
    TwoDefinitionsAndAnnotations();
    VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    auto set = MakeDescriptorLayout(definition);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push;
    const auto saved = Snapshot(ci);
    DescriptorHistoryAllocationFailures(device_owner);
    for (bool all_alias : {false, true}) {
        CallbackOwner first, second;
        auto a = Callbacks(first), b = Callbacks(second);
        alias_handle = Handle<VkPipelineLayout>(100000 + all_alias);
        alias_private = all_alias;
        VkPipelineLayout h[2]{};
        CHECK(make(device, &ci, &a, &h[0]) == VK_SUCCESS);
        CHECK(make(device, &ci, &b, &h[1]) == VK_SUCCESS);
        const auto native_second = vkrt_rtp_native_layout(&dev, h[1]);
        CHECK(native_second && layouts.count(native_second) && vkrt_rtp_layout_compatible(&dev, h[0], h[1], 0));
        drop(device, h[0], &a);
        const auto first_calls = first.calls;
        first.live = false;
        CHECK(vkrt_rtp_native_layout(&dev, h[1]) == native_second && vkrt_rtp_layout_compatible(&dev, h[1], h[1], 0));
        const uint32_t values[2]{};
        CHECK(vkrt_rtp_push(&cb, h[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(values), values) == VK_SHADER_STAGE_COMPUTE_BIT);
        drop(device, h[1], &b);
        const auto second_calls = second.calls;
        second.live = false;
        CHECK(!vkrt_rtp_layout_compatible(&dev, h[1], h[1], 0) && NativeReferences());
        Reset();
        CHECK(first.calls == first_calls && second.calls == second_calls && first.allocations == first.frees &&
              second.allocations == second.frees);
    }
    CallbackOwner first, second;
    auto a = Callbacks(first), b = Callbacks(second);
    alias_handle = Handle<VkPipelineLayout>(100010);
    alias_private = true;
    VkPipelineLayout survivor{};
    CHECK(make(device, &ci, &a, &survivor) == VK_SUCCESS);
    const auto survivor_refs = NativeReferences();
    unsigned fault_cases = 0;
    for (unsigned step = 1; step <= 3; ++step) {
        fail_at = create_attempts + step;
        VkPipelineLayout rejected = Handle<VkPipelineLayout>(9);
        CHECK(make(device, &ci, &b, &rejected) == VK_ERROR_OUT_OF_HOST_MEMORY && !rejected);
        fail_at = 0;
        CHECK(NativeReferences() == survivor_refs && vkrt_rtp_layout_compatible(&dev, survivor, survivor, 0));
        ++fault_cases;
    }
    second.budget = 0;
    VkPipelineLayout rejected = Handle<VkPipelineLayout>(9);
    auto allocation_result = make(device, &ci, &b, &rejected);
    CHECK(allocation_result == VK_ERROR_OUT_OF_HOST_MEMORY && !rejected);
    second.budget = -1;
    if (allocation_result == VK_SUCCESS) drop(device, rejected, &b);
    CHECK(NativeReferences() == survivor_refs && second.allocations == second.frees);
    ++fault_cases;
    drop(device, survivor, &a);
    CHECK(!NativeReferences());
    second.budget = 0;
    allocation_result = make(device, &ci, &b, &rejected);
    CHECK(allocation_result == VK_ERROR_OUT_OF_HOST_MEMORY && !rejected && !NativeReferences());
    second.budget = -1;
    if (allocation_result == VK_SUCCESS) drop(device, rejected, &b);
    ++fault_cases;
    ConcurrentRetirement(ci, first, second, a, b);
    CHECK(make(device, &ci, &a, &survivor) == VK_SUCCESS);
    VkPipelineLayout recreated{};
    CHECK(make(device, &ci, &b, &recreated) == VK_SUCCESS);
    // Device finish retains creation callback contexts only while both are live.
    vkrt_rtp_finish(&dev);
    CHECK(!NativeReferences() && callback_allocations.empty() && device_owner.allocations == device_owner.frees &&
          first.allocations == first.frees && second.allocations == second.frees);
    device_owner.live = false;
    CallbackOwner instance_owner;
    instance_owner.device = true;
    instance.has_allocator = true;
    instance.allocator = Callbacks(instance_owner);
    CHECK(vkrt_rtp_init(&dev, nullptr) == VK_SUCCESS);
    set = MakeDescriptorLayout(definition);
    CHECK(make(device, &ci, &a, &survivor) == VK_SUCCESS);
    CHECK(make(device, &ci, &b, &recreated) == VK_SUCCESS);
    vkrt_rtp_finish(&dev);
    CHECK(instance_owner.allocations == instance_owner.frees && callback_allocations.empty() && !NativeReferences() &&
          first.allocations == first.frees && second.allocations == second.frees && saved == Snapshot(ci));
    instance.has_allocator = false;
    instance.allocator = {};
    alias_handle = VK_NULL_HANDLE;
    alias_private = false;
    std::printf("Public layout ownership: simultaneous RT definitions, isolated sidecars, retained native replay; OOM %u\n",
                fault_cases);
}
}  // namespace LayoutOwnership
