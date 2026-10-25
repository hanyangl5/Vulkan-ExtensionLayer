/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"

/* Mock only the downstream dispatch; exercise the actual layer entry points. */
struct vkrt_global vkrt_global;
static unsigned failures, updates, allocations;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define HANDLE(type, value) ((type)(uintptr_t)(value))
static const VkDevice device = HANDLE(VkDevice, 1);
static const VkWriteDescriptorSet *original_writes;
static const VkCopyDescriptorSet *original_copies;
static struct vkrt_virtual_as objects[3];
static unsigned update_mode;

static VKAPI_ATTR void VKAPI_CALL mock_update(VkDevice d, uint32_t count,
   const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
   ++updates;
   REQUIRE(d == device);
   REQUIRE(copy_count == 1 && copies == original_copies);
   if (update_mode == 0) {
      REQUIRE(count == 3);
      REQUIRE(!memcmp(&writes[1], &original_writes[1], sizeof(writes[1])));
      for (unsigned i = 0; i < 3; i += 2) {
         REQUIRE(writes[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
         REQUIRE(writes[i].dstSet == original_writes[i].dstSet);
         REQUIRE(writes[i].dstBinding == original_writes[i].dstBinding);
         REQUIRE(writes[i].dstArrayElement == original_writes[i].dstArrayElement);
         REQUIRE(writes[i].descriptorCount == original_writes[i].descriptorCount);
         REQUIRE(!writes[i].pNext && !writes[i].pImageInfo && !writes[i].pTexelBufferView);
      }
      const unsigned indices[] = {0, 1, 2, 2, 0};
      for (unsigned j = 0; j < 5; ++j) {
         const VkDescriptorBufferInfo *b = j < 3 ? &writes[0].pBufferInfo[j] : &writes[2].pBufferInfo[j - 3];
         REQUIRE(b->buffer == objects[indices[j]].backing_buffer);
         REQUIRE(b->offset == objects[indices[j]].offset);
         REQUIRE(b->range == objects[indices[j]].size);
      }
   } else if (update_mode == 1) {
      REQUIRE(count == 1 && writes[0].descriptorCount == 2);
      REQUIRE(writes[0].pBufferInfo[0].buffer == VK_NULL_HANDLE);
      REQUIRE(writes[0].pBufferInfo[0].offset == 0);
      REQUIRE(writes[0].pBufferInfo[0].range == VK_WHOLE_SIZE);
      REQUIRE(writes[0].pBufferInfo[1].buffer == objects[1].backing_buffer);
   } else {
      REQUIRE(writes == original_writes);
   }
}

static const VkMemoryAllocateInfo *original_alloc;
static const VkMemoryDedicatedAllocateInfo *dedicated;
static const VkMemoryOpaqueCaptureAddressAllocateInfo *suffix;
static VkMemoryAllocateFlags expected_flags;
static uint32_t expected_mask;
static VkResult allocation_result;
static VKAPI_ATTR VkResult VKAPI_CALL mock_allocate(VkDevice d, const VkMemoryAllocateInfo *info,
   const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
   ++allocations;
   REQUIRE(d == device && allocator == NULL);
   REQUIRE(info->allocationSize == original_alloc->allocationSize);
   REQUIRE(info->memoryTypeIndex == original_alloc->memoryTypeIndex);
   unsigned flag_count = 0, dedicated_count = 0, suffix_count = 0;
   for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
      if (p->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
         const VkMemoryAllocateFlagsInfo *f = (const void *)p;
         ++flag_count;
         REQUIRE(f->flags == expected_flags && f->deviceMask == expected_mask);
      } else if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
         const VkMemoryDedicatedAllocateInfo *m = (const void *)p;
         ++dedicated_count;
         REQUIRE(dedicated && m->buffer == dedicated->buffer && m->image == dedicated->image);
      } else if (p->sType == VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO) {
         ++suffix_count;
         REQUIRE((const void *)p == (const void *)suffix);
      } else REQUIRE(false);
   }
   REQUIRE(flag_count == 1);
   REQUIRE(dedicated_count == (dedicated ? 1u : 0u));
   REQUIRE(suffix_count == (suffix ? 1u : 0u));
   *memory = HANDLE(VkDeviceMemory, 123);
   return allocation_result;
}

static void descriptors(void)
{
   VkAccelerationStructureKHR handles[5];
   for (unsigned i = 0; i < 3; ++i) {
      objects[i] = (struct vkrt_virtual_as){.owner = device,
         .backing_buffer = HANDLE(VkBuffer, 100 + i), .offset = 256 * (i + 1), .size = 512 * (i + 1)};
      handles[i] = HANDLE(VkAccelerationStructureKHR, 200 + i);
      vkrt_map_object(VKRT_HKEY(handles[i]), &objects[i]);
   }
   handles[3] = handles[2]; handles[4] = handles[0];
   VkWriteDescriptorSetAccelerationStructureKHR chains[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, .accelerationStructureCount = 3, .pAccelerationStructures = handles},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, .accelerationStructureCount = 2, .pAccelerationStructures = handles + 3}};
   VkDescriptorBufferInfo ordinary = {HANDLE(VkBuffer, 77), 16, 64};
   VkWriteDescriptorSet writes[3] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &chains[0], .dstSet = HANDLE(VkDescriptorSet, 9),
         .dstBinding = 7, .dstArrayElement = 2, .descriptorCount = 3, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ordinary},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &chains[1], .dstSet = HANDLE(VkDescriptorSet, 10),
         .dstBinding = 11, .dstArrayElement = 4, .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}};
   VkWriteDescriptorSet snapshot[3]; memcpy(snapshot, writes, sizeof(writes));
   VkCopyDescriptorSet copy = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .descriptorCount = 1};
   original_writes = writes; original_copies = &copy;
   vkrt_UpdateDescriptorSets(device, 3, writes, 1, &copy);
   REQUIRE(updates == 1 && !memcmp(snapshot, writes, sizeof(writes)));
   REQUIRE(chains[0].pAccelerationStructures == handles && chains[0].accelerationStructureCount == 3);

   update_mode = 1;
   handles[0] = VK_NULL_HANDLE;
   chains[0].accelerationStructureCount = writes[0].descriptorCount = 2;
   vkrt_UpdateDescriptorSets(device, 1, writes, 1, &copy);
   REQUIRE(updates == 2);
   /* Bad counts/handles must not be forwarded as zero-count or partial writes. */
   chains[0].accelerationStructureCount = 1;
   vkrt_UpdateDescriptorSets(device, 1, writes, 1, &copy);
   REQUIRE(updates == 2);
   chains[0].accelerationStructureCount = 2;
   handles[1] = HANDLE(VkAccelerationStructureKHR, 999);
   vkrt_UpdateDescriptorSets(device, 1, writes, 1, &copy);
   REQUIRE(updates == 2);
   update_mode = 2;
   original_writes = &writes[1];
   vkrt_UpdateDescriptorSets(device, 1, original_writes, 1, &copy);
   original_writes = NULL;
   vkrt_UpdateDescriptorSets(device, 0, NULL, 1, &copy);
   REQUIRE(updates == 4);
}

static void memory_flags(void)
{
   VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096, .memoryTypeIndex = 3};
   original_alloc = &info;
   expected_flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
   VkDeviceMemory memory;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(info.pNext == NULL && memory == HANDLE(VkDeviceMemory, 123));
   VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
   info.pNext = &flags;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(flags.flags == 0 && flags.pNext == NULL);

   VkMemoryOpaqueCaptureAddressAllocateInfo tail = {.sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
      .opaqueCaptureAddress = 0x123456789abcdef0ULL};
   VkMemoryDedicatedAllocateInfo prefix = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &flags, .buffer = HANDLE(VkBuffer, 456)};
   flags.flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
   flags.deviceMask = 3; flags.pNext = &tail;
   info.pNext = &prefix;
   dedicated = &prefix; suffix = &tail;
   expected_flags |= flags.flags; expected_mask = flags.deviceMask;
   VkMemoryAllocateFlagsInfo saved = flags;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(!memcmp(&saved, &flags, sizeof(flags)) && prefix.pNext == &flags && info.pNext == &prefix);
   allocation_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   REQUIRE(!memcmp(&saved, &flags, sizeof(flags)));
   allocation_result = VK_SUCCESS;
   flags.flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_SUCCESS);
   REQUIRE(allocations == 5);

   /* An unrecognized prefix cannot be copied safely: reject without changing
    * the caller's chain or forwarding a partially cloned allocation. */
   VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM, .pNext = (const void *)&flags};
   flags.flags = 0;
   prefix.pNext = &unknown;
   REQUIRE(vkrt_AllocateMemory(device, &info, NULL, &memory) == VK_ERROR_EXTENSION_NOT_PRESENT);
   REQUIRE(allocations == 5 && flags.flags == 0 && prefix.pNext == &unknown);
   REQUIRE(unknown.pNext == (const void *)&flags);
}

int main(void)
{
   struct vkrt_device_data dev = {.device = device};
   dev.vtable.UpdateDescriptorSets = mock_update;
   dev.vtable.AllocateMemory = mock_allocate;
   vkrt_map_object(VKRT_HKEY(device), &dev);
   descriptors();
   memory_flags();
   printf("Host translation: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
