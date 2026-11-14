/* SPDX-License-Identifier: MIT
 *
 * tests/gpu_harness.h - shared Vulkan/AS fixture helpers.
 *
 * This is an include-only harness: each test translation unit provides its own
 * main() and uses these helpers to create an acceleration-structure capable
 * device, buffers and command state. It deliberately contains no shader
 * traversal path; end-to-end ray tracing verification lives in the RT pipeline
 * fixtures (test_rtp_api and friends).
 */
#ifndef VKRT_GPU_HARNESS_H
#define VKRT_GPU_HARNESS_H

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define CHECK(x)                                             \
    do {                                                     \
        VkResult r_ = (x);                                   \
        if (r_ != VK_SUCCESS) {                              \
            fprintf(stderr, "%s failed: %d\n", #x, (int)r_); \
            exit(1);                                         \
        }                                                    \
    } while (0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct gpu_buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *mapped;
    VkDeviceAddress addr;
    VkDeviceSize size;
};

/* VK_KHR_acceleration_structure entry points are not exported from
 * vulkan-1.lib; they have to be pulled from the device dispatch table. */
static PFN_vkCreateAccelerationStructureKHR pCreateAS;
static PFN_vkDestroyAccelerationStructureKHR pDestroyAS;

static VkInstance g_instance;
static VkPhysicalDevice g_pdev;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfamily;
static VkCommandPool g_pool;

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return 0;
}

static void buf_create(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, struct gpu_buf *out) {
    memset(out, 0, sizeof(*out));
    out->size = size;
    printf("  buf_create size=%llu usage=0x%x\n", (unsigned long long)size, usage);

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(g_dev, &bci, NULL, &out->buf));
    printf("  CreateBuffer ok\n");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, out->buf, &req);
    printf("  mem req: size=%llu bits=0x%x\n", (unsigned long long)req.size, req.memoryTypeBits);

    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_mem_type(req.memoryTypeBits, want),
    };
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) mai.pNext = &flags;
    CHECK(vkAllocateMemory(g_dev, &mai, NULL, &out->mem));
    printf("  AllocateMemory ok (type %u)\n", mai.memoryTypeIndex);
    CHECK(vkBindBufferMemory(g_dev, out->buf, out->mem, 0));

    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        CHECK(vkMapMemory(g_dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->mapped));
        printf("  MapMemory ok -> %p\n", out->mapped);
    }

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        static PFN_vkGetBufferDeviceAddress pGBDA;
        if (!pGBDA) {
            /* Enabled through VK_KHR_buffer_device_address, so the KHR name
             * is the one the loader definitely resolves -- the unsuffixed
             * core name is only guaranteed once the device is 1.2. */
            pGBDA = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(g_dev, "vkGetBufferDeviceAddressKHR");
            if (!pGBDA) pGBDA = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(g_dev, "vkGetBufferDeviceAddress");
            if (!pGBDA) {
                fprintf(stderr, "vkGetBufferDeviceAddress(KHR) missing\n");
                exit(1);
            }
        }
        VkBufferDeviceAddressInfo ai = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = out->buf,
        };
        out->addr = pGBDA(g_dev, &ai);
        printf("  device address -> 0x%llx\n", (unsigned long long)out->addr);
    }
}

static void buf_destroy(struct gpu_buf *b) {
    if (b->mapped) vkUnmapMemory(g_dev, b->mem);
    if (b->buf) vkDestroyBuffer(g_dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(g_dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

/* Acceleration-structure capable device. Ray query is intentionally absent:
 * the layer no longer advertises or lowers VK_KHR_ray_query. */
static void init_vulkan(void) {
    const char *iext[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = iext,
    };
    CHECK(vkCreateInstance(&ici, NULL, &g_instance));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, NULL);
    if (n == 0) {
        fprintf(stderr, "no Vulkan device\n");
        exit(1);
    }
    {
        VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
        vkEnumeratePhysicalDevices(g_instance, &n, devs);
        g_pdev = devs[0];
        free(devs);
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_pdev, &properties);
    printf("Physical API %u.%u; timestampComputeAndGraphics=%u\n", VK_API_VERSION_MAJOR(properties.apiVersion),
           VK_API_VERSION_MINOR(properties.apiVersion), properties.limits.timestampComputeAndGraphics);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &qn, NULL);
    VkQueueFamilyProperties *qp = calloc(qn, sizeof(*qp));
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &qn, qp);
    g_qfamily = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            g_qfamily = i;
            break;
        }
    }
    free(qp);
    if (g_qfamily == UINT32_MAX) {
        fprintf(stderr, "no compute queue\n");
        exit(1);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_qfamily,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };

    const char *dext[] = {
#ifdef VKRT_TEST_FIXTURE_ONLY
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
#endif
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };

    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &bda,
        .accelerationStructure = VK_TRUE,
    };
#ifdef VKRT_TEST_FIXTURE_ONLY
    VkPhysicalDeviceSynchronization2Features sync2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                                                      .synchronization2 = VK_TRUE};
    bda.pNext = &sync2;
#endif

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &asf,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = (uint32_t)ARRAY_SIZE(dext),
        .ppEnabledExtensionNames = dext,
    };
    CHECK(vkCreateDevice(g_pdev, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfamily, 0, &g_queue);

    pCreateAS = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(g_dev, "vkCreateAccelerationStructureKHR");
    pDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(g_dev, "vkDestroyAccelerationStructureKHR");
    if (!pCreateAS || !pDestroyAS) {
        fprintf(stderr, "device has no vkCreateAccelerationStructureKHR\n");
        exit(1);
    }

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_qfamily,
    };
    CHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_pool));
}

static uint32_t *read_spv(const char *path, size_t *words) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "fseek failed\n");
        exit(1);
    }
    long size = ftell(f);
    rewind(f);
    if (size < 0 || (size & 3)) {
        fprintf(stderr, "bad SPIR-V size\n");
        exit(1);
    }
    uint32_t *w = malloc((size_t)size);
    if (!w) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    if (fread(w, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        exit(1);
    }
    fclose(f);
    *words = (size_t)size / 4;
    return w;
}

/* GPU builder fixtures record AS builds after binding application state. */
static void (*g_before_dispatch)(VkCommandBuffer);
static int g_fail;
static VkSemaphore g_submit_wait;
static VkCommandBuffer g_prebuild_cb;
static int g_submit_sync2;

/* Run the currently installed recording hook in one submitted command
 * buffer and wait for completion. Fixtures that used to piggy-back on a ray
 * dispatch use this to record their internal command chains without a
 * traversal shader. */
static void run_recorded_hook(void) {
    VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocate, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    if (g_before_dispatch) g_before_dispatch(command);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g_queue));
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
}

static void expect_u(const char *what, uint32_t got, uint32_t want) {
    if (got != want) {
        printf("  FAIL %s: got %u want %u\n", what, got, want);
        g_fail++;
    }
}

static void expect_f(const char *what, float got, float want, float eps) {
    if (!isfinite(got) || fabsf(got - want) > eps) {
        printf("  FAIL %s: got %g want %g\n", what, got, want);
        g_fail++;
    }
}

#endif /* VKRT_GPU_HARNESS_H */
