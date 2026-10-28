/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Public AS query publication on compute queues, including an explicitly
 * injected timestamp-free capability profile. Native and software controls
 * check their own compacted sizes; those representations need not match.
 */
#define main acquire_fixture_unused_main
#include "test_query_acquire.c"
#undef main

static void check_family(bool require_zero) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_pdev, &properties);
    if (require_zero) REQUIRE(!properties.limits.timestampComputeAndGraphics);
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    REQUIRE(families && g_qfamily < count);
    if (!families || g_qfamily >= count) exit(1);
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &count, families);
    REQUIRE(families[g_qfamily].queueFlags & VK_QUEUE_COMPUTE_BIT);
    if (require_zero) REQUIRE(families[g_qfamily].timestampValidBits == 0);
    printf("Public AS query compute family=%u timestampValidBits=%u%s\n", g_qfamily, families[g_qfamily].timestampValidBits,
           require_zero ? " (explicit zero-timestamp control)" : "");
    free(families);
}

static VkCommandBuffer record_cycle(const struct acquire_scene *scene, const struct query_case *test) {
    VkCommandBuffer command = begin_command();
    vkCmdFillBuffer(command, test->output.buf, 0, OUTPUT_BYTES, 0xcdcdcdcd);
    vkCmdResetQueryPool(command, test->pool, 0, 1);
    const VkMemoryBarrier as_dependency = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                           .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &as_dependency, 0, NULL, 0, NULL);
    write_property(command, 1, &scene->as, property_type, test->pool, 0);
    const VkMemoryBarrier fill_dependency = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                             .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                             .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &fill_dependency, 0, NULL,
                         0, NULL);
    vkCmdCopyQueryPoolResults(command, test->pool, 0, 1, test->output.buf, OUTPUT_OFFSET, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    const VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    return command;
}

int main(int argc, char **argv) {
    bool require_zero = argc == 2 && !strcmp(argv[1], "--require-zero-timestamps");
    if (argc > 2 || (argc == 2 && !require_zero)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    init_vulkan();
    check_family(require_zero);
    submit_legacy = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit");
    submit_two = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
    build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    write_property = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetDeviceProcAddr(
        g_dev, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    REQUIRE(submit_legacy && submit_two && build_as && write_property);
    if (g_fail) {
        destroy_device();
        return 1;
    }
    struct acquire_scene scene = {0};
    create_scene(&scene);
    struct query_case test = {0};
    prepare_query(&test, false);
    const VkCommandBuffer cycle = record_cycle(&scene, &test);
    const struct semaphore_op wait = {scene.build_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    uint64_t last_reference = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned epoch = 0; epoch < 3; ++epoch) {
            VkFence fence = create_fence();
            CHECK(submit(g_queue, mode != 0, cycle, 1, &wait, NULL, fence));
            wait_fence(fence);
            const uint64_t reference = reference_value(&test);
            last_reference = reference;
            REQUIRE(reference <= scene.storage.size);
            check_output(&test, reference, true);
            printf("AS query %s epoch=%u host value=%llu; copied value/availability/padding checked\n", mode ? "submit2" : "legacy",
                   epoch, (unsigned long long)reference);
            vkDestroyFence(g_dev, fence, NULL);
        }
    }
    CHECK(vkResetCommandBuffer(cycle, 0));
    REQUIRE(reference_value(&test) == last_reference);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &cycle);
    CHECK(vkResetCommandPool(g_dev, g_pool, 0));
    REQUIRE(reference_value(&test) == last_reference);
    VkCommandBuffer reset = begin_command();
    vkCmdResetQueryPool(reset, test.pool, 0, 1);
    CHECK(vkEndCommandBuffer(reset));
    VkFence reset_fence = create_fence();
    CHECK(submit(g_queue, false, reset, 0, NULL, NULL, reset_fence));
    wait_fence(reset_fence);
    uint64_t unavailable[] = {UINT64_C(0xcdcdcdcdcdcdcdcd), UINT64_C(0xcdcdcdcdcdcdcdcd)};
    REQUIRE(vkGetQueryPoolResults(g_dev, test.pool, 0, 1, sizeof(unavailable), unavailable, sizeof(unavailable),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY);
    REQUIRE(unavailable[1] == 0);
    vkDestroyFence(g_dev, reset_fence, NULL);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &reset);
    finish_query(&test);
    vkDestroySemaphore(g_dev, scene.build_done, NULL);
    pDestroyAS(g_dev, scene.as, NULL);
    buf_destroy(&scene.scratch);
    buf_destroy(&scene.storage);
    buf_destroy(&scene.vertices);
    destroy_device();
    printf("Public AS query publication, six replays and reset-to-unavailable: %s (%d failures, %u validation errors)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", g_fail, atomic_load(&validation_errors));
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
