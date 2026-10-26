// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Explicit software-backend public API test, not complete extension qualification.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
static VkResult layerInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
static void layerDestroyInstance(VkInstance, const VkAllocationCallbacks*);
static VkResult layerDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
#define vkCreateInstance layerInstance
#define vkDestroyInstance layerDestroyInstance
#define vkCreateDevice layerDevice
#include "native_fixture.h"
#undef vkCreateDevice
#undef vkDestroyInstance
#undef vkCreateInstance
#include "../omm_storage.h"
#include <array>
#include <atomic>
#include <string>

static std::atomic<unsigned> validationErrors{0}, validationWarnings{0};
static VkDebugUtilsMessengerEXT validationMessenger{};
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
        return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++validationWarnings;
    std::fprintf(stderr, "Validation %s: %s\n", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "Error" : "Warning",
                 data->pMessage);
    return VK_FALSE;
}
static VkResult layerInstance(const VkInstanceCreateInfo* original, const VkAllocationCallbacks* allocator, VkInstance* output) {
    auto info = *original;
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < original->enabledExtensionCount; ++i) extensions.push_back(original->ppEnabledExtensionNames[i]);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    auto debug = init<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    debug.pNext = original->pNext;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validation;
    info.pNext = &debug;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    const auto result = vkCreateInstance(&info, allocator, output);
    if (result == VK_SUCCESS) {
        debug.pNext = nullptr;
        auto create =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(*output, "vkCreateDebugUtilsMessengerEXT"));
        if (create) CHECK(create(*output, &debug, nullptr, &validationMessenger));
    }
    return result;
}
static void layerDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (validationMessenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, validationMessenger, nullptr);
        validationMessenger = VK_NULL_HANDLE;
    }
    vkDestroyInstance(instance, allocator);
}
static VkResult layerDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* original, const VkAllocationCallbacks* allocator,
                            VkDevice* output) {
    auto requested = *original;
    auto bda = init<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    bda.bufferDeviceAddress = VK_TRUE;
    auto as = init<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
    as.accelerationStructure = VK_TRUE;
    as.pNext = &bda;
    auto micromap =
        init<VkPhysicalDeviceOpacityMicromapFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT);
    micromap.micromap = VK_TRUE;
    micromap.pNext = &as;
    auto sync = init<VkPhysicalDeviceSynchronization2Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
    sync.synchronization2 = VK_TRUE;
    sync.pNext = &micromap;
    auto maintenance = init<VkPhysicalDeviceMaintenance4Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES);
    maintenance.maintenance4 = VK_TRUE;
    maintenance.pNext = &sync;
    const char* extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                VK_KHR_MAINTENANCE_4_EXTENSION_NAME};
    requested.pNext = &maintenance;
    requested.enabledExtensionCount = 5;
    requested.ppEnabledExtensionNames = extensions;
    // The selected physical device supports native RT, but no native RQ/RTP
    // feature is requested: automatic consumer instrumentation is still open.
    return vkCreateDevice(physical, &requested, allocator, output);
}
using namespace vel::omm;
namespace {
unsigned checks = 0, failures = 0;
void expect(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
struct Api {
    PFN_vkGetMicromapBuildSizesEXT sizes;
    PFN_vkCreateMicromapEXT create;
    PFN_vkDestroyMicromapEXT destroy;
    PFN_vkCmdBuildMicromapsEXT build;
    PFN_vkCmdCopyMicromapEXT copy;
    PFN_vkCmdPipelineBarrier2KHR barrier;
    PFN_vkQueueSubmit2KHR submit;
    explicit Api(VkDevice device) {
#define LOAD(member, name)                                                     \
    member = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
    if (!member) std::exit(1)
        LOAD(sizes, vkGetMicromapBuildSizesEXT);
        LOAD(create, vkCreateMicromapEXT);
        LOAD(destroy, vkDestroyMicromapEXT);
        LOAD(build, vkCmdBuildMicromapsEXT);
        LOAD(copy, vkCmdCopyMicromapEXT);
        LOAD(barrier, vkCmdPipelineBarrier2KHR);
        LOAD(submit, vkQueueSubmit2KHR);
#undef LOAD
    }
    void dependency(VkCommandBuffer cb, VkPipelineStageFlags2 source, VkAccessFlags2 reads, VkPipelineStageFlags2 destination,
                    VkAccessFlags2 writes) const {
        auto memory = init<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        memory.srcStageMask = source;
        memory.srcAccessMask = reads;
        memory.dstStageMask = destination;
        memory.dstAccessMask = writes;
        auto info = init<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        info.memoryBarrierCount = 1;
        info.pMemoryBarriers = &memory;
        barrier(cb, &info);
    }
};
std::array<uint32_t, 32> constants(uint32_t seed) {
    std::array<uint32_t, 32> result{};
    for (uint32_t i = 0; i < 32; ++i) result[i] = (seed + i * 0x10203u) ^ (i * 0xaabbccddu);
    return result;
}
uint32_t digest(const std::array<uint32_t, 32>& words) {
    uint32_t value = 0x9e3779b9u;
    for (uint32_t i = 0; i < 32; ++i) value = ((value << 5) | (value >> 27)) ^ (words[i] + i * 0x45d9f3bu);
    return value;
}
struct Program {
    Fixture& fixture;
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    Program(Fixture& f, const char* filename, VkDeviceAddress output, uint32_t index) : fixture(f) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() % 4) std::exit(1);
        auto bytes = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<uint32_t> words(bytes / 4);
        if (!file.read(reinterpret_cast<char*>(words.data()), bytes)) std::exit(1);
        auto create = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        create.codeSize = bytes;
        create.pCode = words.data();
        VkShaderModule shader;
        CHECK(vkCreateShaderModule(f.device, &create, nullptr, &shader));
        makeLayout();
        struct Specialization {
            uint64_t output;
            uint32_t index;
        } data{output, index};
        VkSpecializationMapEntry entries[] = {{0, 0, 8}, {1, 8, 4}};
        VkSpecializationInfo specialization{2, entries, sizeof(data), &data};
        auto info = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        info.layout = layout;
        info.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = &specialization;
        CHECK(vkCreateComputePipelines(f.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        vkDestroyShaderModule(f.device, shader, nullptr);
    }
    void makeLayout() {
        VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, 128};
        auto info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        CHECK(vkCreatePipelineLayout(fixture.device, &info, nullptr, &layout));
    }
    void capture(VkCommandBuffer cb, const std::array<uint32_t, 32>& words) {
        if (!layout) makeLayout();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_ALL, 0, 128, words.data());
        VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
        auto info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        VkPipelineLayout fragment;
        CHECK(vkCreatePipelineLayout(fixture.device, &info, nullptr, &fragment));
        const uint32_t fragmentOnly = 0x01234567;
        vkCmdPushConstants(cb, fragment, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &fragmentOnly);
        // Both public layouts die before the injected build or copy. The layer
        // must replay private snapshots with the exact original push ranges.
        vkDestroyPipelineLayout(fixture.device, fragment, nullptr);
        vkDestroyPipelineLayout(fixture.device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    ~Program() {
        vkDestroyPipeline(fixture.device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    }
};
void release(Fixture& fixture, const Buffer& buffer) {
    for (auto i = fixture.buffers.begin(); i != fixture.buffers.end(); ++i)
        if (i->handle == buffer.handle) {
            if (i->mapped) vkUnmapMemory(fixture.device, i->memory);
            vkDestroyBuffer(fixture.device, i->handle, nullptr);
            vkFreeMemory(fixture.device, i->memory, nullptr);
            fixture.buffers.erase(i);
            return;
        }
    std::exit(1);
}
struct Map {
    Buffer storage;
    VkMicromapEXT handle;
    uint64_t offset = 256, size;
};
Map createMap(Fixture& f, const Api& api, uint64_t size) {
    Map result{};
    result.size = size;
    result.storage =
        f.buffer(256 + size + 64, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    auto info = init<VkMicromapCreateInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT);
    info.buffer = result.storage.handle;
    info.offset = result.offset;
    info.size = size;
    info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    CHECK(api.create(f.device, &info, nullptr, &result.handle));
    return result;
}
void clearMap(Map& map) { std::memset(map.storage.mapped, 0xa5, static_cast<size_t>(256 + map.size + 64)); }
bool compare(const Map& map, const std::vector<uint8_t>& expected) {
    auto* data = static_cast<const uint8_t*>(map.storage.mapped);
    bool correct = map.size >= expected.size() && !std::memcmp(data + map.offset, expected.data(), expected.size());
    for (size_t i = 0; i < map.offset; ++i) correct &= data[i] == 0xa5;
    for (size_t i = map.offset + map.size; i < map.offset + map.size + 64; ++i) correct &= data[i] == 0xa5;
    return correct;
}
void submitPair(Fixture& f, const Api& api, VkCommandBuffer build, VkCommandBuffer copy) {
    auto semaphoreInfo = init<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    VkSemaphore semaphore;
    CHECK(vkCreateSemaphore(f.device, &semaphoreInfo, nullptr, &semaphore));
    auto command0 = init<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    command0.commandBuffer = build;
    auto command1 = init<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    command1.commandBuffer = copy;
    auto signal = init<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    signal.semaphore = semaphore;
    signal.stageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
    auto wait = signal;
    VkSubmitInfo2 submissions[2] = {init<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2),
                                    init<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2)};
    submissions[0].commandBufferInfoCount = 1;
    submissions[0].pCommandBufferInfos = &command0;
    submissions[0].signalSemaphoreInfoCount = 1;
    submissions[0].pSignalSemaphoreInfos = &signal;
    submissions[1].commandBufferInfoCount = 1;
    submissions[1].pCommandBufferInfos = &command1;
    submissions[1].waitSemaphoreInfoCount = 1;
    submissions[1].pWaitSemaphoreInfos = &wait;
    auto fenceInfo = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence;
    CHECK(vkCreateFence(f.device, &fenceInfo, nullptr, &fence));
    CHECK(api.submit(f.queue, 2, submissions, fence));
    CHECK(vkWaitForFences(f.device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(f.device, fence, nullptr);
    vkDestroySemaphore(f.device, semaphore, nullptr);
}
void run(Fixture& f, const Api& api, const char* shader, const char* name, uint32_t count, uint32_t stride, bool fourState,
         bool pointers) {
    std::vector<uint8_t> triangles((count - 1) * stride + 8, 0xcd), data;
    std::vector<VkMicromapUsageEXT> usages;
    size_t bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto format = count == 1 ? (fourState ? StorageFormat::FourState : StorageFormat::TwoState) : StorageFormat(1 + (i & 1));
        uint32_t level = count == 1 ? 0 : i % 3;
        const uint32_t offset = count == 1 ? 0 : 7 + i * 3;
        VkMicromapTriangleEXT record{offset, static_cast<uint16_t>(level), static_cast<uint16_t>(format)};
        std::memcpy(triangles.data() + i * stride, &record, 8);
        uint64_t size = 0;
        if (storage_data_size(format, level, &size) != StorageResult::Success) std::exit(1);
        bytes = std::max(bytes, size_t(offset + size));
        usages.push_back({1, level, static_cast<uint32_t>(format)});
    }
    data.resize(bytes);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 37 + 0xa5);
    constexpr auto inputUsage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto triangleBuffer = f.buffer(triangles.size(), inputUsage, true), dataBuffer = f.buffer(data.size(), inputUsage, true);
    std::memcpy(triangleBuffer.mapped, triangles.data(), triangles.size());
    std::memcpy(dataBuffer.mapped, data.data(), data.size());
    expect(!(triangleBuffer.address & 255) && !(dataBuffer.address & 255), "public build input BDA256 alignment");
    std::vector<const VkMicromapUsageEXT*> usagePointers;
    for (const auto& usage : usages) usagePointers.push_back(&usage);
    auto info = init<VkMicromapBuildInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT);
    info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    info.flags = VK_BUILD_MICROMAP_ALLOW_COMPACTION_BIT_EXT;
    info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    info.usageCountsCount = static_cast<uint32_t>(usages.size());
    info.pUsageCounts = pointers ? nullptr : usages.data();
    info.ppUsageCounts = pointers ? usagePointers.data() : nullptr;
    info.triangleArrayStride = stride;
    auto sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
    api.sizes(f.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &sizes);
    expect(sizes.micromapSize >= 64 && sizes.buildScratchSize >= 4, "public build size query returns bounded storage and scratch");
    auto source = createMap(f, api, sizes.micromapSize), clone = createMap(f, api, sizes.micromapSize),
         compact = createMap(f, api, sizes.micromapSize);
    auto scratch = f.buffer(sizes.buildScratchSize + std::max<VkDeviceSize>(256, f.scratchAlignment) - 1,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    info.data.deviceAddress = dataBuffer.address;
    info.triangleArray.deviceAddress = triangleBuffer.address;
    info.scratchData.deviceAddress = (scratch.address + std::max<VkDeviceSize>(256, f.scratchAlignment) - 1) &
                                     ~(std::max<VkDeviceSize>(256, f.scratchAlignment) - 1);
    info.dstMicromap = source.handle;
    auto result = f.buffer(8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    Program app0(f, shader, result.address, 0), app1(f, shader, result.address, 1);
    auto record = [&](VkCommandBuffer build, VkCommandBuffer copy, const auto& words0, const auto& words1) {
        constexpr auto micro = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
        constexpr auto read = VK_ACCESS_2_MICROMAP_READ_BIT_EXT, write = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
        api.dependency(build, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                       micro | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | read | write);
        app0.capture(build, words0);
        api.build(build, 1, &info);
        vkCmdDispatch(build, 1, 1, 1);  // No app rebind/push after the injected kernel.
        api.dependency(build, micro | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, write | VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(build));
        api.dependency(copy, micro, write, micro, read | write);
        app1.capture(copy, words1);
        auto cloneInfo = init<VkCopyMicromapInfoEXT>(VK_STRUCTURE_TYPE_COPY_MICROMAP_INFO_EXT);
        cloneInfo.src = source.handle;
        cloneInfo.dst = clone.handle;
        cloneInfo.mode = VK_COPY_MICROMAP_MODE_CLONE_EXT;
        api.copy(copy, &cloneInfo);
        api.dependency(copy, micro, write, micro, read);
        auto compactInfo = cloneInfo;
        compactInfo.src = clone.handle;
        compactInfo.dst = compact.handle;
        compactInfo.mode = VK_COPY_MICROMAP_MODE_COMPACT_EXT;
        api.copy(copy, &compactInfo);
        vkCmdDispatch(copy, 1, 1, 1);  // Both internal copy kernels must restore app state.
        api.dependency(copy, micro | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, write | VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(copy));
    };
    VkCommandBuffer commands[] = {f.begin(), f.begin()};
    auto words0 = constants(123), words1 = constants(456);
    record(commands[0], commands[1], words0, words1);
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        if (iteration == 2) {
            CHECK(vkResetCommandPool(f.device, f.pool, 0));
            auto begin = init<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
            CHECK(vkBeginCommandBuffer(commands[0], &begin));
            CHECK(vkBeginCommandBuffer(commands[1], &begin));
            words0 = constants(789);
            words1 = constants(987);
            record(commands[0], commands[1], words0, words1);
        }
        data[0] ^= 7;
        std::memcpy(dataBuffer.mapped, data.data(), data.size());
        clearMap(source);
        clearMap(clone);
        clearMap(compact);
        std::memset(result.mapped, 0xff, 8);
        std::vector<uint8_t> expected;
        expect(storage_build({triangles.data(), triangles.size(), count, stride, data.data(), data.size()}, &expected) ==
                   StorageResult::Success,
               "independent CPU canonical oracle");
        submitPair(f, api, commands[0], commands[1]);
        expect(compare(source, expected), "public build canonical bytes and guards match CPU reference");
        expect(compare(clone, expected) && compare(compact, expected),
               "public clone/compact consume execution-time built data and preserve guards");
        expect(*static_cast<const uint32_t*>(result.mapped) == digest(words0) &&
                   static_cast<const uint32_t*>(result.mapped)[1] == digest(words1),
               "public kernels restore app pipeline and ALL128 push history after public layouts are destroyed");
        const auto* magic = static_cast<const uint8_t*>(source.storage.mapped) + source.offset;
        expect(!std::memcmp(magic, "VELOMM\0\0", 8), "canonical magic proves forced software backend, not native OMM pass-through");
        std::printf("Checked public path: %s %s (%zu logical input bytes, build/copy canonical + PC128)\n", name,
                    iteration == 0   ? "initial"
                    : iteration == 1 ? "CB replay"
                                     : "pool reset/re-record",
                    data.size());
    }
    vkFreeCommandBuffers(f.device, f.pool, 2, commands);
    // Build products must survive legal input/scratch destruction. The copy
    // below is recorded only after those buffers no longer exist.
    release(f, triangleBuffer);
    release(f, dataBuffer);
    release(f, scratch);
    clearMap(clone);
    auto final = f.begin();
    api.dependency(final, VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
                   VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_MICROMAP_READ_BIT_EXT);
    auto copyInfo = init<VkCopyMicromapInfoEXT>(VK_STRUCTURE_TYPE_COPY_MICROMAP_INFO_EXT);
    copyInfo.src = source.handle;
    copyInfo.dst = clone.handle;
    copyInfo.mode = VK_COPY_MICROMAP_MODE_CLONE_EXT;
    api.copy(final, &copyInfo);
    api.dependency(final, VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    f.submit(final);
    std::vector<uint8_t> expected;
    storage_build({triangles.data(), triangles.size(), count, stride, data.data(), data.size()}, &expected);
    expect(compare(clone, expected), "public copy remains correct after all build input and scratch buffers are destroyed");
    api.destroy(f.device, compact.handle, nullptr);
    api.destroy(f.device, clone.handle, nullptr);
    api.destroy(f.device, source.handle, nullptr);
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const char* force = std::getenv("VK_VEL_OMM_EXPERIMENTAL_SOFTWARE");
    if (!force || std::strcmp(force, "1")) {
        std::puts("SKIP: requires explicit software OMM experimental selection");
        return 77;
    }
    {
        Fixture fixture;
        if (!fixture.open()) return 77;
        Api api(fixture.device);
        std::puts(
            "Public OMM API test: AS+BDA+Int64+sync2+maintenance4+micromap; no RQ/RTP device features; canonical software proof "
            "required");
        run(fixture, api, argv[1], "tiny 2-state", 1, 8, false, false);
        run(fixture, api, argv[1], "tiny 4-state", 1, 8, true, true);
        run(fixture, api, argv[1], "mixed strided 2/4-state", 5, 24, false, false);
    }
    std::printf("Public software OMM layer API: %u checks, %u failures, %u validation errors, %u warnings\n", checks, failures,
                validationErrors.load(), validationWarnings.load());
    return failures || validationErrors || validationWarnings ? 1 : 0;
}
