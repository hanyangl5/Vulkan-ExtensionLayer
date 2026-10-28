/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Legal public allocation pressure against the explicitly injected native
 * limit of 4096. A real OOM is allowed; exceeding the native count is not.
 */
#define main unused_acquire_main
#include "test_query_acquire.c"
#undef main

int main(int argc, char **argv) {
    bool native = argc == 2 && !strcmp(argv[1], "--native-full");
    if (argc > 2 || (argc == 2 && !native)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    init_vulkan();
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_pdev, &properties);
    REQUIRE(properties.limits.maxMemoryAllocationCount == 4096);
    if (g_fail) return 1;
    /* Creating an unbuilt AS is legal. Its layer address cell forces a live
     * private pool slice before the public allocator reaches the boundary. */
    struct gpu_buf storage;
    buf_create(4096, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &storage);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = storage.buf,
                                                   .size = 4096,
                                                   .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
    VkAccelerationStructureKHR acceleration;
    CHECK(pCreateAS(g_dev, &create, NULL, &acceleration));
    VkDeviceMemory *allocations = calloc(4095, sizeof(*allocations));
    if (!allocations) return 1;
    const VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = 4096,
                                       .memoryTypeIndex = find_mem_type(UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    unsigned count = 0;
    VkResult stopped = VK_SUCCESS;
    while (count < 4095) {
        stopped = vkAllocateMemory(g_dev, &info, NULL, &allocations[count]);
        if (stopped != VK_SUCCESS) break;
        ++count;
    }
    if (native) {
        REQUIRE(count == 4095 && stopped == VK_SUCCESS);
    } else {
        REQUIRE(count > 4000 && count < 4095 && stopped == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        /* One freed native slot must become usable again. No fixed quota is
         * inferred from where the previous public allocation failed. */
        if (count) {
            vkFreeMemory(g_dev, allocations[--count], NULL);
            CHECK(vkAllocateMemory(g_dev, &info, NULL, &allocations[count]));
            ++count;
        }
        VkAccelerationStructureKHR another;
        create.offset = 256;
        create.size -= create.offset;
        CHECK(pCreateAS(g_dev, &create, NULL, &another));
        pDestroyAS(g_dev, another, NULL);
    }
    printf("Public live allocations including AS storage=%u of 4096; terminal result=%d; mode=%s\n", count + 1, stopped,
           native ? "native-full control" : "software OOM and free/retry");
    while (count) vkFreeMemory(g_dev, allocations[--count], NULL);
    free(allocations);
    pDestroyAS(g_dev, acceleration, NULL);
    buf_destroy(&storage);
    destroy_device();
    printf("Public allocation-count pressure: %s (%u validation errors)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", atomic_load(&validation_errors));
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
