/* SPDX-License-Identifier: MIT
 * The same public AS-build + ray-query API is tested with VKRT and the native
 * ICD. No BVH bytes are uploaded by the CPU in this fixture.
 */
#include "bvh4_abi.h"
#define VKRT_TEST_FIXTURE_ONLY
#include <stdbool.h>
#include "00_single_tri/main.c"

struct built_as {
   struct gpu_buf output, scratch;
   VkAccelerationStructureKHR handle;
   VkDeviceAddress address;
   VkDeviceSize size;
   VkDeviceSize offset;
   VkAccelerationStructureBuildGeometryInfoKHR info;
   VkAccelerationStructureBuildRangeInfoKHR ranges[2];
};
static struct built_as *g_builds[2];
static PFN_vkCmdBuildAccelerationStructuresKHR pBuild;
static bool g_software;
static bool g_split_builds;
static bool g_aligned_values;
static bool g_pool_pressure;
static VkAccelerationStructureKHR g_pressure_views[1023];

/* Bind at the smallest legal nonzero offset. Ordinary translated storage
 * buffers can require only 16-byte alignment even though AS values require
 * 256-byte alignment. Exercise that case through real AS builds and queries. */
static void create_as_output(VkDeviceSize size, struct gpu_buf *out) {
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!g_aligned_values) {
        buf_create(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->size = size;
    VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(g_dev, &ci, NULL, &out->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, out->buf, &req);
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = req.size + req.alignment,
        .memoryTypeIndex =
            find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(g_dev, &allocation, NULL, &out->mem));
    CHECK(vkBindBufferMemory(g_dev, out->buf, out->mem, req.alignment));
    void *mapping;
    CHECK(vkMapMemory(g_dev, out->mem, 0, VK_WHOLE_SIZE, 0, &mapping));
    out->mapped = (uint8_t *)mapping + req.alignment;
    PFN_vkGetBufferDeviceAddress address = (void *)vkGetDeviceProcAddr(g_dev, "vkGetBufferDeviceAddressKHR");
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->buf};
    out->addr = address(g_dev, &ai);
    printf("AS suballocation: alignment=%llu bindOffset=%llu BDA=0x%llx mod256=%llu\n", (unsigned long long)req.alignment,
           (unsigned long long)req.alignment, (unsigned long long)out->addr, (unsigned long long)(out->addr & 255));
    if (req.alignment < 128 && !(out->addr & 127)) {
        fprintf(stderr, "FAIL suballocation did not exercise a non-128-aligned physical root\n");
        ++g_fail;
    }
}

static void record_builds(VkCommandBuffer cmd)
{
   if (g_split_builds) {
      VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      CHECK(vkAllocateCommandBuffers(g_dev, &alloc, &g_prebuild_cb));
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkBeginCommandBuffer(g_prebuild_cb, &begin));
      const VkAccelerationStructureBuildRangeInfoKHR *range = g_builds[0]->ranges;
      pBuild(g_prebuild_cb, 1, &g_builds[0]->info, &range);
      CHECK(vkEndCommandBuffer(g_prebuild_cb));
      VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      CHECK(vkCreateSemaphore(g_dev, &ci, NULL, &g_submit_wait));
      if (g_submit_sync2) {
         VkCommandBufferSubmitInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_prebuild_cb};
         VkSemaphoreSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = g_submit_wait, .stageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR};
         VkSubmitInfo2 s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1, .pCommandBufferInfos = &cb,
            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
         PFN_vkQueueSubmit2 submit2 = (void *)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
         CHECK(submit2(g_queue, 1, &s, VK_NULL_HANDLE));
      } else {
         VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
            .pCommandBuffers = &g_prebuild_cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_submit_wait};
         CHECK(vkQueueSubmit(g_queue, 1, &s, VK_NULL_HANDLE));
      }
   }
   for (uint32_t i = g_split_builds ? 1 : 0; i < 2; ++i) {
      const VkAccelerationStructureBuildRangeInfoKHR *ranges = g_builds[i]->ranges;
      pBuild(cmd, 1, &g_builds[i]->info, &ranges);
      VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
         .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
      if (g_submit_sync2) {
         VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = b.srcAccessMask, .dstAccessMask = b.dstAccessMask};
         VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
         PFN_vkCmdPipelineBarrier2 barrier2 = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdPipelineBarrier2KHR");
         barrier2(cmd, &dep);
      } else vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
         0, 1, &b, 0, NULL, 0, NULL);
   }
}

static void prepare_as(struct built_as *as, VkAccelerationStructureTypeKHR type,
   uint32_t geometry_count, const VkAccelerationStructureGeometryKHR *geometries,
   const VkAccelerationStructureBuildRangeInfoKHR *ranges, bool pointers)
{
   memset(as, 0, sizeof(*as));
   as->info = (VkAccelerationStructureBuildGeometryInfoKHR){
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = type, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = geometry_count, .pGeometries = geometries};
   memcpy(as->ranges, ranges, geometry_count * sizeof(*ranges));
   uint32_t counts[2];
   const VkAccelerationStructureGeometryKHR *pg[2];
   for (uint32_t i = 0; i < geometry_count; ++i) { counts[i] = ranges[i].primitiveCount; pg[i] = &geometries[i]; }
   if (pointers) { as->info.pGeometries = NULL; as->info.ppGeometries = pg; }
   VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
   PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes = (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
   get_sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as->info, counts, &sizes);
   /* Restore the long-lived geometry pointer before returning. */
   as->info.pGeometries = geometries; as->info.ppGeometries = NULL;
   as->size = sizes.accelerationStructureSize;
   as->offset = 256;
   bool pressure = g_pool_pressure && type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
   VkDeviceSize stride = (as->size + 255) & ~UINT64_C(255);
   create_as_output(pressure ? stride * 1024 + 512 : as->size + 512, &as->output);
   memset(as->output.mapped, 0xa5, (size_t)as->output.size);
   VkAccelerationStructureCreateInfoKHR ci = {
       .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
       .buffer = as->output.buf,
       .offset = 256,
       .size = as->size,
       .type = g_aligned_values || pressure ? VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR : type};
   if (pressure) {
       /* Keep 1024 distinct offsets live while building/querying the last one.
        * This forces pool growth, rather than repeatedly recycling one slot. */
       for (uint32_t i = 0; i < ARRAY_SIZE(g_pressure_views); ++i) {
           ci.offset = 256 + stride * i;
           CHECK(pCreateAS(g_dev, &ci, NULL, &g_pressure_views[i]));
       }
       as->offset = ci.offset = 256 + stride * ARRAY_SIZE(g_pressure_views);
   }
   CHECK(pCreateAS(g_dev, &ci, NULL, &as->handle));
   VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
      .accelerationStructure = as->handle};
   PFN_vkGetAccelerationStructureDeviceAddressKHR address = (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
   as->address = address(g_dev, &ai);
   if (as->address & 255) {
       fprintf(stderr, "FAIL public AS value is not 256-aligned\n");
       ++g_fail;
   }
   if (pressure) {
       ai.accelerationStructure = g_pressure_views[0];
       VkDeviceAddress first = address(g_dev, &ai);
       if (first + stride * ARRAY_SIZE(g_pressure_views) != as->address) ++g_fail;
       VkAccelerationStructureKHR alias;
       CHECK(pCreateAS(g_dev, &ci, NULL, &alias));
       ai.accelerationStructure = alias;
       if (address(g_dev, &ai) != as->address) ++g_fail;
       pDestroyAS(g_dev, as->handle, NULL);
       as->handle = alias;
       /* Retire/recreate holes with neighbors live, before GPU consumption. */
       for (uint32_t i = 1; i < ARRAY_SIZE(g_pressure_views); i += 2) {
           pDestroyAS(g_dev, g_pressure_views[i], NULL);
           ci.offset = 256 + stride * i;
           CHECK(pCreateAS(g_dev, &ci, NULL, &g_pressure_views[i]));
       }
       printf("AS pool pressure: 1024 live offsets, alias survives, holes reused; last offset=%llu\n",
              (unsigned long long)as->offset);
   }
   if (g_aligned_values) {
       VkAccelerationStructureKHR alias, sibling;
       CHECK(pCreateAS(g_dev, &ci, NULL, &alias));
       ci.offset = 0;
       CHECK(pCreateAS(g_dev, &ci, NULL, &sibling));
       ai.accelerationStructure = alias;
       VkDeviceAddress alias_value = address(g_dev, &ai);
       ai.accelerationStructure = sibling;
       VkDeviceAddress sibling_value = address(g_dev, &ai);
       printf("GENERIC values: original=0x%llx alias=0x%llx offset0=0x%llx physical=0x%llx\n", (unsigned long long)as->address,
              (unsigned long long)alias_value, (unsigned long long)sibling_value, (unsigned long long)(as->output.addr + 256));
       if (alias_value != as->address || sibling_value + 256 != as->address) {
           fprintf(stderr, "FAIL GENERIC AS alias/relative-offset address contract\n");
           ++g_fail;
       }
       pDestroyAS(g_dev, sibling, NULL);
       pDestroyAS(g_dev, alias, NULL);
       ai.accelerationStructure = as->handle;
       if (address(g_dev, &ai) != as->address) {
           fprintf(stderr, "FAIL alias destruction changed AS value\n");
           ++g_fail;
       }
   }
   buf_create(sizes.buildScratchSize + 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &as->scratch);
   memset(as->scratch.mapped, 0xa5, (size_t)as->scratch.size);
   as->info.dstAccelerationStructure = as->handle;
   as->info.scratchData.deviceAddress = (as->scratch.addr + 255) & ~(VkDeviceAddress)255;
}

static void destroy_as(struct built_as *as)
{
    if (g_pool_pressure && as->info.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
        for (uint32_t i = 0; i < ARRAY_SIZE(g_pressure_views); ++i) pDestroyAS(g_dev, g_pressure_views[i], NULL);
    pDestroyAS(g_dev, as->handle, NULL);
    buf_destroy(&as->scratch);
    buf_destroy(&as->output);
}

static void rejected_builds(const struct built_as *as)
{
   if (!g_software) return;
   VkCommandBuffer cmd;
   VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(vkAllocateCommandBuffers(g_dev, &alloc, &cmd));
   VkAccelerationStructureKHR small;
   /* A singleton's queried output can be exactly 256 bytes. Use the actual
    * successful build's queried size to guarantee this object is too small. */
   VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                              .buffer = as->output.buf,
                                              .offset = 256,
                                              .size = as->size - 1,
                                              .type = as->info.type};
   CHECK(pCreateAS(g_dev, &ci, NULL, &small));
   for (uint32_t i = 0; i < 2; ++i) {
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      VkAccelerationStructureBuildGeometryInfoKHR info = as->info;
      if (i == 0) { info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR; info.srcAccelerationStructure = as->handle; }
      else info.dstAccelerationStructure = small;
      const VkAccelerationStructureBuildRangeInfoKHR *range = as->ranges;
      pBuild(cmd, 1, &info, &range);
      VkResult result = vkEndCommandBuffer(cmd);
      VkResult expected = i ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
      if (result != expected) { fprintf(stderr, "FAIL rejected build: %d expected %d\n", result, expected); ++g_fail; }
      CHECK(vkResetCommandBuffer(cmd, 0));
   }
   pDestroyAS(g_dev, small, NULL);
   vkFreeCommandBuffers(g_dev, g_pool, 1, &cmd);
}

static void validate_tree(const struct built_as *as, uint32_t expected_leaves)
{
   if (!g_software) return;
   const uint8_t *bytes = as->output.mapped;
   for (uint32_t i = 0; i < 256; ++i) {
      const uint8_t *scratch = as->scratch.mapped;
      if (bytes[as->offset - 256 + i] != 0xa5 || bytes[as->offset + as->size + i] != 0xa5 ||
          scratch[as->scratch.size - 256 + i] != 0xa5) {
          fprintf(stderr, "AS/scratch bounds guard corrupted\n");
          ++g_fail;
          break;
      }
   }
   bytes += as->offset;
   VelRtBvh4Header h;
   memcpy(&h, bytes, sizeof(h));
   expect_u("shared magic", h.magic, VEL_RT_BVH4_MAGIC);
   expect_u("shared version", h.version, VEL_RT_BVH4_ABI_VERSION);
   expect_u("shared type", h.type, as->info.type);
   expect_u("shared status flags", h.flags, 0);
   uint64_t counts[] = {h.node_count, h.triangle_count, h.aabb_count, h.instance_count};
   const uint64_t offsets[] = {h.node_offset, h.triangle_offset, h.aabb_offset, h.instance_offset, h.leaf_parent_offset};
   const uint64_t strides[] = {sizeof(VelRtBvh4Node), sizeof(VelRtBvh4Triangle), sizeof(VelRtBvh4Aabb), sizeof(VelRtBvh4Instance),
                               4};
   uint64_t leaf_count = counts[1] + counts[2] + counts[3], total = counts[0] + leaf_count;
   if (h.magic != VEL_RT_BVH4_MAGIC || h.version != VEL_RT_BVH4_ABI_VERSION || h.flags || h.total_size > as->size ||
       h.total_size < sizeof(h) || total > as->size / 4) {
       fprintf(stderr, "Invalid shared BVH4 header\n");
       ++g_fail;
       return;
   }
   for (unsigned i = 0; i < 5; ++i) {
       uint64_t count = i < 4 ? counts[i] : leaf_count;
       if (offsets[i] > h.total_size || count > (h.total_size - offsets[i]) / strides[i]) {
           fprintf(stderr, "Invalid shared BVH4 region %u\n", i);
           ++g_fail;
           return;
       }
   }
   expect_u("active leaf count", (uint32_t)leaf_count, expected_leaves);
   struct visit {
       uint32_t id, parent;
       float bounds[6];
   };
   struct visit *stack = malloc((size_t)(total + 1) * sizeof(*stack));
   uint8_t *visited = calloc((size_t)(total + 1), 1);
   if (!stack || !visited) {
       free(stack);
       free(visited);
       ++g_fail;
       return;
   }
   uint64_t top = 0;
   uint32_t leaves = 0, nodes = 0;
   stack[top] = (struct visit){.id = h.root_id, .parent = VEL_RT_BVH4_INVALID_ID};
   memcpy(stack[top].bounds, h.bounds_min, 12);
   memcpy(stack[top++].bounds + 3, h.bounds_max, 12);
   while (top) {
       struct visit current = stack[--top];
       uint32_t id = current.id, index = id & VEL_RT_BVH4_INDEX_MASK, kind = id >> VEL_RT_BVH4_KIND_SHIFT;
       if (id == VEL_RT_BVH4_INVALID_ID) continue;
       uint64_t slot = index;
       for (unsigned i = 0; i < kind; ++i) slot += counts[i];
       if (index >= counts[kind] || slot >= total || visited[slot]) {
           fprintf(stderr, "Invalid/cyclic shared node 0x%x\n", id);
           ++g_fail;
           break;
       }
       visited[slot] = 1;
       if (kind == VEL_RT_BVH4_INTERNAL) {
           const VelRtBvh4Node *node = (const void *)(bytes + h.node_offset + (uint64_t)index * sizeof(*node));
           ++nodes;
           expect_u("internal parent", node->parent_id, current.parent);
           if (node->child_count < 2 || node->child_count > 4) {
               ++g_fail;
               break;
           }
           for (uint32_t j = 0; j < node->child_count; ++j) {
               if (top >= total + 1 || node->child_id[j] == VEL_RT_BVH4_INVALID_ID) {
                   ++g_fail;
                   goto finish;
               }
               stack[top] = (struct visit){.id = node->child_id[j], .parent = id};
               memcpy(stack[top].bounds, node->bounds_min[j], 12);
               memcpy(stack[top].bounds + 3, node->bounds_max[j], 12);
               for (unsigned axis = 0; axis < 3; ++axis)
                   if (current.bounds[axis] > stack[top].bounds[axis] + 1e-4f ||
                       current.bounds[axis + 3] < stack[top].bounds[axis + 3] - 1e-4f)
                       ++g_fail;
               ++top;
           }
       } else {
           ++leaves;
           uint32_t parent;
           memcpy(&parent, bytes + h.leaf_parent_offset + (slot - counts[0]) * 4, 4);
           expect_u("leaf parent", parent, current.parent);
           if (kind == VEL_RT_BVH4_TRIANGLE || kind == VEL_RT_BVH4_AABB) {
               uint64_t offset = offsets[kind] + (uint64_t)index * strides[kind];
               uint32_t primitive, geometry, flags;
               float bounds[6] = {INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
               if (kind == VEL_RT_BVH4_TRIANGLE) {
                   const VelRtBvh4Triangle *leaf = (const void *)(bytes + offset);
                   primitive = leaf->primitive_id;
                   geometry = leaf->geometry_id;
                   flags = leaf->geometry_flags;
                   for (unsigned vertex = 0; vertex < 3; ++vertex)
                       for (unsigned axis = 0; axis < 3; ++axis) {
                           bounds[axis] = fminf(bounds[axis], leaf->vertices[vertex][axis]);
                           bounds[axis + 3] = fmaxf(bounds[axis + 3], leaf->vertices[vertex][axis]);
                       }
               } else {
                   const VelRtBvh4Aabb *leaf = (const void *)(bytes + offset);
                   primitive = leaf->primitive_id;
                   geometry = leaf->geometry_id;
                   flags = leaf->geometry_flags;
                   memcpy(bounds, leaf->bounds_min, 12);
                   memcpy(bounds + 3, leaf->bounds_max, 12);
               }
               if (geometry >= as->info.geometryCount) {
                   ++g_fail;
                   goto finish;
               }
               const VkAccelerationStructureGeometryKHR *input =
                   as->info.pGeometries ? &as->info.pGeometries[geometry] : as->info.ppGeometries[geometry];
               expect_u("full geometry flags", flags, input->flags);
               if (primitive >= as->ranges[geometry].primitiveCount) ++g_fail;
               for (unsigned axis = 0; axis < 3; ++axis)
                   if (current.bounds[axis] > bounds[axis] + 1e-4f || current.bounds[axis + 3] < bounds[axis + 3] - 1e-4f) ++g_fail;
           }
       }
   }
finish:
   expect_u("reachable leaves", leaves, expected_leaves);
   expect_u("reachable internal nodes", nodes, h.node_count);
   free(visited); free(stack);
}

static void triangles_case(uint32_t n, bool inactive, bool indexed, bool two_geometries, bool transformed)
{
   printf("GPU BUILD CASE: n=%u inactive=%d indexed=%d multi=%d transform=%d\n",
      n, inactive, indexed, two_geometries, transformed);
   uint32_t count = n ? n : 1;
   struct gpu_buf vertices, indices, transforms, instances;
   buf_create((VkDeviceSize)(count * 3 + 1) * 12, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertices);
   float *v = vertices.mapped;
   memset(v, 0, (size_t)vertices.size);
   for (uint32_t i = 0; i < n; ++i) {
      float x = (float)i * 3;
      float triangle[9] = {x-1,-1,0, x+1,-1,0, x,1,0};
      if (inactive && i % 7 == 0) triangle[0] = NAN;
      memcpy(v + 3 + i * 9, triangle, sizeof(triangle));
   }
   buf_create((VkDeviceSize)(count * 3 + 2) * 2, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &indices);
   uint16_t *ix = indices.mapped;
   for (uint32_t i = 0; i < count * 3; ++i) ix[2 + i] = (uint16_t)i;
   buf_create(64, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &transforms);
   float matrix[12] = {1,0,0,0, 0,1,0,0, 0,0,1, transformed ? 1.f : 0.f};
   memcpy((uint8_t *)transforms.mapped + 16, matrix, sizeof(matrix));
   uint32_t split = two_geometries ? n / 2 : n;
   VkAccelerationStructureGeometryKHR geoms[2];
   VkAccelerationStructureBuildRangeInfoKHR ranges[2];
   for (uint32_t i = 0; i < 2; ++i) {
       geoms[i] = (VkAccelerationStructureGeometryKHR){
           .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
           .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
           .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR | (n == 1 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0),
           .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                  .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                  .vertexData.deviceAddress = vertices.addr,
                                  .vertexStride = 12,
                                  .maxVertex = count * 3,
                                  .indexType = indexed ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_NONE_KHR,
                                  .indexData.deviceAddress = indices.addr,
                                  .transformData.deviceAddress = transformed ? transforms.addr : 0}};
       uint32_t first = i ? split : 0;
       ranges[i] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = i ? n - split : split,
                                                              .primitiveOffset = indexed ? 4 + first * 6 : 12 + first * 36,
                                                              .firstVertex = indexed ? 1 : 0,
                                                              .transformOffset = transformed ? 16 : 0};
   }
   struct built_as blas, tlas;
   prepare_as(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, two_geometries ? 2 : 1, geoms, ranges, two_geometries);
   buf_create(128, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instances);
   VkAccelerationStructureInstanceKHR instance = {.transform = {.matrix = {{1,0,0,0},{0,1,0,0},{0,0,1,0}}},
      .instanceCustomIndex = 42, .mask = 0xff, .accelerationStructureReference = blas.address};
   if (transformed) { instance.transform.matrix[0][0] = 2; instance.transform.matrix[1][3] = 4; instance.transform.matrix[2][3] = -1; }
   memcpy((uint8_t *)instances.mapped + 16, &instance, sizeof(instance));
   VkAccelerationStructureGeometryKHR inst_geom = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
         .data.deviceAddress = instances.addr}};
   VkAccelerationStructureBuildRangeInfoKHR inst_range = {.primitiveCount = 1, .primitiveOffset = 16};
   prepare_as(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1, &inst_geom, &inst_range, false);
   g_builds[0] = &blas; g_builds[1] = &tlas;
   g_before_dispatch = record_builds;
   const VkAccelerationStructureGeometryKHR *pgeoms[] = {&geoms[0], &geoms[1]};
   if (two_geometries) { blas.info.pGeometries = NULL; blas.info.ppGeometries = pgeoms; }
   uint32_t targets[] = {0, n > 1 ? n - 1 : 0, n / 2};
   uint32_t random = 0x12345678;
   for (uint32_t j = 0; j < 15; ++j) {
      random = random * 1664525u + 1013904223u;
      uint32_t target = j < 3 ? targets[j] : random % count;
      float u = j < 3 ? .25f : .1f + .3f * (float)(random & 65535) / 65535.f;
      float bv = j < 3 ? .5f : .1f + .3f * (float)(random >> 16) / 65535.f;
      float direction[] = {j < 3 ? 0 : .137f, j < 3 ? 0 : -.071f, -1};
      bool outside = j >= 3 && j % 4 == 0;
      float origin[] = {((float)target * 3 + 2 * u + bv - 1) * (transformed ? 2 : 1) - 2 * direction[0],
         (outside ? 3 : 2 * bv - 1) + (transformed ? 4.f : 0.f) - 2 * direction[1], 2};
      struct out_block out;
      run_ray(tlas.handle, origin, direction, 0, 100, 0, 0xff, &out);
      bool hit = n && !outside && !(inactive && target % 7 == 0);
      expect_u("GPU-built hit", out.hit_type, hit);
      expect_u("GPU-built shadow", out.shadow_hit, hit);
      if (hit) {
         uint32_t geom = two_geometries && target >= split;
         expect_u("original primitive", out.primitive_index, target - (geom ? split : 0));
         expect_u("geometry", out.geometry_index, geom);
         expect_u("instance", out.instance_index, 0);
         expect_u("instance custom", out.instance_custom_index, 42);
         expect_f("distance", out.t, 2, 1e-4f);
         expect_f("bary u", out.bary[0], u, 3e-4f);
         expect_f("bary v", out.bary[1], bv, 3e-4f);
      }
      /* Re-recording builds on every ray also tests scratch reinitialization. */
      if (j == 2) g_before_dispatch = NULL;
   }
   if (n && !inactive) {
      const float origin[] = {0, transformed ? 4.f : 0.f, 2}, direction[] = {0,0,-1};
      const uint32_t flags[] = {0x40, 0x80, 0x10, 0x20, 0x01, 0x02};
      const uint32_t expected[] = {n != 1, n == 1, 1, 0, 1, 1};
      for (uint32_t j = 0; j < ARRAY_SIZE(flags); ++j) {
         struct out_block out;
         run_ray(tlas.handle, origin, direction, 0, 100, flags[j], 0xff, &out);
         expect_u("ray culling/opacity flags", out.hit_type, expected[j]);
      }
   }
   uint32_t active = n - (inactive ? (n + 6) / 7 : 0);
   validate_tree(&blas, active);
   validate_tree(&tlas, active ? 1 : 0);
   if (n == 1 && !g_aligned_values && !g_pool_pressure) rejected_builds(&blas);
   g_before_dispatch = NULL;
   destroy_as(&tlas); destroy_as(&blas);
   buf_destroy(&instances); buf_destroy(&transforms); buf_destroy(&indices); buf_destroy(&vertices);
}

static void aabb_instances_case(void)
{
   printf("GPU BUILD CASE: procedural AABBs + instance pointers/masks/resume\n");
   struct gpu_buf boxes, instances, pointers;
   buf_create(3 * 32 + 8, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &boxes);
   for (uint32_t i = 0; i < 3; ++i) {
      float box[] = {(float)i * 3 - 1, -1, -.25f, (float)i * 3 + 1, 1, .25f};
      if (i == 2) box[0] = NAN;
      memcpy((uint8_t *)boxes.mapped + 8 + i * 32, box, sizeof(box));
   }
   VkAccelerationStructureGeometryKHR box_geom = {
       .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
       .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
       .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR,
       .geometry.aabbs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                          .data.deviceAddress = boxes.addr,
                          .stride = 32}};
   VkAccelerationStructureBuildRangeInfoKHR box_range = {.primitiveCount = 3, .primitiveOffset = 8};
   struct built_as blas, tlas;
   prepare_as(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 1, &box_geom, &box_range, false);
   buf_create(5 * 64, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instances);
   buf_create(6 * 8, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &pointers);
   for (uint32_t i = 0; i < 5; ++i) {
      VkAccelerationStructureInstanceKHR instance = {.transform = {.matrix = {{1,0,0,0},{0,1,0,0},{0,0,1,0}}},
         .instanceCustomIndex = 100 + i, .mask = i == 1 ? 0 : 1u << i,
         .accelerationStructureReference = i == 0 ? 0 : blas.address};
      if (i == 2) instance.transform.matrix[0][3] = 100;
      if (i == 4) instance.transform.matrix[2][3] = -2;
      memcpy((uint8_t *)instances.mapped + i * 64, &instance, sizeof(instance));
      ((uint64_t *)pointers.mapped)[1 + i] = instances.addr + i * 64;
   }
   VkAccelerationStructureGeometryKHR inst_geom = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
         .arrayOfPointers = VK_TRUE, .data.deviceAddress = pointers.addr}};
   VkAccelerationStructureBuildRangeInfoKHR inst_range = {.primitiveCount = 5, .primitiveOffset = 8};
   prepare_as(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1, &inst_geom, &inst_range, false);
   g_builds[0] = &blas; g_builds[1] = &tlas; g_before_dispatch = record_builds;
   const uint32_t masks[] = {0xff, 1u << 4, 1u << 3, 1u << 2, 1u << 1, 0};
   for (uint32_t j = 0; j < ARRAY_SIZE(masks); ++j) {
      struct out_block out;
      const float origin[] = {3,0,2}, direction[] = {0,0,-1};
      run_ray(tlas.handle, origin, direction, 0, 100, 0, masks[j], &out);
      bool hit = j < 3;
      expect_u("procedural hit", out.hit_type, hit ? 2 : 0);
      expect_u("procedural shadow", out.shadow_hit, hit);
      if (hit) {
         uint32_t instance = j == 1 ? 4 : 3;
         expect_u("procedural primitive", out.primitive_index, 1);
         expect_u("procedural instance ID", out.instance_index, instance);
         expect_u("procedural custom ID", out.instance_custom_index, 100 + instance);
         expect_f("procedural distance", out.t, j == 1 ? 4 : 2, 1e-5f);
      }
      g_before_dispatch = NULL;
   }
   validate_tree(&blas, 2); validate_tree(&tlas, 3);
   destroy_as(&tlas); destroy_as(&blas);
   buf_destroy(&pointers); buf_destroy(&instances); buf_destroy(&boxes);
}

int main(int argc, char **argv) {
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    g_software = layers && strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY");
    g_aligned_values = argc == 2 && !strcmp(argv[1], "--aligned-values");
    g_pool_pressure = argc == 2 && !strcmp(argv[1], "--pool-pressure");
    init_vulkan();
    pBuild = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    if (!pBuild) return 1;
    if (g_aligned_values || g_pool_pressure) {
        triangles_case(1, false, false, false, false);
        goto finish;
    }
    triangles_case(0, false, false, false, false);
    triangles_case(1, false, false, false, false);
    triangles_case(2, false, true, true, true);
    triangles_case(257, true, true, true, false);
    g_split_builds = true;
    triangles_case(1025, false, false, false, true);
    g_submit_sync2 = true;
    aabb_instances_case();
finish:
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("GPU BUILD %s (%d failures, %s)\n", g_fail ? "FAIL" : "PASS", g_fail, g_software ? "software" : "native");
    return g_fail ? 1 : 0;
}
