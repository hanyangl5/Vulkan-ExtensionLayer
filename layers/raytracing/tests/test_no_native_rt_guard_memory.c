/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Independent 4096-allocation boundary controls using a fake native dispatch.
 */
#define main unused_general_guard_main
#include "test_no_native_rt_guard.c"
#undef main

static uint32_t memory_serial;
static int fail_memory;
static VKAPI_ATTR VkResult VKAPI_CALL memory_allocate(VkDevice device, const VkMemoryAllocateInfo *info,
                                                      const VkAllocationCallbacks *a, VkDeviceMemory *memory) {
    REQUIRE(device == (VkDevice)&native_device && info->allocationSize == 4 && !a);
    if (fail_memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *memory = (VkDeviceMemory)(uintptr_t)++memory_serial;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL memory_free(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
    REQUIRE(device == (VkDevice)&native_device && memory && !a);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL memory_gdpa(VkDevice device, const char *name) {
    if (!strcmp(name, "vkAllocateMemory")) return (PFN_vkVoidFunction)memory_allocate;
    if (!strcmp(name, "vkFreeMemory")) return (PFN_vkVoidFunction)memory_free;
    return driver_GDPA(device, name);
}
static VKAPI_ATTR void VKAPI_CALL memory_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *properties) {
    memset(properties, 0, sizeof(*properties));
    properties->limits.maxMemoryAllocationCount = 65536;
    REQUIRE(physical == (VkPhysicalDevice)&native_physical);
}
static VKAPI_ATTR void VKAPI_CALL memory_properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties) {
    memory_properties(physical, &properties->properties);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL memory_gipa(VkInstance instance, const char *name) {
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)memory_properties;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (PFN_vkVoidFunction)memory_properties2;
    return driver_GIPA(instance, name);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    remove(argv[1]);
    environment("VKRT_NO_RT_GUARD_LOG", argv[1]);
    environment("VKRT_NO_RT_GUARD_MEMORY_COUNT", "4096");
    environment("VKRT_NO_RT_GUARD_MASK_DISCOVERY", "");
    environment("VKRT_NO_RT_GUARD_ZERO_TIMESTAMPS", "");
    environment("VKRT_NO_RT_GUARD_DISPATCH_LIMITS", "");
    VkLayerInstanceLink il = {.pfnNextGetInstanceProcAddr = memory_gipa};
    VkLayerInstanceCreateInfo ic = {
        .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &il};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &ic};
    VkInstance instance;
    REQUIRE(guard_CreateInstance(&ii, NULL, &instance) == VK_SUCCESS);
    const char *names[] = {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"};
    VkPhysicalDeviceProperties properties;
    guard_GetPhysicalDeviceProperties((VkPhysicalDevice)&native_physical, &properties);
    REQUIRE(properties.limits.maxMemoryAllocationCount == 4096);
    for (unsigned i = 0; i < 2; ++i) {
        VkPhysicalDeviceProperties2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        ((PFN_vkGetPhysicalDeviceProperties2)guard_GetInstanceProcAddr(instance, names[i]))((VkPhysicalDevice)&native_physical,
                                                                                            &all);
        REQUIRE(!memcmp(&properties, &all.properties, sizeof(properties)));
    }
    VkLayerDeviceLink dl = {.pfnNextGetInstanceProcAddr = memory_gipa, .pfnNextGetDeviceProcAddr = memory_gdpa};
    VkLayerDeviceCreateInfo dc = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &dl};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dc};
    VkDevice device;
    REQUIRE(guard_CreateDevice((VkPhysicalDevice)&native_physical, &di, NULL, &device) == VK_SUCCESS);
    PFN_vkAllocateMemory allocate = (void *)guard_GetDeviceProcAddr(device, "vkAllocateMemory");
    PFN_vkFreeMemory release = (void *)guard_GetDeviceProcAddr(device, "vkFreeMemory");
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4};
    VkDeviceMemory memory[4096];
    fail_memory = 1;
    REQUIRE(allocate(device, &info, NULL, memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    fail_memory = 0;
    for (unsigned i = 0; i < 4096; ++i) REQUIRE(allocate(device, &info, NULL, &memory[i]) == VK_SUCCESS);
    if (argc == 3 && !strcmp(argv[2], "--overflow")) {
        VkDeviceMemory extra;
        allocate(device, &info, NULL, &extra);
        return 1;
    }
    release(device, memory[17], NULL);
    REQUIRE(allocate(device, &info, NULL, &memory[17]) == VK_SUCCESS);
    for (unsigned i = 0; i < 4096; ++i) release(device, memory[i], NULL);
    REQUIRE(guard_memory_live == 0 && guard_memory_peak == 4096 && guard_memory_allocations == 4097 &&
            guard_memory_attempts == 4098 && guard_memory_frees == 4097);
    guard_DestroyDevice(device, NULL);
    guard_DestroyInstance(instance, NULL);
    printf("Native allocation-count guard controls: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
