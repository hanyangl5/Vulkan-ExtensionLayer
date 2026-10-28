/* SPDX-License-Identifier: MIT
 * Completion must not introduce a new earlier semaphore signal operation.
 * Run once with the software layer and once as a native RT control. --oracle
 * executes only the independent CPU dependency model, without Vulkan calls.
 *
 * Reuse public-device/buffer/debug helpers from the query replay fixture. Its
 * main is not called; this test does not use its software-only size oracle.
 */
#define main query_replay_unused_main
#include "test_query_replay.c"
#undef main

/* Normative basis:
 * https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-implicit
 * Earlier semaphore signals belong to a later semaphore signal's first scope.
 * Earlier fence signals do not gain that semaphore-to-semaphore ordering edge.
 * Submit2's stageMask restricts command stages, not the earlier-signal edge:
 * https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html
 */
static void progress_oracle(void) {
    enum { QUERY_PREFIX, TRANSFER_TAIL, RETIREMENT, COMPUTE_SIGNAL, HOST_GATE, NODES };
    for (unsigned private_semaphore = 0; private_semaphore < 2; ++private_semaphore) {
        const unsigned dependencies[NODES] = {0, 1u << HOST_GATE, (1u << QUERY_PREFIX) | (1u << TRANSFER_TAIL),
                                              (1u << QUERY_PREFIX) | (private_semaphore ? 1u << RETIREMENT : 0),
                                              1u << COMPUTE_SIGNAL};
        unsigned complete = 0;
        bool changed;
        do {
            changed = false;
            for (unsigned i = 0; i < NODES; ++i) {
                if ((complete & (1u << i)) || (dependencies[i] & ~complete)) continue;
                complete |= 1u << i;
                changed = true;
            }
        } while (changed);
        if (private_semaphore)
            REQUIRE(complete == (1u << QUERY_PREFIX));
        else
            REQUIRE(complete == (1u << NODES) - 1);
    }
    puts("signal-order dependency model: private full-scope semaphore adds a feedback cycle; GPU progress is observed separately");
}

static VkCommandBuffer signal_query_prefix(VkQueryPool pool, const struct object *blas) {
    VkCommandBuffer command = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(command, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    /* This query-only prefix is a separate batch BEFORE the transfer wait.
     * There is no public build/copy/fill or dependency after the tail in this
     * recording. Private ALL->CS query barriers therefore cannot have the
     * future TRANSFER tail in their first scope. */
    reset_query(command, pool, 0, 1);
    write_as(command, 1, &blas->handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, pool, 0);
    CHECK(vkEndCommandBuffer(command));
    return command;
}

static void signal_order_case(const struct object *blas, struct gpu_buf *tail_buffer, bool application_fence, bool software) {
    VkQueryPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                       .queryCount = 1};
    VkQueryPool pool;
    CHECK(vkCreateQueryPool(g_dev, &pool_info, NULL, &pool));
    VkCommandBuffer prefix = signal_query_prefix(pool, blas);
    VkCommandBuffer tail = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(tail, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    const uint32_t pattern = application_fence ? UINT32_C(0x13579bdf) : UINT32_C(0x2468ace0);
    vkCmdFillBuffer(tail, tail_buffer->buf, 0, 4, pattern);
    /* TRANSFER->HOST visibility only. In particular, this is not an ALL->CS
     * barrier capable of propagating the blocked tail to B's COMPUTE scope. */
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(tail, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(tail));
    VkSemaphore gate = create_timeline(), b_signal = create_timeline();
    VkFence fence_a = application_fence ? create_fence() : VK_NULL_HANDLE;
    VkFence fence_b = create_fence();
    PFN_vkQueueSubmit2 submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
    REQUIRE(submit2 != NULL);
    VkCommandBufferSubmitInfo prefix_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = prefix};
    VkCommandBufferSubmitInfo tail_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = tail};
    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = gate,
                                  .value = 1,
                                  .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    VkSubmitInfo2 a[] = {
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &prefix_info},
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
         .waitSemaphoreInfoCount = 1,
         .pWaitSemaphoreInfos = &wait,
         .commandBufferInfoCount = 1,
         .pCommandBufferInfos = &tail_info}};
    CHECK(submit2(g_queue, ARRAY_SIZE(a), a, fence_a));
    VkSemaphoreSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = b_signal,
                                    .value = 1,
                                    .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};
    /* B is an ordinary, empty native batch. It has no AS/query command or
     * replay recording, no transfer operation, and no pipeline barrier. */
    VkSubmitInfo2 b = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
    CHECK(submit2(g_queue, 1, &b, fence_b));
    const uint64_t one = 1;
    VkSemaphoreWaitInfo await_b = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &b_signal, .pValues = &one};
    VkResult progress = wait_semaphores(g_dev, &await_b, UINT64_C(5000000000));
    /* Stage scopes describe dependencies, not a five-second scheduling bound.
     * Native controls on the test GPU also time out here. Preserve readiness as
     * an observation; require eventual completion after releasing the gate.
     * The host submit spies separately verify absence of a private signal. */
    REQUIRE(progress == VK_SUCCESS || progress == VK_TIMEOUT);
    /* A's tail and B's whole-submit fence must still be pending. A fence is a
     * full completion witness, while the earlier COMPUTE signal is a prefix. */
    REQUIRE(vkGetFenceStatus(g_dev, fence_b) == VK_NOT_READY);
    if (fence_a) REQUIRE(vkGetFenceStatus(g_dev, fence_a) == VK_NOT_READY);
    /* Always release the gate, even on regression, so failure is bounded and
     * pending objects can be retired rather than leaving a hung test process. */
    signal_value(gate, 1);
    CHECK(wait_semaphores(g_dev, &await_b, timeout_ns));
    wait_fence(fence_b);
    if (fence_a) wait_fence(fence_a);
    uint64_t value[2] = {0};
    CHECK(get_query(g_dev, pool, 0, 1, sizeof(value), value, sizeof(value),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT));
    REQUIRE(value[1] && value[0] > 0 && value[0] <= blas->capacity);
    if (software) REQUIRE(value[0] == blas->required);
    REQUIRE(*(const uint32_t *)tail_buffer->mapped == pattern);
    printf("signal-order %s control, callerFence=%u: B signal before gate=%u, compacted=%llu\n", software ? "software" : "native",
           application_fence, progress == VK_SUCCESS, (unsigned long long)value[0]);
    vkDestroyFence(g_dev, fence_b, NULL);
    if (fence_a) vkDestroyFence(g_dev, fence_a, NULL);
    vkDestroySemaphore(g_dev, b_signal, NULL);
    vkDestroySemaphore(g_dev, gate, NULL);
    VkCommandBuffer commands[] = {prefix, tail};
    vkFreeCommandBuffers(g_dev, g_pool, ARRAY_SIZE(commands), commands);
    vkDestroyQueryPool(g_dev, pool, NULL);
}

int main(int argc, char **argv) {
    progress_oracle();
    if (argc == 2 && !strcmp(argv[1], "--oracle")) return g_fail ? 1 : 0;
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--oracle]\n", argv[0]);
        return 1;
    }
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    const bool software = layers && strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY");
    init_vulkan();
    build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    write_as = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetInstanceProcAddr(
        g_instance, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    reset_query = (PFN_vkCmdResetQueryPool)vkGetDeviceProcAddr(g_dev, "vkCmdResetQueryPool");
    get_query = (PFN_vkGetQueryPoolResults)vkGetDeviceProcAddr(g_dev, "vkGetQueryPoolResults");
    queue_submit = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit");
    signal_semaphore = (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(g_dev, "vkSignalSemaphore");
    wait_semaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(g_dev, "vkWaitSemaphores");
    REQUIRE(build_as && write_as && reset_query && get_query && queue_submit && signal_semaphore && wait_semaphores);
    if (g_fail) return 1;
    struct object blas = {0};
    struct gpu_buf vertices, tail;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    buf_create(9 * sizeof(float),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &vertices);
    const float data[9] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    memcpy(vertices.mapped, data, sizeof(data));
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 2,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    create_object(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry, 1);
    VkCommandBuffer setup = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(setup, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    const VkAccelerationStructureBuildRangeInfoKHR *range = &blas.range;
    build_as(setup, 1, &blas.info, &range);
    barrier(setup);
    CHECK(vkEndCommandBuffer(setup));
    submit_and_wait(1, &setup);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &setup);
    buf_create(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, &tail);
    signal_order_case(&blas, &tail, false, software);
    signal_order_case(&blas, &tail, true, software);
    buf_destroy(&tail);
    destroy_object(&blas);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
    destroy(g_instance, messenger, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("query signal order: %s (%d failures, %u validation errors)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", g_fail, atomic_load(&validation_errors));
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
