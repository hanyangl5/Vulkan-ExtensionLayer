// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal GPU harness shared by executable typed-backend component tests.
struct GPU {
    VkInstance instance{};
    VkDevice device{};
    VkQueue queue{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkShaderModule shader{};
    VkDescriptorSet set{};
    uint32_t *mapped = nullptr;
    VkDeviceAddress address = 0;
    bool specialized_context = false;
    bool Init(const std::vector<uint32_t> &code, uint32_t bytes, VkDeviceSize second_binding_offset = VK_WHOLE_SIZE,
              bool wide_features = true, VkDeviceSize frame_binding_bytes = VK_WHOLE_SIZE,
              VkDeviceSize context_offset = VK_WHOLE_SIZE) {
        specialized_context = context_offset != VK_WHOLE_SIZE;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        auto result = vkCreateInstance(&ci, nullptr, &instance);
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) return false;
        Check(result);
        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) return false;
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        auto physical = devices[0];
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        vkGetPhysicalDeviceFeatures2(physical, &features);
        if (specialized_context && (!v12.bufferDeviceAddress || !features.features.shaderInt64)) return false;
        if (wide_features && !(v12.shaderFloat16 && v12.shaderInt8 && features.features.shaderFloat64 &&
                               features.features.shaderInt64 && features.features.shaderInt16))
            return false;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        uint32_t family = 0;
        while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (family == count) return false;
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.shaderFloat16 = v12.shaderInt8 = wide_features;
        v12.bufferDeviceAddress = specialized_context;
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderFloat64 = enabled.shaderInt64 = enabled.shaderInt16 = wide_features;
        if (specialized_context) enabled.shaderInt64 = VK_TRUE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12};
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qi;
        dc.pEnabledFeatures = &enabled;
        Check(vkCreateDevice(physical, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = bytes;
        bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (specialized_context ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        Check(vkCreateBuffer(device, &bc, nullptr, &buffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        uint32_t memory_type = 0;
        while (memory_type < properties.memoryTypeCount) {
            const auto flags = properties.memoryTypes[memory_type].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << memory_type)) &&
                (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
            ++memory_type;
        }
        Require(memory_type < properties.memoryTypeCount, "No coherent host-visible frame memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        VkMemoryAllocateFlagsInfo allocation_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        allocation_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (specialized_context) allocation.pNext = &allocation_flags;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        Check(vkAllocateMemory(device, &allocation, nullptr, &memory));
        Check(vkBindBufferMemory(device, buffer, memory, 0));
        Check(vkMapMemory(device, memory, 0, bytes, 0, reinterpret_cast<void **>(&mapped)));
        std::memset(mapped, 0xcd, bytes);
        if (specialized_context) {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer;
            address = vkGetBufferDeviceAddress(device, &info);
            Require(address != 0, "No BDA for typed frame test buffer");
        }
        uint32_t binding_count = second_binding_offset == VK_WHOLE_SIZE ? 1 : 2;
        VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sc.bindingCount = binding_count;
        sc.pBindings = bindings;
        Check(vkCreateDescriptorSetLayout(device, &sc, nullptr, &set_layout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding_count};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pc.maxSets = 1;
        pc.poolSizeCount = 1;
        pc.pPoolSizes = &size;
        Check(vkCreateDescriptorPool(device, &pc, nullptr, &descriptors));
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sa.descriptorPool = descriptors;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &set_layout;
        Check(vkAllocateDescriptorSets(device, &sa, &set));
        VkDescriptorBufferInfo bi[] = {{buffer, 0, frame_binding_bytes == VK_WHOLE_SIZE ? bytes : frame_binding_bytes},
                                       {buffer, second_binding_offset, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < binding_count; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device, binding_count, writes, 0, nullptr);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lc.setLayoutCount = 1;
        lc.pSetLayouts = &set_layout;
        lc.pushConstantRangeCount = specialized_context ? 0 : 1;
        lc.pPushConstantRanges = &range;
        Check(vkCreatePipelineLayout(device, &lc, nullptr, &layout));
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = code.size() * 4;
        sm.pCode = code.data();
        Check(vkCreateShaderModule(device, &sm, nullptr, &shader));
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = shader;
        cp.stage.pName = "main";
        uint64_t context_address = address + (specialized_context ? context_offset : 0);
        uint32_t root_words[] = {uint32_t(context_address), uint32_t(context_address >> 32)};
        VkSpecializationMapEntry root_entries[] = {{0, 0, 4}, {1, 4, 4}};
        VkSpecializationInfo specialization{2, root_entries, sizeof(root_words), root_words};
        if (specialized_context) cp.stage.pSpecializationInfo = &specialization;
        cp.layout = layout;
        Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline));
        VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &command_pool, nullptr, &pool));
        return true;
    }
    void Dispatch(uint32_t phase, uint32_t limit, uint32_t invocations) {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command;
        Check(vkAllocateCommandBuffers(device, &allocation, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(command, &begin));
        VkMemoryBarrier available{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        available.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        available.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &available, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const uint32_t push[] = {phase, limit};
        if (!specialized_context) vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(command, invocations, 1, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                             0, nullptr);
        Check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &command);
    }
    ~GPU() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptors) vkDestroyDescriptorPool(device, descriptors, nullptr);
            if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (mapped) vkUnmapMemory(device, memory);
            if (buffer) vkDestroyBuffer(device, buffer, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
