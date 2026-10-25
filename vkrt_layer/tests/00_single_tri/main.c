/* SPDX-License-Identifier: MIT
 *
 * tests/00_single_tri/main.c - V0 differential test (design doc section 13
 * items 1-4).
 *
 * Runs rq_test.comp through the software ray-query layer against a
 * CPU-built BVHIR and diffs every committed result against
 * vkrt_cpu_ref_intersect. Cases:
 *
 *   1. single triangle: miss / front hit / back hit
 *   2. two overlapping triangles: nearest committed hit wins
 *   3. primitive id survives (written as the ORIGINAL index at build time)
 *   4. TLAS: non-identity transform, visibility mask, custom index
 *
 * Enable the layer with
 *   set VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY
 *   set VK_LAYER_PATH=<build>/layers
 * or run with --no-layer to get the ICD's native ray query as the oracle.
 *
 * The AS objects are created through the normal VK_KHR_acceleration_structure
 * API so the layer's VirtualAS / descriptor rewrite / SPIR-V rewrite are all
 * exercised. The BVHIR itself is written by the CPU builder (M1) rather
 * than the GPU builder (M2), which does not exist yet.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "vkrt_cpu_bvh.h"

/* Mirror the few gl_RayFlags*EXT values this file uses so it does not
 * need a GLSL header. */
#ifndef gl_RAY_FLAG_NONE
#define gl_RAY_FLAG_NONE 0x0u
#endif
#ifndef gl_RAY_FLAG_TERMINATE_ON_FIRST_HIT
#define gl_RAY_FLAG_TERMINATE_ON_FIRST_HIT 0x4u
#endif
#ifndef gl_RAY_FLAG_OPAQUE
#define gl_RAY_FLAG_OPAQUE 0x1u
#endif

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         fprintf(stderr, "%s failed: %d\n", #x, (int)r_);                     \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct gpu_buf {
   VkBuffer       buf;
   VkDeviceMemory mem;
   void          *mapped;
   VkDeviceAddress addr;
   VkDeviceSize   size;
};

/* VK_KHR_acceleration_structure entry points are not exported from
 * vulkan-1.lib; they have to be pulled from the device dispatch table. */
static PFN_vkCreateAccelerationStructureKHR             pCreateAS;
static PFN_vkDestroyAccelerationStructureKHR            pDestroyAS;

static VkInstance       g_instance;
static VkPhysicalDevice g_pdev;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_qfamily;
static VkCommandPool    g_pool;

static uint32_t
find_mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(g_pdev, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return 0;
}

static void
buf_create(VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags want, struct gpu_buf *out)
{
   memset(out, 0, sizeof(*out));
   out->size = size;
   printf("  buf_create size=%llu usage=0x%x\n", (unsigned long long)size, usage);

   VkBufferCreateInfo bci = {
      .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size        = size,
      .usage       = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   CHECK(vkCreateBuffer(g_dev, &bci, NULL, &out->buf));
   printf("  CreateBuffer ok\n");

   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(g_dev, out->buf, &req);
   printf("  mem req: size=%llu bits=0x%x\n",
          (unsigned long long)req.size, req.memoryTypeBits);

   VkMemoryAllocateFlagsInfo flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
   };
   VkMemoryAllocateInfo mai = {
      .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize  = req.size,
      .memoryTypeIndex = find_mem_type(req.memoryTypeBits, want),
   };
   if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
      mai.pNext = &flags;
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
         pGBDA = (PFN_vkGetBufferDeviceAddress)
                 vkGetDeviceProcAddr(g_dev, "vkGetBufferDeviceAddressKHR");
         if (!pGBDA)
            pGBDA = (PFN_vkGetBufferDeviceAddress)
                    vkGetDeviceProcAddr(g_dev, "vkGetBufferDeviceAddress");
         if (!pGBDA) {
            fprintf(stderr, "vkGetBufferDeviceAddress(KHR) missing\n");
            exit(1);
         }
      }
      VkBufferDeviceAddressInfo ai = {
         .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
         .buffer = out->buf,
      };
      out->addr = pGBDA(g_dev, &ai);
      printf("  device address -> 0x%llx\n", (unsigned long long)out->addr);
   }
}

static void
buf_destroy(struct gpu_buf *b)
{
   if (b->mapped) vkUnmapMemory(g_dev, b->mem);
   if (b->buf)    vkDestroyBuffer(g_dev, b->buf, NULL);
   if (b->mem)    vkFreeMemory(g_dev, b->mem, NULL);
   memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ */

static void
init_vulkan(void)
{
   const char *iext[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
   VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
   VkInstanceCreateInfo ici = {
      .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo        = &app_info,
      .enabledExtensionCount   = 1,
      .ppEnabledExtensionNames = iext,
   };
   CHECK(vkCreateInstance(&ici, NULL, &g_instance));

   uint32_t n = 0;
   vkEnumeratePhysicalDevices(g_instance, &n, NULL);
   if (n == 0) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
   {
      VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
      vkEnumeratePhysicalDevices(g_instance, &n, devs);
      g_pdev = devs[0];
      free(devs);
   }

   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &qn, NULL);
   VkQueueFamilyProperties *qp = calloc(qn, sizeof(*qp));
   vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &qn, qp);
   g_qfamily = UINT32_MAX;
   for (uint32_t i = 0; i < qn; i++) {
      if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_qfamily = i; break; }
   }
   free(qp);
   if (g_qfamily == UINT32_MAX) { fprintf(stderr, "no compute queue\n"); exit(1); }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = g_qfamily,
      .queueCount       = 1,
      .pQueuePriorities = &prio,
   };

   const char *dext[] = {
#ifdef VKRT_TEST_FIXTURE_ONLY
      VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
#endif
      VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
      VK_KHR_RAY_QUERY_EXTENSION_NAME,
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
   VkPhysicalDeviceRayQueryFeaturesKHR rqf = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
      .pNext = &asf,
      .rayQuery = VK_TRUE,
   };
#ifdef VKRT_TEST_FIXTURE_ONLY
   VkPhysicalDeviceSynchronization2Features sync2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
      .synchronization2 = VK_TRUE};
   bda.pNext = &sync2;
#endif

   VkDeviceCreateInfo dci = {
      .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext                   = &rqf,
      .queueCreateInfoCount    = 1,
      .pQueueCreateInfos       = &qci,
      .enabledExtensionCount   = (uint32_t)ARRAY_SIZE(dext),
      .ppEnabledExtensionNames = dext,
   };
   CHECK(vkCreateDevice(g_pdev, &dci, NULL, &g_dev));
   vkGetDeviceQueue(g_dev, g_qfamily, 0, &g_queue);

   pCreateAS  = (PFN_vkCreateAccelerationStructureKHR)
                vkGetDeviceProcAddr(g_dev, "vkCreateAccelerationStructureKHR");
   pDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)
                vkGetDeviceProcAddr(g_dev, "vkDestroyAccelerationStructureKHR");
   if (!pCreateAS || !pDestroyAS) {
      fprintf(stderr, "device has no vkCreateAccelerationStructureKHR\n");
      exit(1);
   }

   VkCommandPoolCreateInfo pci = {
      .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = g_qfamily,
   };
   CHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_pool));
}

static uint32_t *
read_spv(const char *path, size_t *words)
{
   FILE *f = fopen(path, "rb");
   if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *w = malloc((size_t)sz);
   fread(w, 4, (size_t)sz / 4, f);
   fclose(f);
   *words = (size_t)sz / 4;
   return w;
}

struct out_block {
   uint32_t hit_type;
   uint32_t primitive_index;
   float    bary[2];
   float    t;
   uint32_t hit_kind;
   uint32_t proceeds;
   uint32_t shadow_hit;
   uint32_t front_face;
   uint32_t geometry_index, instance_index, instance_custom_index;
   uint32_t getter_errors;
};

/* GPU builder fixture records AS builds after binding application state. */
static void (*g_before_dispatch)(VkCommandBuffer);
static int g_fail;
static VkSemaphore g_submit_wait;
static VkCommandBuffer g_prebuild_cb;
static int g_submit_sync2;

/* Run one ray through the shader and report what it saw. */
static void
run_ray(VkAccelerationStructureKHR tlas, const float origin[3],
        const float dir[3], float tmin, float tmax,
        uint32_t flags, uint32_t mask, struct out_block *out)
{
   memset(out, 0, sizeof(*out));
   out->primitive_index = 0xFFFFFFFFu;
   out->t = -1.0f;

   struct gpu_buf outbuf;
   buf_create(sizeof(*out),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &outbuf);

   /* --- descriptor set layout: AS + storage buffer ---------------- */
   VkDescriptorSetLayoutBinding b[2] = {
      { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings    = b,
   };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &dsl));

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = 48,
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &dsl,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pcr,
   };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(g_dev, &plci, NULL, &pl));

   /* --- pipeline -------------------------------------------------- */
   size_t words = 0;
   uint32_t *spv = read_spv("rq_test.comp.spv", &words);
   VkShaderModuleCreateInfo smci = {
      .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = words * 4,
      .pCode    = spv,
   };
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(g_dev, &smci, NULL, &sm));
   free(spv);

   VkComputePipelineCreateInfo cpci = {
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = sm,
                  .pName  = "main" },
      .layout = pl,
   };
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

   /* --- descriptor set -------------------------------------------- */
   VkDescriptorPoolSize sizes[2] = {
      { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
   };
   VkDescriptorPoolCreateInfo dpci = {
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets       = 1,
      .poolSizeCount = 2,
      .pPoolSizes    = sizes,
   };
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(g_dev, &dpci, NULL, &dp));

   VkDescriptorSetAllocateInfo dsai = {
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = dp,
      .descriptorSetCount = 1,
      .pSetLayouts        = &dsl,
   };
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(g_dev, &dsai, &ds));

   VkWriteDescriptorSetAccelerationStructureKHR as_write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures = &tlas,
   };
   VkDescriptorBufferInfo bi = { .buffer = outbuf.buf, .offset = 0, .range = VK_WHOLE_SIZE };
   VkWriteDescriptorSet w[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = &as_write,
        .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bi },
   };
   vkUpdateDescriptorSets(g_dev, 2, w, 0, NULL);

   /* --- record + submit ------------------------------------------- */
   struct {
      float    origin[3], tmin;
      float    dir[3],   tmax;
      uint32_t flags, mask;
   } push;
   memcpy(push.origin, origin, 12);
   memcpy(push.dir, dir, 12);
   push.tmin = tmin; push.tmax = tmax;
   push.flags = flags; push.mask = mask;

   VkCommandBufferAllocateInfo cbai = {
      .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool        = g_pool,
      .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(g_dev, &cbai, &cb));

   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cb, &begin));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
   if (g_before_dispatch) g_before_dispatch(cb);
   vkCmdDispatch(cb, 1, 1, 1);
   VkMemoryBarrier host_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &host_barrier, 0, NULL, 0, NULL);
   CHECK(vkEndCommandBuffer(cb));

   VkSubmitInfo sub = {
      .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers    = &cb,
   };
   VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
   if (g_submit_wait) {
      sub.waitSemaphoreCount = 1;
      sub.pWaitSemaphores = &g_submit_wait;
      sub.pWaitDstStageMask = &wait_stage;
   }
   if (g_submit_sync2) {
      VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
      VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = g_submit_wait, .stageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR};
      VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
         .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
         .waitSemaphoreInfoCount = g_submit_wait ? 1 : 0, .pWaitSemaphoreInfos = &wait};
      PFN_vkQueueSubmit2 submit2 = (void *)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
      CHECK(submit2(g_queue, 1, &submit, VK_NULL_HANDLE));
   } else CHECK(vkQueueSubmit(g_queue, 1, &sub, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(g_queue));
   if (g_submit_wait) vkDestroySemaphore(g_dev, g_submit_wait, NULL);
   if (g_prebuild_cb) vkFreeCommandBuffers(g_dev, g_pool, 1, &g_prebuild_cb);
   g_submit_wait = VK_NULL_HANDLE;
   g_prebuild_cb = VK_NULL_HANDLE;

   printf("  readback mapped=%p size=%llu\n", outbuf.mapped,
          (unsigned long long)outbuf.size);
   if (outbuf.mapped) {
      const uint32_t *w = (const uint32_t *)outbuf.mapped;
      printf("  raw: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
             w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
   }
   memcpy(out, outbuf.mapped, sizeof(*out));
   if (out->getter_errors) {
      printf("FAIL: object-ray/matrix getters disagree (%u)\n", out->getter_errors);
      ++g_fail;
   }

   vkFreeCommandBuffers(g_dev, g_pool, 1, &cb);
   vkDestroyPipeline(g_dev, pipe, NULL);
   vkDestroyShaderModule(g_dev, sm, NULL);
   vkDestroyDescriptorPool(g_dev, dp, NULL);
   vkDestroyPipelineLayout(g_dev, pl, NULL);
   vkDestroyDescriptorSetLayout(g_dev, dsl, NULL);
   buf_destroy(&outbuf);
}

/* ------------------------------------------------------------------ */

static void
expect_u(const char *what, uint32_t got, uint32_t want)
{
   if (got != want) {
      printf("  FAIL %s: got %u want %u\n", what, got, want);
      g_fail++;
   }
}

static void
expect_f(const char *what, float got, float want, float eps)
{
   if (!isfinite(got) || fabsf(got - want) > eps) {
      printf("  FAIL %s: got %g want %g\n", what, got, want);
      g_fail++;
   }
}

#ifndef VKRT_TEST_FIXTURE_ONLY
int
main(int argc, char **argv)
{
   (void)argc; (void)argv;
   /* Unbuffered: a segfault must not swallow the progress trail. */
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);
   printf("init_vulkan...\n");
   init_vulkan();
   printf("init_vulkan ok\n");

   /* One triangle in the XY plane, counter-clockwise => front face +z. */
   const float verts[] = {
      -1.0f, -1.0f, 0.0f,
       1.0f, -1.0f, 0.0f,
       0.0f,  1.0f, 0.0f,
   };
   struct vkrt_cpu_geom geom = {
      .vertices = verts, .vertex_count = 3,
      .indices = NULL, .triangle_count = 1,
      .geometry_flags = 0,          /* non-opaque => candidate/confirm path */
      .geometry_id = 0,
   };
   printf("cpu BLAS build...\n");
   struct vkrt_cpu_bvh blas_bvh;
   if (vkrt_cpu_build_blas(&geom, &blas_bvh) != 0) {
      fprintf(stderr, "cpu BLAS build failed\n");
      return 1;
   }

   printf("blas buffer (%zu bytes)...\n", blas_bvh.size);
   struct gpu_buf blas;
   buf_create(blas_bvh.size,
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &blas);
   memcpy(blas.mapped, blas_bvh.data, blas_bvh.size);

   struct vkrt_cpu_instance inst = {
      .blas_base = blas.addr,
      .otw = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } },
      .custom_instance_index = 7,
      .mask = 0xFF,
      .instance_id = 3,
      .instance_flags = 0,
      .sbt_record_offset = 0,
   };
   struct vkrt_cpu_bvh tlas_bvh;
   if (vkrt_cpu_build_tlas(&inst, 1, &tlas_bvh) != 0) {
      fprintf(stderr, "cpu TLAS build failed\n");
      return 1;
   }

   printf("tlas buffer (%zu bytes)...\n", tlas_bvh.size);
   struct gpu_buf tlas_buf;
   buf_create(tlas_bvh.size,
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &tlas_buf);
   memcpy(tlas_buf.mapped, tlas_bvh.data, tlas_bvh.size);

   /* Create the AS objects through the normal API so the layer's
    * VirtualAS / descriptor rewrite / SPIR-V rewrite are exercised. */
   VkAccelerationStructureCreateInfoKHR asci = {
      .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
      .buffer = tlas_buf.buf,
      .size   = tlas_bvh.size,
      .type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
   };
   printf("create AS...\n");
   VkAccelerationStructureKHR tlas;
   CHECK(pCreateAS(g_dev, &asci, NULL, &tlas));
   printf("create AS ok\n");

   printf("V0 single-triangle differential test\n");
   printf("  blas bytes=%zu  tlas bytes=%zu\n", blas_bvh.size, tlas_bvh.size);

   struct out_block o;
   float origin[3] = { 0.0f, 0.0f,  2.0f };
   float toward[3] = { 0.0f, 0.0f, -1.0f };
   float away[3]   = { 0.0f, 0.0f,  1.0f };

   /* --- case 1a: straight-down hit at the centroid ----------------- */
   printf("case 1a: front hit\n");
   run_ray(tlas, origin, toward, 0.001f, 100.0f,
           gl_RAY_FLAG_NONE, 0xFF, &o);
   expect_u("hit_type", o.hit_type, 1u);
   expect_u("primitive_index", o.primitive_index, 0u);
   expect_f("t", o.t, 2.0f, 1e-4f);
   expect_u("proceeds >= 1", o.proceeds >= 1u ? 1u : 0u, 1u);
   printf("  t=%g bary=(%g,%g) proceeds=%u hit_kind=0x%08x prim=%u\n",
          o.t, o.bary[0], o.bary[1], o.proceeds, o.hit_kind, o.primitive_index);

   /* --- case 1b: miss --------------------------------------------- */
   printf("case 1b: miss\n");
   run_ray(tlas, origin, away, 0.001f, 100.0f,
           gl_RAY_FLAG_NONE, 0xFF, &o);
   expect_u("hit_type", o.hit_type, 0u);
   expect_u("shadow_hit", o.shadow_hit, 0u);

   /* --- case 1c: mask rejects ------------------------------------- */
   printf("case 1c: visibility mask rejects\n");
   run_ray(tlas, origin, toward, 0.001f, 100.0f,
           gl_RAY_FLAG_NONE, 0x00, &o);
   expect_u("hit_type", o.hit_type, 0u);

   /* --- case 1d: shadow ray agrees ------------------------------- */
   printf("case 1d: shadow ray\n");
   run_ray(tlas, origin, toward, 0.001f, 100.0f,
           gl_RAY_FLAG_NONE, 0xFF, &o);
   expect_u("shadow_hit", o.shadow_hit, 1u);

   /* --- CPU oracle for case 1a ------------------------------------ */
   {
      float t, u, v;
      int front = 0;
      int hit = vkrt_cpu_ref_intersect(verts, verts + 3, verts + 6,
                                       origin, toward, 0.001f, 100.0f,
                                       &t, &u, &v, &front);
      printf("oracle: hit=%d t=%g u=%g v=%g front=%d\n", hit, t, u, v, front);
      if (!hit) { printf("  FAIL oracle missed\n"); g_fail++; }
      else {
         expect_f("t (oracle)", o.t, t, 1e-4f);
         expect_f("u (oracle)", o.bary[0], u, 1e-4f);
         expect_f("v (oracle)", o.bary[1], v, 1e-4f);
         expect_u("front (oracle)", o.front_face, (uint32_t)front);
      }
   }

   /* =================================================================
    * case 2 + 3: two overlapping triangles. The nearer one must win,
    * and the reported primitive index must be the ORIGINAL one even
    * though the builder reorders triangles internally.
    * ================================================================= */
   {
      printf("case 2+3: two overlapping triangles, nearest wins, primitive id\n");

      /* tri 0 at z = 0 (far), tri 1 at z = 1 (near). Same footprint so a
       * ray down -z hits both and must commit the near one. */
      const float two[] = {
         -1.0f, -1.0f, 0.0f,   1.0f, -1.0f, 0.0f,   0.0f, 1.0f, 0.0f,
         -1.0f, -1.0f, 1.0f,   1.0f, -1.0f, 1.0f,   0.0f, 1.0f, 1.0f,
      };
      struct vkrt_cpu_geom g2 = {
         .vertices = two, .vertex_count = 6,
         .indices = NULL, .triangle_count = 2,
         .geometry_flags = 0, .geometry_id = 0,
      };
      struct vkrt_cpu_bvh b2;
      if (vkrt_cpu_build_blas(&g2, &b2) != 0) { printf("  FAIL build\n"); g_fail++; }
      else {
         struct gpu_buf blas2;
         buf_create(b2.size,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &blas2);
         memcpy(blas2.mapped, b2.data, b2.size);

         struct vkrt_cpu_instance i2 = {
            .blas_base = blas2.addr,
            .otw = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } },
            .custom_instance_index = 0,
            .mask = 0xFF,
            .instance_id = 0,
         };
         struct vkrt_cpu_bvh t2;
         vkrt_cpu_build_tlas(&i2, 1, &t2);
         struct gpu_buf tb2;
         buf_create(t2.size,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &tb2);
         memcpy(tb2.mapped, t2.data, t2.size);

         VkAccelerationStructureCreateInfoKHR c2 = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = tb2.buf, .size = t2.size,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
         };
         VkAccelerationStructureKHR tlas2;
         CHECK(pCreateAS(g_dev, &c2, NULL, &tlas2));

         float o2[3] = { 0.0f, 0.0f, 5.0f };
         float d2[3] = { 0.0f, 0.0f, -1.0f };
         run_ray(tlas2, o2, d2, 0.001f, 100.0f, gl_RAY_FLAG_NONE, 0xFF, &o);
         expect_u("hit_type", o.hit_type, 1u);
         /* near triangle is original index 1, at z = 1 => t = 4 */
         expect_u("primitive_index", o.primitive_index, 1u);
         expect_f("t (nearest)", o.t, 4.0f, 1e-4f);
         expect_u("proceeds (both candidates)", o.proceeds, 2u);
         printf("  prim=%u t=%g (want prim=1 t=4)\n", o.primitive_index, o.t);

         /* And the far one is still reachable on its own: shoot from below. */
         float o3[3] = { 0.0f, 0.0f, -5.0f };
         float d3[3] = { 0.0f, 0.0f, 1.0f };
         run_ray(tlas2, o3, d3, 0.001f, 100.0f, gl_RAY_FLAG_NONE, 0xFF, &o);
         expect_u("hit_type (from below)", o.hit_type, 1u);
         expect_u("primitive_index (from below)", o.primitive_index, 0u);
         expect_f("t (from below)", o.t, 5.0f, 1e-4f);

         pDestroyAS(g_dev, tlas2, NULL);
         buf_destroy(&tb2);
         buf_destroy(&blas2);
         vkrt_cpu_bvh_free(&b2);
         vkrt_cpu_bvh_free(&t2);
      }
   }

   /* =================================================================
    * case 4: TLAS with a non-identity transform. The instance is
    * translated +2 in z, so a ray from z = 4 travelling -z must hit at
    * t = 1 (world space) even though the geometry sits at z = 0 in
    * object space.
    * ================================================================= */
   {
      printf("case 4: non-identity instance transform\n");

      /* Object-space triangle at z = 0 (the original single triangle). */
      struct vkrt_cpu_geom g4 = {
         .vertices = verts, .vertex_count = 3,
         .indices = NULL, .triangle_count = 1,
         .geometry_flags = 0, .geometry_id = 0,
      };
      struct vkrt_cpu_bvh b4;
      if (vkrt_cpu_build_blas(&g4, &b4) != 0) { printf("  FAIL build\n"); g_fail++; }
      else {
         struct gpu_buf blas4;
         buf_create(b4.size,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &blas4);
         memcpy(blas4.mapped, b4.data, b4.size);

         /* object -> world = translate (0, 0, +2), row-major 3x4. */
         struct vkrt_cpu_instance i4 = {
            .blas_base = blas4.addr,
            .otw = { { 1, 0, 0, 0 },
                     { 0, 1, 0, 0 },
                     { 0, 0, 1, 2 } },
            .custom_instance_index = 42,
            .mask = 0xFF,
            .instance_id = 5,
         };
         /* Visit a farther instance first, then the nearer one. This makes
          * the suspend/resume path retain and revisit TLAS siblings after
          * completing the first BLAS; both must be reported as candidates. */
         struct vkrt_cpu_instance i4s[2] = { i4, i4 };
         i4s[0].otw[2][3] = 1.0f;
         i4s[0].instance_id = 6;
         struct vkrt_cpu_bvh t4;
         vkrt_cpu_build_tlas(i4s, 2, &t4);
         struct gpu_buf tb4;
         buf_create(t4.size,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &tb4);
         memcpy(tb4.mapped, t4.data, t4.size);

         VkAccelerationStructureCreateInfoKHR c4 = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = tb4.buf, .size = t4.size,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
         };
         VkAccelerationStructureKHR tlas4;
         CHECK(pCreateAS(g_dev, &c4, NULL, &tlas4));

         float o4[3] = { 0.0f, 0.0f, 4.0f };
         float d4[3] = { 0.0f, 0.0f, -1.0f };
         run_ray(tlas4, o4, d4, 0.001f, 100.0f, gl_RAY_FLAG_NONE, 0xFF, &o);
         expect_u("hit_type (transformed)", o.hit_type, 1u);
         /* world t: the triangle now sits at z = 2, ray starts at z = 4 */
         expect_f("t (transformed)", o.t, 2.0f, 1e-4f);
         expect_u("proceeds (both instances)", o.proceeds, 2u);
         printf("  t=%g (want 2)\n", o.t);

         /* Mask 0x00 still rejects the transformed instance. */
         run_ray(tlas4, o4, d4, 0.001f, 100.0f, gl_RAY_FLAG_NONE, 0x00, &o);
         expect_u("hit_type (masked out)", o.hit_type, 0u);

         pDestroyAS(g_dev, tlas4, NULL);
         buf_destroy(&tb4);
         buf_destroy(&blas4);
         vkrt_cpu_bvh_free(&b4);
         vkrt_cpu_bvh_free(&t4);
      }
   }

   printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);

   pDestroyAS(g_dev, tlas, NULL);
   buf_destroy(&tlas_buf);
   buf_destroy(&blas);
   vkrt_cpu_bvh_free(&blas_bvh);
   vkrt_cpu_bvh_free(&tlas_bvh);
   vkDestroyCommandPool(g_dev, g_pool, NULL);
   vkDestroyDevice(g_dev, NULL);
   vkDestroyInstance(g_instance, NULL);
   return g_fail ? 1 : 0;
}
#endif
