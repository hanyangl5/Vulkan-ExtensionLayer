/* SPDX-License-Identifier: MIT
 * Public command entry points against a native spy. These checks exercise
 * dynamic maintenance6 layouts on machines without that optional NV feature.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
extern "C" {
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"
#include "vkrt_deferred.h"
#include "vkrt_rtp.h"
#include "vkrt_debug_objects.h"
#include "typed_runtime_abi.h"
struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
}
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <string>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>

namespace {
std::atomic_uint failures{0};
unsigned forwarded = 0, create_attempts = 0, fail_at = 0;
#define CHECK(value)                                                 \
    do {                                                             \
        if (!(value)) {                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                              \
        }                                                            \
    } while (0)
template <class H>
H Handle(uintptr_t value) {
    return (H)value;
}
template <class T>
std::array<unsigned char, sizeof(T)> Snapshot(const T &value) {
    std::array<unsigned char, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}
const VkDevice device = Handle<VkDevice>(1);
const VkCommandBuffer command = Handle<VkCommandBuffer>(2);
const VkPhysicalDevice physical = Handle<VkPhysicalDevice>(3);
struct NativeLayout {
    VkPipelineLayoutCreateFlags flags = 0;
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> pushes;
    unsigned references = 1;
};
std::map<VkPipelineLayout, NativeLayout> layouts;
std::set<VkPipelineLayout> destroyed;
std::mutex native_layout_lock;
uintptr_t next_layout = 100;
VkPipelineLayout alias_handle{};
bool alias_private = false;
struct CallbackOwner {
    bool live = true, device = false;
    int budget = -1;
    unsigned allocations = 0, frees = 0, calls = 0;
};
std::mutex callback_lock;
std::map<void *, CallbackOwner *> callback_allocations;
VKAPI_ATTR void *VKAPI_CALL Allocate(void *user, size_t size, size_t, VkSystemAllocationScope scope) {
    std::lock_guard<std::mutex> lock(callback_lock);
    auto &owner = *static_cast<CallbackOwner *>(user);
    CHECK(owner.live && scope == (owner.device ? VK_SYSTEM_ALLOCATION_SCOPE_DEVICE : VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    ++owner.calls;
    if (!owner.budget) return nullptr;
    if (owner.budget > 0) --owner.budget;
    void *p = std::malloc(size);
    if (p) {
        ++owner.allocations;
        CHECK(callback_allocations.emplace(p, &owner).second);
    }
    return p;
}
VKAPI_ATTR void VKAPI_CALL Free(void *user, void *memory) {
    if (!memory) return;
    std::lock_guard<std::mutex> lock(callback_lock);
    auto &owner = *static_cast<CallbackOwner *>(user);
    CHECK(owner.live);
    ++owner.calls;
    auto i = callback_allocations.find(memory);
    CHECK(i != callback_allocations.end());
    if (i != callback_allocations.end()) {
        // Object callback contexts use compatible malloc/free implementations;
        // DEVICE bookkeeping must retain its actual lifetime owner.
        CHECK(owner.device == i->second->device && (!owner.device || &owner == i->second));
        ++i->second->frees;
        callback_allocations.erase(i);
    }
    std::free(memory);
}
VKAPI_ATTR void *VKAPI_CALL Reallocate(void *, void *, size_t, size_t, VkSystemAllocationScope) {
    CHECK(false);  // This fixture does not require a native reallocation.
    return nullptr;
}
VkAllocationCallbacks Callbacks(CallbackOwner &owner) {
    VkAllocationCallbacks result{};
    result.pUserData = &owner;
    result.pfnAllocation = Allocate;
    result.pfnReallocation = Reallocate;
    result.pfnFree = Free;
    return result;
}
std::mutex retirement_lock;
std::condition_variable retirement_changed;
bool gate_retirement = false, retirement_entered = false, release_retirement = false, creation_entered = false;
VkShaderStageFlags expected_stages = 0;
uint32_t expected_push_bytes = 256;
const VkBindDescriptorSetsInfo *passthrough = nullptr;
VkBindDescriptorSetsInfo observed{};
std::vector<VkDescriptorSet> observed_sets;
std::vector<uint32_t> observed_offsets;
vkrt_device_data dev{};
vkrt_instance_data instance{};
vkrt_cmd_buffer_data cb{};
uintptr_t next_native = 10000;
unsigned native_creates = 0, fail_native_at = 0;
std::set<VkShaderModule> native_shaders;
std::set<VkPipeline> native_pipelines;
std::vector<VkPipelineCreateFlags> native_flags;
std::vector<VkPipelineCache> native_caches;
struct MockLease {
    vkrt_private_memory_class memory_class;
    vkrt_private_slice slice;
    std::vector<uint8_t> host;
};
std::map<vkrt_private_allocation *, MockLease> leases;
uint64_t arena_ceiling = UINT64_MAX;
unsigned arena_attempts = 0, context_attempts = 0;
std::vector<uint32_t> dispatches;
std::vector<std::array<uint64_t, 2>> trace_pushes;
std::vector<VkBufferMemoryBarrier> trace_barriers;
struct DescriptorEvent {
    std::string kind;
    VkPipelineLayout layout{};
    std::vector<uint64_t> values;
    bool operator==(const DescriptorEvent &other) const {
        return kind == other.kind && layout == other.layout && values == other.values;
    }
};
bool descriptor_history_test = false;
std::vector<DescriptorEvent> descriptor_events;
std::vector<VkDescriptorBufferBindingInfoEXT> native_descriptor_buffers;
std::map<std::pair<VkPipelineBindPoint, uint32_t>, std::pair<uint32_t, VkDeviceSize>> native_descriptor_offsets;
std::set<std::pair<VkPipelineBindPoint, uint32_t>> native_embedded_samplers;
VKAPI_ATTR void VKAPI_CALL NativeDescriptorBuffers(VkCommandBuffer c, uint32_t count,
                                                   const VkDescriptorBufferBindingInfoEXT *infos) {
    CHECK(c == command && count && infos);
    native_descriptor_buffers.assign(infos, infos + count);
    // Vulkan's global BindDescriptorBuffers invalidates offsets at both native
    // bind points; preserving only compute offsets cannot restore later Draw.
    native_descriptor_offsets.clear();
    DescriptorEvent event{"buffers", VK_NULL_HANDLE, {count}};
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(infos[i].sType == VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT && !infos[i].pNext);
        event.values.insert(event.values.end(), {infos[i].address, infos[i].usage});
    }
    if (descriptor_history_test) descriptor_events.push_back(std::move(event));
}
VKAPI_ATTR void VKAPI_CALL NativePush(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set,
                                      uint32_t count, const VkWriteDescriptorSet *writes) {
    CHECK(c == command && point == VK_PIPELINE_BIND_POINT_COMPUTE && layouts.count(layout));
    DescriptorEvent event{"push", layout, {uint64_t(point), set, count}};
    for (uint32_t n = 0; n < count; ++n) {
        const auto &w = writes[n];
        CHECK(w.sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET && !w.pNext &&
              w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && w.pBufferInfo);
        event.values.insert(event.values.end(), {w.dstBinding, w.dstArrayElement, uint64_t(w.descriptorType), w.descriptorCount});
        for (uint32_t i = 0; i < w.descriptorCount; ++i)
            event.values.insert(event.values.end(),
                                {VKRT_HKEY(w.pBufferInfo[i].buffer), w.pBufferInfo[i].offset, w.pBufferInfo[i].range});
    }
    if (descriptor_history_test) descriptor_events.push_back(std::move(event));
}
VKAPI_ATTR void VKAPI_CALL NativePush2(VkCommandBuffer c, const VkPushDescriptorSetInfo *info) {
    CHECK(info && !info->pNext && info->stageFlags == VK_SHADER_STAGE_COMPUTE_BIT);
    NativePush(c, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites);
}
VKAPI_ATTR void VKAPI_CALL NativeOffsets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                                         uint32_t count, const uint32_t *indices, const VkDeviceSize *offsets) {
    CHECK(c == command && (point == VK_PIPELINE_BIND_POINT_COMPUTE || point == VK_PIPELINE_BIND_POINT_GRAPHICS) &&
          layouts.count(layout));
    DescriptorEvent event{"offsets", layout, {uint64_t(point), first, count}};
    for (uint32_t n = 0; n < count; ++n) {
        CHECK(indices[n] < native_descriptor_buffers.size());
        native_descriptor_offsets[{point, first + n}] = {indices[n], offsets[n]};
        event.values.insert(event.values.end(), {indices[n], offsets[n]});
    }
    if (descriptor_history_test) descriptor_events.push_back(std::move(event));
}
VKAPI_ATTR void VKAPI_CALL NativeOffsets2(VkCommandBuffer c, const VkSetDescriptorBufferOffsetsInfoEXT *info) {
    CHECK(info && !info->pNext);
    const bool all = info->stageFlags == VK_SHADER_STAGE_ALL;
    if (all || (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT))
        NativeOffsets(c, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, info->setCount, info->pBufferIndices,
                      info->pOffsets);
    if (all || (info->stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT))
        NativeOffsets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, info->setCount, info->pBufferIndices,
                      info->pOffsets);
}
VKAPI_ATTR void VKAPI_CALL NativeSamplers(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set) {
    CHECK(c == command && (point == VK_PIPELINE_BIND_POINT_COMPUTE || point == VK_PIPELINE_BIND_POINT_GRAPHICS) &&
          layouts.count(layout));
    native_embedded_samplers.insert({point, set});
    if (descriptor_history_test) descriptor_events.push_back({"samplers", layout, {uint64_t(point), set}});
}
VKAPI_ATTR void VKAPI_CALL NativeSamplers2(VkCommandBuffer c, const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *info) {
    CHECK(info && !info->pNext);
    const bool all = info->stageFlags == VK_SHADER_STAGE_ALL;
    if (all || (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT))
        NativeSamplers(c, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->set);
    if (all || (info->stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT))
        NativeSamplers(c, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->set);
}
void NativeDrawDescriptorOracle() {
    const auto found = native_descriptor_offsets.find({VK_PIPELINE_BIND_POINT_GRAPHICS, 0});
    CHECK(found != native_descriptor_offsets.end());
    if (found != native_descriptor_offsets.end()) {
        const auto [index, offset] = found->second;
        CHECK(index < native_descriptor_buffers.size());
        if (index < native_descriptor_buffers.size())
            CHECK(native_descriptor_buffers[index].address + offset == UINT64_C(0xc00060040));
    }
    CHECK(native_embedded_samplers.count({VK_PIPELINE_BIND_POINT_GRAPHICS, 0}) == 1);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeProc(VkDevice d, const char *name) {
    CHECK(d == device);
    if (!std::strcmp(name, "vkCmdBindDescriptorBuffersEXT")) return (PFN_vkVoidFunction)NativeDescriptorBuffers;
    if (!std::strcmp(name, "vkCmdPushDescriptorSet") || !std::strcmp(name, "vkCmdPushDescriptorSetKHR"))
        return (PFN_vkVoidFunction)NativePush;
    if (!std::strcmp(name, "vkCmdPushDescriptorSet2") || !std::strcmp(name, "vkCmdPushDescriptorSet2KHR"))
        return (PFN_vkVoidFunction)NativePush2;
    if (!std::strcmp(name, "vkCmdSetDescriptorBufferOffsetsEXT")) return (PFN_vkVoidFunction)NativeOffsets;
    if (!std::strcmp(name, "vkCmdSetDescriptorBufferOffsets2EXT")) return (PFN_vkVoidFunction)NativeOffsets2;
    if (!std::strcmp(name, "vkCmdBindDescriptorBufferEmbeddedSamplersEXT")) return (PFN_vkVoidFunction)NativeSamplers;
    if (!std::strcmp(name, "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT")) return (PFN_vkVoidFunction)NativeSamplers2;
    return nullptr;
}
VKAPI_ATTR void VKAPI_CALL BindNativeCompute(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline pipeline) {
    CHECK(c == command && point == VK_PIPELINE_BIND_POINT_COMPUTE && native_pipelines.count(pipeline));
}
VKAPI_ATTR void VKAPI_CALL TracePush(VkCommandBuffer c, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                     uint32_t bytes, const void *data) {
    CHECK(c == command && layouts.count(layout) && stages == VK_SHADER_STAGE_COMPUTE_BIT && !offset && bytes == 16);
    std::array<uint64_t, 2> values{};
    std::memcpy(values.data(), data, sizeof(values));
    trace_pushes.push_back(values);
}
VKAPI_ATTR void VKAPI_CALL TraceDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
    CHECK(c == command && x && y == 1 && z == 1);
    dispatches.push_back(x);
    if (descriptor_history_test) descriptor_events.push_back({"dispatch", VK_NULL_HANDLE, {x}});
}
VKAPI_ATTR void VKAPI_CALL TraceBarrier(VkCommandBuffer c, VkPipelineStageFlags source, VkPipelineStageFlags target,
                                        VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier *, uint32_t count,
                                        const VkBufferMemoryBarrier *barriers, uint32_t image_count, const VkImageMemoryBarrier *) {
    CHECK(c == command && target == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && !flags && !memory_count && count == 1 && !image_count);
    CHECK(source == VK_PIPELINE_STAGE_HOST_BIT || source == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    CHECK(barriers->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && barriers->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    if (source == VK_PIPELINE_STAGE_HOST_BIT)
        CHECK(barriers->srcAccessMask == VK_ACCESS_HOST_WRITE_BIT && barriers->dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
    else
        CHECK(barriers->srcAccessMask == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) &&
              barriers->dstAccessMask == barriers->srcAccessMask);
    trace_barriers.push_back(*barriers);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateNativeShader(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                  VkShaderModule *out) {
    CHECK(d == device && !a && ci->codeSize && ci->pCode);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    CHECK(validator.Validate(ci->pCode, ci->codeSize / sizeof(uint32_t)));
    uint32_t physical_accesses = 0;
    for (size_t p = 5; p < ci->codeSize / sizeof(uint32_t); p += ci->pCode[p] >> 16) {
        if ((ci->pCode[p] & 65535) == spv::OpConvertUToPtr) ++physical_accesses;
        if ((ci->pCode[p] & 65535) == spv::OpName)
            CHECK(std::strcmp(reinterpret_cast<const char *>(ci->pCode + p + 2), "vel_rt_private_frames") != 0);
    }
    CHECK(physical_accesses != 0);
    *out = Handle<VkShaderModule>(next_native++);
    CHECK(native_shaders.insert(*out).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyNativeShader(VkDevice d, VkShaderModule shader, const VkAllocationCallbacks *a) {
    CHECK(d == device && !a && native_shaders.erase(shader) == 1);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateNativePipeline(VkDevice d, VkPipelineCache cache, uint32_t count,
                                                    const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                    VkPipeline *out) {
    CHECK(d == device && count == 1 && !a);
    CHECK(!(ci->flags & ~VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) && !ci->basePipelineHandle);
    CHECK(ci->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && native_shaders.count(ci->stage.module));
    CHECK(layouts.count(ci->layout) && layouts.at(ci->layout).pushes.size() == 1 && layouts.at(ci->layout).pushes[0].size == 16);
    *out = VK_NULL_HANDLE;
    native_flags.push_back(ci->flags);
    native_caches.push_back(cache);
    if (++native_creates == fail_native_at) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = Handle<VkPipeline>(next_native++);
    CHECK(native_pipelines.insert(*out).second);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyNativePipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks *a) {
    CHECK(d == device && !a && native_pipelines.erase(pipeline) == 1);
}
VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *out) {
    CHECK(p == physical);
    *out = {};
    out->limits.maxPushConstantsSize = 256;
    out->limits.maxComputeWorkGroupCount[0] = 65535;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci,
                                            const VkAllocationCallbacks *allocator, VkPipelineLayout *out) {
    {
        std::lock_guard<std::mutex> lock(retirement_lock);
        if (gate_retirement) {
            creation_entered = true;
            retirement_changed.notify_all();
        }
    }
    std::lock_guard<std::mutex> lock(native_layout_lock);
    CHECK(d == device && !ci->pNext);
    if (allocator) CHECK(alias_handle && static_cast<CallbackOwner *>(allocator->pUserData)->live);
    *out = VK_NULL_HANDLE;
    if (++create_attempts == fail_at) return VK_ERROR_OUT_OF_HOST_MEMORY;
    NativeLayout value;
    value.flags = ci->flags;
    if (ci->setLayoutCount) value.sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
    if (ci->pushConstantRangeCount)
        value.pushes.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
    for (auto range : value.pushes) CHECK(range.stageFlags == VK_SHADER_STAGE_COMPUTE_BIT);
    *out = alias_handle && (alias_private || allocator) ? alias_handle : Handle<VkPipelineLayout>(next_layout++);
    auto equal = [](const NativeLayout &a, const NativeLayout &b) {
        if (a.flags != b.flags || a.sets != b.sets || a.pushes.size() != b.pushes.size()) return false;
        for (size_t i = 0; i < a.pushes.size(); ++i)
            if (a.pushes[i].stageFlags != b.pushes[i].stageFlags || a.pushes[i].offset != b.pushes[i].offset ||
                a.pushes[i].size != b.pushes[i].size)
                return false;
        return true;
    };
    auto candidate = layouts.find(*out);
    if (candidate != layouts.end() && !equal(candidate->second, value)) *out = Handle<VkPipelineLayout>(next_layout++);
    auto old = layouts.find(*out);
    if (old != layouts.end()) {
        CHECK(equal(old->second, value));
        for (size_t i = 0; i < value.pushes.size() && i < old->second.pushes.size(); ++i)
            CHECK(!std::memcmp(&old->second.pushes[i], &value.pushes[i], sizeof(VkPushConstantRange)));
        ++old->second.references;
    } else {
        CHECK(layouts.emplace(*out, std::move(value)).second);
        destroyed.erase(*out);  // A fully retired handle may be reused.
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice d, VkPipelineLayout layout, const VkAllocationCallbacks *allocator) {
    {
        std::unique_lock<std::mutex> lock(retirement_lock);
        if (gate_retirement && allocator && layout == alias_handle) {
            retirement_entered = true;
            retirement_changed.notify_all();
            CHECK(retirement_changed.wait_for(lock, std::chrono::seconds(5), [] { return release_retirement; }));
        }
    }
    std::lock_guard<std::mutex> lock(native_layout_lock);
    CHECK(d == device);
    if (allocator) CHECK(static_cast<CallbackOwner *>(allocator->pUserData)->live);
    auto old = layouts.find(layout);
    CHECK(old != layouts.end() && old->second.references);
    if (old != layouts.end() && --old->second.references == 0) {
        layouts.erase(old);
        CHECK(destroyed.insert(layout).second);
    }
}
VKAPI_ATTR void VKAPI_CALL Bind2(VkCommandBuffer c, const VkBindDescriptorSetsInfo *info) {
    CHECK(c == command);
    ++forwarded;
    if (passthrough) {
        CHECK(info == passthrough);
        return;
    }
    CHECK(info->sType == VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO && !info->pNext);
    CHECK(info->layout && layouts.count(info->layout));
    CHECK(info->stageFlags == expected_stages);
    const auto &layout = layouts.at(info->layout);
    CHECK(layout.pushes.size() == 1 && layout.pushes[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT && layout.pushes[0].offset == 0 &&
          layout.pushes[0].size == expected_push_bytes);
    observed = *info;
    observed_sets.assign(info->pDescriptorSets, info->pDescriptorSets + info->descriptorSetCount);
    observed_offsets.assign(info->pDynamicOffsets, info->pDynamicOffsets + info->dynamicOffsetCount);
    if (descriptor_history_test) {
        DescriptorEvent event{"sets",
                              info->layout,
                              {VK_PIPELINE_BIND_POINT_COMPUTE, info->firstSet, info->descriptorSetCount, info->dynamicOffsetCount}};
        for (uint32_t n = 0; n < info->descriptorSetCount; ++n) event.values.push_back(VKRT_HKEY(info->pDescriptorSets[n]));
        for (uint32_t n = 0; n < info->dynamicOffsetCount; ++n) event.values.push_back(info->pDynamicOffsets[n]);
        descriptor_events.push_back(std::move(event));
    }
}
VKAPI_ATTR void VKAPI_CALL BindLegacy(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                                      uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count,
                                      const uint32_t *offsets) {
    CHECK(point == VK_PIPELINE_BIND_POINT_COMPUTE);
    VkBindDescriptorSetsInfo info{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};
    info.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    info.layout = layout;
    info.firstSet = first;
    info.descriptorSetCount = count;
    info.pDescriptorSets = sets;
    info.dynamicOffsetCount = dynamic_count;
    info.pDynamicOffsets = offsets;
    Bind2(c, &info);
}
void Reset() {
    vkrt_rtp_reset_command(&cb);
    CHECK(layouts.empty());
    cb.record_result = VK_SUCCESS;
}
std::vector<DescriptorEvent> InstallDescriptorHistory() {
    auto make = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    std::array<VkPipelineLayout, 6> identity{}, native{};
    for (uint32_t n = 0; n < identity.size(); ++n) {
        const auto set = Handle<VkDescriptorSetLayout>(4000 + n);
        VkSampler samplers[] = {Handle<VkSampler>(4100), Handle<VkSampler>(4101)};
        VkDescriptorSetLayoutBinding binding{0,
                                             n == 0             ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                             : n == 3 || n == 5 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             2,
                                             VkShaderStageFlags(n < 4 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT),
                                             n == 3 || n == 5 ? samplers : nullptr};
        VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        definition.flags = n == 1   ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR
                           : n >= 2 ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
                                    : 0;
        if (n == 3 || n == 5) definition.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT;
        definition.bindingCount = 1;
        definition.pBindings = &binding;
        CHECK(vkrt_rtp_track_descriptor_layout(&dev, set, &definition) == VK_SUCCESS);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &set;
        ci.pushConstantRangeCount = n < 4 ? 1 : 0;
        ci.pPushConstantRanges = &push;
        VkPipelineLayout layout{};
        CHECK(make(device, &ci, nullptr, &layout) == VK_SUCCESS);
        native[n] = vkrt_rtp_native_layout(&dev, layout);
        CHECK(vkrt_rtp_replay_layout_capture(&cb, layout, &identity[n]) == VK_SUCCESS && identity[n]);
        // Replay must retain the immutable original definition after both
        // public objects and their caller-owned creation inputs disappear.
        destroy(device, layout, nullptr);
        vkrt_rtp_forget_descriptor_layout(&dev, set);
        std::memset(&binding, 0xa5, sizeof(binding));
        std::memset(samplers, 0xa5, sizeof(samplers));
    }
    auto bind = (PFN_vkCmdBindDescriptorSets)vkrt_rtp_proc("vkCmdBindDescriptorSets");
    auto buffers = (PFN_vkCmdBindDescriptorBuffersEXT)vkrt_rtp_proc("vkCmdBindDescriptorBuffersEXT");
    auto bind2 = (PFN_vkCmdBindDescriptorSets2KHR)vkrt_rtp_proc("vkCmdBindDescriptorSets2KHR");
    auto push = (PFN_vkCmdPushDescriptorSetKHR)vkrt_rtp_proc("vkCmdPushDescriptorSetKHR");
    auto push2 = (PFN_vkCmdPushDescriptorSet2KHR)vkrt_rtp_proc("vkCmdPushDescriptorSet2KHR");
    auto offsets = (PFN_vkCmdSetDescriptorBufferOffsetsEXT)vkrt_rtp_proc("vkCmdSetDescriptorBufferOffsetsEXT");
    auto offsets2 = (PFN_vkCmdSetDescriptorBufferOffsets2EXT)vkrt_rtp_proc("vkCmdSetDescriptorBufferOffsets2EXT");
    auto samplers = (PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT)vkrt_rtp_proc("vkCmdBindDescriptorBufferEmbeddedSamplersEXT");
    auto samplers2 =
        (PFN_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT)vkrt_rtp_proc("vkCmdBindDescriptorBufferEmbeddedSamplers2EXT");
    CHECK(bind && buffers && bind2 && push && push2 && offsets && offsets2 && samplers && samplers2);
    expected_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptor_history_test = true;
    descriptor_events.clear();
    VkDescriptorBufferBindingInfoEXT infos[8]{};
    DescriptorEvent expected_buffers{"buffers", VK_NULL_HANDLE, {8}};
    for (uint32_t i = 0; i < 8; ++i) {
        infos[i] = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr, UINT64_C(0x800000000) + i * UINT64_C(0x10000),
                    VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT};
        expected_buffers.values.insert(expected_buffers.values.end(), {UINT64_C(0x800000000) + i * UINT64_C(0x10000),
                                                                       VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT});
    }
    buffers(command, 8, infos);
    std::memset(infos, 0xa5, sizeof(infos));
    for (uint32_t round = 0; round < 2; ++round) {
        VkDescriptorSet sets[] = {Handle<VkDescriptorSet>(501 + round)};
        uint32_t dynamic[] = {256 + 512 * round, 512 + 512 * round};
        VkBindDescriptorSetsInfo binding{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binding.layout = identity[0];
        binding.descriptorSetCount = 1;
        binding.pDescriptorSets = sets;
        binding.dynamicOffsetCount = 2;
        binding.pDynamicOffsets = dynamic;
        if (round)
            bind2(command, &binding);
        else
            bind(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity[0], 0, 1, sets, 2, dynamic);
        std::memset(sets, 0xa5, sizeof(sets));
        std::memset(dynamic, 0xa5, sizeof(dynamic));
        std::memset(&binding, 0xa5, sizeof(binding));
        VkDescriptorBufferInfo buffers[] = {{Handle<VkBuffer>(601 + 2 * round), 16 + 64 * round, 32 + 64 * round},
                                            {Handle<VkBuffer>(602 + 2 * round), 48 + 64 * round, 64 + 64 * round}};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.descriptorCount = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = buffers;
        VkPushDescriptorSetInfo pushing{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO};
        pushing.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushing.layout = identity[1];
        pushing.descriptorWriteCount = 1;
        pushing.pDescriptorWrites = &write;
        if (round)
            push2(command, &pushing);
        else
            push(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity[1], 0, 1, &write);
        std::memset(buffers, 0xa5, sizeof(buffers));
        std::memset(&write, 0xa5, sizeof(write));
        std::memset(&pushing, 0xa5, sizeof(pushing));
        uint32_t indices[] = {3 + 4 * round};
        VkDeviceSize values[] = {0x100000010ull * (round + 1)};
        VkSetDescriptorBufferOffsetsInfoEXT setting{VK_STRUCTURE_TYPE_SET_DESCRIPTOR_BUFFER_OFFSETS_INFO_EXT};
        setting.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        setting.layout = identity[2];
        setting.setCount = 1;
        setting.pBufferIndices = indices;
        setting.pOffsets = values;
        if (round)
            offsets2(command, &setting);
        else
            offsets(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity[2], 0, 1, indices, values);
        std::memset(indices, 0xa5, sizeof(indices));
        std::memset(values, 0xa5, sizeof(values));
        std::memset(&setting, 0xa5, sizeof(setting));
        VkBindDescriptorBufferEmbeddedSamplersInfoEXT embedding{
            VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_BUFFER_EMBEDDED_SAMPLERS_INFO_EXT};
        embedding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        embedding.layout = identity[3];
        if (round)
            samplers2(command, &embedding);
        else
            samplers(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity[3], 0);
        std::memset(&embedding, 0xa5, sizeof(embedding));
    }
    uint32_t graphics_index = 5;
    VkDeviceSize graphics_offset = UINT64_C(0x300000030);
    offsets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, identity[4], 0, 1, &graphics_index, &graphics_offset);
    samplers(command, VK_PIPELINE_BIND_POINT_GRAPHICS, identity[5], 0);
    // /2 ALL affects both native bind points. Preserve its flags in the real
    // capture schema so graphics cannot silently retain the older values.
    graphics_index = 6;
    graphics_offset = UINT64_C(0x400000040);
    VkSetDescriptorBufferOffsetsInfoEXT all_offsets{VK_STRUCTURE_TYPE_SET_DESCRIPTOR_BUFFER_OFFSETS_INFO_EXT};
    all_offsets.stageFlags = VK_SHADER_STAGE_ALL;
    all_offsets.layout = identity[4];
    all_offsets.setCount = 1;
    all_offsets.pBufferIndices = &graphics_index;
    all_offsets.pOffsets = &graphics_offset;
    offsets2(command, &all_offsets);
    VkBindDescriptorBufferEmbeddedSamplersInfoEXT all_samplers{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_BUFFER_EMBEDDED_SAMPLERS_INFO_EXT};
    all_samplers.stageFlags = VK_SHADER_STAGE_ALL;
    all_samplers.layout = identity[5];
    samplers2(command, &all_samplers);
    std::memset(&all_offsets, 0xa5, sizeof(all_offsets));
    std::memset(&all_samplers, 0xa5, sizeof(all_samplers));
    graphics_index = UINT32_MAX;
    graphics_offset = UINT64_MAX;
    // Literal expectations are independent of the captured payload and native
    // observations. In particular, old classic bindings cannot move past the
    // newer push/offset commands when restoring application compute state.
    std::vector<DescriptorEvent> expected = {
        expected_buffers,
        {"sets", native[0], {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 2, 501, 256, 512}},
        {"push",
         native[1],
         {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, 601, 16, 32, 602, 48, 64}},
        {"offsets", native[2], {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 3, 0x100000010ull}},
        {"samplers", native[3], {VK_PIPELINE_BIND_POINT_COMPUTE, 0}},
        {"sets", native[0], {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 2, 502, 768, 1024}},
        {"push",
         native[1],
         {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, 603, 80, 96, 604, 112, 128}},
        {"offsets", native[2], {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 7, 0x200000020ull}},
        {"samplers", native[3], {VK_PIPELINE_BIND_POINT_COMPUTE, 0}},
        {"offsets", native[4], {VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 1, 5, UINT64_C(0x300000030)}},
        {"samplers", native[5], {VK_PIPELINE_BIND_POINT_GRAPHICS, 0}},
        {"offsets", native[4], {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 6, UINT64_C(0x400000040)}},
        {"offsets", native[4], {VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 1, 6, UINT64_C(0x400000040)}},
        {"samplers", native[5], {VK_PIPELINE_BIND_POINT_COMPUTE, 0}},
        {"samplers", native[5], {VK_PIPELINE_BIND_POINT_GRAPHICS, 0}},
    };
    CHECK(cb.record_result == VK_SUCCESS && descriptor_events == expected);
    NativeDrawDescriptorOracle();
    descriptor_events.clear();
    return expected;
}
void CheckDescriptorRestore(const std::vector<DescriptorEvent> &history, std::initializer_list<uint32_t> tiles) {
    std::vector<DescriptorEvent> expected;
    for (auto count : tiles) expected.push_back({"dispatch", VK_NULL_HANDLE, {count}});
    expected.insert(expected.end(), history.begin(), history.end());
    CHECK(descriptor_events == expected);
    NativeDrawDescriptorOracle();
    descriptor_events.clear();
}
void PipelineDerivatives() {
    auto make_layout = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy_layout = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    auto create = (PFN_vkCreateRayTracingPipelinesKHR)vkrt_rtp_proc("vkCreateRayTracingPipelinesKHR");
    auto destroy = (PFN_vkDestroyPipeline)vkrt_rtp_proc("vkDestroyPipeline");
    auto handles = (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkrt_rtp_proc("vkGetRayTracingShaderGroupHandlesKHR");
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main"
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::vector<uint32_t> words;
    CHECK(tools.Assemble(source, &words) && tools.Validate(words));
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_ci.codeSize = words.size() * sizeof(uint32_t);
    shader_ci.pCode = words.data();
    VkShaderModule shader{};
    bool handled = false;
    CHECK(vkrt_rtp_create_shader(&dev, &shader_ci, nullptr, &shader, &handled) == VK_SUCCESS && handled);
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    CHECK(make_layout(device, &layout_ci, nullptr, &layout) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.module = shader;
    stage.pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR ci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    ci.stageCount = 1;
    ci.pStages = &stage;
    ci.groupCount = 1;
    ci.pGroups = &group;
    ci.maxPipelineRayRecursionDepth = 1;
    ci.layout = layout;
    ci.basePipelineIndex = -1;
    VkRayTracingPipelineCreateInfoKHR batch[] = {ci, ci};
    batch[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    batch[1].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    batch[1].basePipelineIndex = 0;
    const auto saved_batch = Snapshot(batch);
    VkPipeline pipelines[3]{};
    native_flags.clear();
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 2, batch, nullptr, pipelines) == VK_SUCCESS);
    CHECK(!std::memcmp(batch, saved_batch.data(), sizeof(batch)));
    ci.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    ci.basePipelineHandle = pipelines[1];
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines[2]) == VK_SUCCESS);
    CHECK(native_flags == std::vector<VkPipelineCreateFlags>({VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT, 0, 0}));
    destroy(device, pipelines[0], nullptr);
    CHECK(native_pipelines.size() == 2);
    std::array<uint8_t, 32> first{}, second{};
    CHECK(handles(device, pipelines[1], 0, 1, first.size(), first.data()) == VK_SUCCESS);
    CHECK(handles(device, pipelines[2], 0, 1, second.size(), second.data()) == VK_SUCCESS && first != second);
    destroy(device, pipelines[1], nullptr);
    destroy(device, pipelines[2], nullptr);
    CHECK(native_pipelines.empty() && native_shaders.empty());
    // Base fields are ignored unless DERIVATIVE is set. A real ordinary
    // pipeline still cannot be used as a base without ALLOW_DERIVATIVES.
    ci.flags = 0;
    ci.basePipelineHandle = Handle<VkPipeline>(77);
    ci.basePipelineIndex = 123;
    VkPipeline ordinary{};
    const auto cache = Handle<VkPipelineCache>(50000);
    CHECK(create(device, VK_NULL_HANDLE, cache, 1, &ci, nullptr, &ordinary) == VK_SUCCESS && native_caches.back() == cache);
    ci.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    ci.basePipelineHandle = ordinary;
    ci.basePipelineIndex = -1;
    VkPipeline rejected_base{};
    unsigned calls = native_creates;
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &rejected_base) == VK_ERROR_FEATURE_NOT_PRESENT &&
          !rejected_base && native_creates == calls);
    destroy(device, ordinary, nullptr);
    // Libraries own typed definitions and do not allocate an executable. A
    // nested/repeated dependency remains linkable after its source is retired.
    VkRayTracingPipelineInterfaceCreateInfoKHR library_interface{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR};
    library_interface.maxPipelineRayPayloadSize = 16;
    library_interface.maxPipelineRayHitAttributeSize = 8;
    VkRayTracingPipelineCreateInfoKHR lib = ci;
    lib.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    lib.pLibraryInterface = &library_interface;
    VkPipeline library_source{}, wrapper{}, linked{};
    const unsigned before_libraries = native_creates;
    VkPipelineCreateFlags2CreateInfoKHR flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    flags2.flags = VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    lib.pNext = &flags2;
    // The legacy field is ignored in its entirety when flags2 is present.
    lib.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    const auto saved_library = Snapshot(lib);
    const auto saved_flags2 = Snapshot(flags2);
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, nullptr, &library_source) == VK_SUCCESS && library_source);
    CHECK(!std::memcmp(&lib, saved_library.data(), sizeof(lib)) && !std::memcmp(&flags2, saved_flags2.data(), sizeof(flags2)));
    CHECK(native_creates == before_libraries);
    VkPipeline unsupported_flags{};
    flags2.flags |= UINT64_C(1) << 63;
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, nullptr, &unsupported_flags) == VK_ERROR_FEATURE_NOT_PRESENT &&
          !unsupported_flags && native_creates == before_libraries);
    lib.pNext = nullptr;
    lib.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    VkPipeline repeated[] = {library_source, library_source};
    VkPipelineLibraryCreateInfoKHR links{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    links.libraryCount = 2;
    links.pLibraries = repeated;
    lib.stageCount = lib.groupCount = 0;
    lib.pStages = nullptr;
    lib.pGroups = nullptr;
    lib.pLibraryInfo = &links;
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &lib, nullptr, &wrapper) == VK_SUCCESS && wrapper);
    CHECK(native_creates == before_libraries);
    destroy(device, library_source, nullptr);
    lib.flags = 0;
    links.libraryCount = 1;
    links.pLibraries = &wrapper;
    CHECK(create(device, VK_NULL_HANDLE, cache, 1, &lib, nullptr, &linked) == VK_SUCCESS && linked);
    CHECK(native_creates == before_libraries + 1 && native_caches.back() == cache);
    destroy(device, wrapper, nullptr);
    std::array<uint8_t, 64> linked_handles{};
    CHECK(handles(device, linked, 0, 2, linked_handles.size(), linked_handles.data()) == VK_SUCCESS);
    CHECK(std::memcmp(linked_handles.data(), linked_handles.data() + 32, 32) != 0);
    destroy(device, linked, nullptr);
    CHECK(native_pipelines.empty() && native_shaders.empty());
    // Invalid provenance is rejected before native compilation. These are host
    // contract tests; native validation never receives malformed create infos.
    unsigned before = native_creates;
    for (int32_t index : {-2, 0, 1}) {
        ci.basePipelineHandle = VK_NULL_HANDLE;
        ci.basePipelineIndex = index;
        VkPipeline rejected = Handle<VkPipeline>(77);
        CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
              !rejected && native_creates == before);
    }
    ci.basePipelineIndex = -1;
    for (auto flag : {VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT,
                      VK_PIPELINE_CREATE_RAY_TRACING_SHADER_GROUP_HANDLE_CAPTURE_REPLAY_BIT_KHR}) {
        ci.flags = flag;
        VkPipeline rejected{};
        CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
              !rejected && native_creates == before);
    }
    // A failed earlier base must never become an invalid native base handle;
    // independent later elements still return their successful objects.
    VkRayTracingPipelineCreateInfoKHR partial[] = {batch[0], batch[1], batch[0]};
    fail_native_at = native_creates + 1;
    VkPipeline result[3]{};
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 3, partial, nullptr, result) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(!result[0] && !result[1] && result[2] && native_pipelines.size() == 1 && native_shaders.empty());
    fail_native_at = 0;
    destroy(device, result[2], nullptr);
    // Real production shader creation is budget-independent. Only replayed
    // trace occurrences acquire distinct mutable slices; original capture is
    // allocation-free. Force an allocation retry and a non-full final tile.
    VkDynamicState dynamic = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
    VkPipelineDynamicStateCreateInfo dynamic_info{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_info.dynamicStateCount = 1;
    dynamic_info.pDynamicStates = &dynamic;
    ci.flags = 0;
    ci.basePipelineHandle = VK_NULL_HANDLE;
    ci.basePipelineIndex = -1;
    ci.pDynamicState = &dynamic_info;
    VkPipeline execution{};
    CHECK(create(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr, &execution) == VK_SUCCESS);
    const auto compiled = native_creates;
    CHECK(vkrt_rtp_bind_pipeline(&cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, execution));
    auto stack = (PFN_vkCmdSetRayTracingPipelineStackSizeKHR)vkrt_rtp_proc("vkCmdSetRayTracingPipelineStackSizeKHR");
    auto trace = (PFN_vkCmdTraceRaysKHR)vkrt_rtp_proc("vkCmdTraceRaysKHR");
    stack(command, (2u << 20) + 1);
    VkStridedDeviceAddressRegionKHR raygen{0x10000, 32, 32}, empty{};
    const auto prior_contexts = context_attempts, prior_arenas = arena_attempts;
    trace(command, &raygen, &empty, &empty, &empty, 5, 1, 1);
    CHECK(cb.record_result == VK_SUCCESS && cb.has_rtp_execution && leases.empty() && context_attempts == prior_contexts &&
          arena_attempts == prior_arenas && dispatches.empty());
    cb.replaying = true;
    const auto prior_forwarded = forwarded;
    const auto descriptor_history = InstallDescriptorHistory();
    const uint64_t stride = (2u << 20) + 16;
    arena_ceiling = stride * 2;
    trace(command, &raygen, &empty, &empty, &empty, 5, 1, 1);
    CHECK(cb.record_result == VK_SUCCESS && native_creates == compiled && arena_attempts == prior_arenas + 2 &&
          context_attempts == prior_contexts + 1 && leases.size() == 2);
    CHECK(dispatches == std::vector<uint32_t>({2, 2, 1}) && trace_pushes.size() == 3 && trace_barriers.size() == 3);
    CheckDescriptorRestore(descriptor_history, {2, 2, 1});
    const uint64_t root = trace_pushes.empty() ? 0 : trace_pushes[0][0];
    for (size_t n = 0; n < trace_pushes.size(); ++n) CHECK(trace_pushes[n][0] == root && trace_pushes[n][1] == n * 2);
    for (const auto &[identity, lease] : leases) {
        (void)identity;
        if (lease.memory_class == VKRT_PRIVATE_HOST_COHERENT) {
            CHECK(lease.slice.size == VEL_RT_RUNTIME_CONTEXT_BYTES + 8 && lease.slice.address == root);
            const auto *mapped = static_cast<const uint8_t *>(lease.slice.mapping);
            auto value = [&](size_t offset) {
                uint64_t v;
                std::memcpy(&v, mapped + offset, 8);
                return v;
            };
            CHECK(value(VEL_RT_RUN_ARENA_BYTES) == stride && value(VEL_RT_RUN_POOL_BYTES) == stride * 2 &&
                  value(VEL_RT_RUN_INVOCATION_COUNT) == 2);
        } else {
            CHECK(!lease.slice.mapping && lease.slice.size == stride * 2);
            for (size_t n = 1; n < trace_barriers.size(); ++n)
                CHECK(trace_barriers[n].buffer == lease.slice.buffer && trace_barriers[n].offset == lease.slice.offset &&
                      trace_barriers[n].size == lease.slice.size);
        }
    }
    arena_ceiling = UINT64_MAX;
    stack(command, (64u << 20) + 1);
    trace(command, &raygen, &empty, &empty, &empty, 1, 1, 1);
    CHECK(cb.record_result == VK_SUCCESS && native_creates == compiled && leases.size() == 4 && trace_pushes.back()[0] != root);
    CheckDescriptorRestore(descriptor_history, {1});
    CHECK(forwarded == prior_forwarded + 6);
    bool complete_large_arena = false;
    for (const auto &[identity, lease] : leases) {
        (void)identity;
        complete_large_arena |= lease.memory_class == VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED && lease.slice.size == (64u << 20) + 16;
    }
    CHECK(complete_large_arena);
    const auto live = leases.size();
    arena_ceiling = 0;
    trace(command, &raygen, &empty, &empty, &empty, 1, 1, 1);
    CHECK(cb.record_result == VK_ERROR_OUT_OF_DEVICE_MEMORY && leases.size() == live && native_creates == compiled);
    CHECK(descriptor_events.empty());
    cb.record_result = VK_SUCCESS;
    arena_ceiling = UINT64_MAX;
    vkrt_rtp_reset_command(&cb);
    cb.replaying = cb.has_rtp_execution = false;
    descriptor_history_test = false;
    forwarded = prior_forwarded;
    CHECK(leases.empty());
    dispatches.clear();
    trace_pushes.clear();
    trace_barriers.clear();
    destroy(device, execution, nullptr);
    CHECK(vkrt_rtp_destroy_shader(&dev, shader, nullptr));
    destroy_layout(device, layout, nullptr);
    CHECK(layouts.empty() && native_pipelines.empty() && native_shaders.empty());
}
void DescriptorDefinitions() {
    auto make = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    const auto a = Handle<VkDescriptorSetLayout>(2000), b = Handle<VkDescriptorSetLayout>(2001);
    VkDescriptorSetLayoutBinding binding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
    VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    definition.bindingCount = 1;
    definition.pBindings = &binding;
    auto track = [&](VkDescriptorSetLayout handle) {
        // A changed definition here models a fresh native object reusing a
        // retired handle, rather than another simultaneously live reference.
        vkrt_rtp_forget_descriptor_layout(&dev, handle);
        CHECK(vkrt_rtp_track_descriptor_layout(&dev, handle, &definition) == VK_SUCCESS);
    };
    auto layout = [&](VkDescriptorSetLayout set, VkPipelineLayoutCreateFlags flags = 0) {
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.flags = flags;
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &set;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push;
        VkPipelineLayout result{};
        CHECK(make(device, &ci, nullptr, &result) == VK_SUCCESS);
        return result;
    };
    std::vector<VkPipelineLayout> owned;
    auto keep = [&](VkDescriptorSetLayout set, VkPipelineLayoutCreateFlags flags = 0) {
        owned.push_back(layout(set, flags));
        return owned.back();
    };
    track(a);
    track(b);
    auto first = keep(a), second = keep(b);
    CHECK(vkrt_rtp_layout_compatible(&dev, first, second, 0));
    // An implementation may return the same non-dispatchable handle for two
    // live creates. The first destruction must preserve the remaining alias,
    // including the immutable identity held by already-created pipelines.
    CHECK(vkrt_rtp_track_descriptor_layout(&dev, a, &definition) == VK_SUCCESS);
    auto alias = keep(a);
    vkrt_rtp_forget_descriptor_layout(&dev, a);
    auto surviving = keep(a);
    CHECK(vkrt_rtp_layout_compatible(&dev, first, alias, 0));
    CHECK(vkrt_rtp_layout_compatible(&dev, first, surviving, 0));
    // Distinct public definitions collapsing to one native handle stay
    // successful, but cannot retroactively rewrite retained pipeline snapshots.
    binding.descriptorCount = 5;
    CHECK(vkrt_rtp_track_descriptor_layout(&dev, a, &definition) == VK_SUCCESS);
    auto ambiguous = keep(a);
    CHECK(!vkrt_rtp_layout_compatible(&dev, first, ambiguous, 0));
    CHECK(vkrt_rtp_layout_compatible(&dev, first, second, 0));
    vkrt_rtp_forget_descriptor_layout(&dev, a);
    auto ambiguous_surviving = keep(a);
    CHECK(vkrt_rtp_layout_compatible(&dev, ambiguous, ambiguous_surviving, 0));
    // Creation inputs and DSL handles can both disappear or be reused while
    // pipeline layouts retain their immutable definitions.
    binding.descriptorCount = 8;
    vkrt_rtp_forget_descriptor_layout(&dev, a);
    track(a);
    auto changed = keep(a);
    vkrt_rtp_forget_descriptor_layout(&dev, b);
    CHECK(vkrt_rtp_layout_compatible(&dev, first, second, 0));
    CHECK(!vkrt_rtp_layout_compatible(&dev, first, changed, 0));
    binding.descriptorCount = 4;
    track(a);
    auto independent = keep(a, VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT);
    CHECK(!vkrt_rtp_layout_compatible(&dev, first, independent, 0));
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    binding_flags.bindingCount = 1;
    binding_flags.pBindingFlags = &flags;
    definition.pNext = &binding_flags;
    track(a);
    auto flagged = keep(a);
    flags = 0;
    track(b);
    auto unflagged = keep(b);
    CHECK(!vkrt_rtp_layout_compatible(&dev, flagged, unflagged, 0));
    CHECK(vkrt_rtp_layout_compatible(&dev, first, unflagged, 0));
    definition.pNext = nullptr;
    VkSampler sampler = Handle<VkSampler>(3000);
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binding.descriptorCount = 1;
    binding.pImmutableSamplers = &sampler;
    track(a);
    track(b);
    auto sampler_a = keep(a), sampler_b = keep(b);
    CHECK(vkrt_rtp_layout_compatible(&dev, sampler_a, sampler_b, 0));
    sampler = Handle<VkSampler>(3001);
    track(b);
    auto other_sampler = keep(b);
    CHECK(!vkrt_rtp_layout_compatible(&dev, sampler_a, other_sampler, 0));
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    VkMutableDescriptorTypeListEXT list{2, types};
    VkMutableDescriptorTypeCreateInfoEXT mutable_info{VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
    mutable_info.mutableDescriptorTypeListCount = 1;
    mutable_info.pMutableDescriptorTypeLists = &list;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
    binding.pImmutableSamplers = nullptr;
    definition.pNext = &mutable_info;
    track(a);
    track(b);
    auto mutable_a = keep(a), mutable_b = keep(b);
    CHECK(vkrt_rtp_layout_compatible(&dev, mutable_a, mutable_b, 0));
    types[1] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    track(b);
    auto other_mutable = keep(b);
    CHECK(!vkrt_rtp_layout_compatible(&dev, mutable_a, other_mutable, 0));
    for (auto handle : owned) destroy(device, handle, nullptr);
    vkrt_rtp_forget_descriptor_layout(&dev, a);
    vkrt_rtp_forget_descriptor_layout(&dev, b);
    CHECK(layouts.empty());
}
void ReplayLayoutLifetimes() {
    auto make = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    const auto set = Handle<VkDescriptorSetLayout>(99000);
    VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    CHECK(vkrt_rtp_track_descriptor_layout(&dev, set, &definition) == VK_SUCCESS);
    VkPushConstantRange pushes[] = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}, {VK_SHADER_STAGE_RAYGEN_BIT_KHR, 16, 4}};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = 2;
    ci.pPushConstantRanges = pushes;
    // Native stripping makes both original RT definitions identical to the
    // ICD. Native aliases are legal only for equal actual native definitions.
    alias_handle = Handle<VkPipelineLayout>(99001);
    alias_private = true;
    VkPipelineLayout public_layout{}, first{}, second{}, repeated{};
    CHECK(make(device, &ci, nullptr, &public_layout) == VK_SUCCESS);
    CHECK(vkrt_rtp_replay_layout_capture(&cb, public_layout, &first) == VK_SUCCESS && first);
    CHECK(vkrt_rtp_replay_layout_capture(&cb, public_layout, &repeated) == VK_SUCCESS && repeated == first);
    pushes[1].offset = 32;
    VkPipelineLayout distinct = Handle<VkPipelineLayout>(9);
    const auto distinct_result = make(device, &ci, nullptr, &distinct);
    CHECK(distinct_result == VK_SUCCESS && distinct && distinct != public_layout);
    if (distinct_result != VK_SUCCESS) {
        destroy(device, public_layout, nullptr);
        vkrt_rtp_reset_command(&cb);
        vkrt_rtp_forget_descriptor_layout(&dev, set);
        alias_handle = VK_NULL_HANDLE;
        alias_private = false;
        return;
    }
    CHECK(!vkrt_rtp_layout_compatible(&dev, public_layout, distinct, 0));
    CHECK(vkrt_rtp_replay_layout_capture(&cb, distinct, &second) == VK_SUCCESS && second && second != first);
    destroy(device, public_layout, nullptr);
    destroy(device, distinct, nullptr);
    vkrt_rtp_forget_descriptor_layout(&dev, set);
    CHECK(!layouts.empty() && layouts.count(alias_handle));
    const auto retained_references = layouts.at(alias_handle).references;
    vkrt_cmd_buffer_data execution{};
    execution.device_data = &dev;
    execution.cmd = command;
    execution.replaying = true;
    CHECK(vkrt_rtp_replay_layouts_inherit(&execution, &cb) == VK_SUCCESS);
    vkrt_rtp_reset_command(&cb);
    CHECK(layouts.at(alias_handle).references == retained_references);
    VkPipelineLayout native{};
    CHECK(vkrt_rtp_replay_layout_native(&execution, first, &native) == VK_SUCCESS && native == alias_handle);
    CHECK(vkrt_rtp_replay_layout_native(&execution, second, &native) == VK_SUCCESS && native == alias_handle);
    vkrt_map_object(VKRT_HKEY(command), &execution);
    auto bind = (PFN_vkCmdBindDescriptorSets)vkrt_rtp_proc("vkCmdBindDescriptorSets");
    auto bind2 = (PFN_vkCmdBindDescriptorSets2)vkrt_rtp_proc("vkCmdBindDescriptorSets2");
    const VkDescriptorSet descriptor = Handle<VkDescriptorSet>(99002);
    const uint32_t dynamic_offset = 0;
    VkBindDescriptorSetsInfo binding{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binding.layout = first;
    binding.descriptorSetCount = 1;
    binding.pDescriptorSets = &descriptor;
    binding.dynamicOffsetCount = 1;
    binding.pDynamicOffsets = &dynamic_offset;
    expected_push_bytes = 16;
    expected_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    unsigned before = forwarded;
    bind2(command, &binding);
    bind(command, VK_PIPELINE_BIND_POINT_COMPUTE, second, 0, 1, &descriptor, 1, &dynamic_offset);
    CHECK(execution.record_result == VK_SUCCESS && forwarded == before + 2 && observed.layout == alias_handle);
    bind(command, VK_PIPELINE_BIND_POINT_COMPUTE, Handle<VkPipelineLayout>(9), 0, 1, &descriptor, 1, &dynamic_offset);
    CHECK(execution.record_result == VK_ERROR_FEATURE_NOT_PRESENT && forwarded == before + 2);
    execution.record_result = VK_SUCCESS;
    expected_push_bytes = 256;
    vkrt_map_object(VKRT_HKEY(command), &cb);
    const uint32_t value = 0x12345678;
    CHECK(!vkrt_rtp_push(&execution, first, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 16, sizeof(value), &value));
    CHECK(execution.record_result == VK_SUCCESS);
    CHECK(!vkrt_rtp_push(&execution, second, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 32, sizeof(value), &value));
    CHECK(execution.record_result == VK_SUCCESS);
    // This range fits the second definition only. A table keyed by the aliased
    // real native handle would incorrectly accept it for the first identity.
    CHECK(!vkrt_rtp_push(&execution, first, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 32, sizeof(value), &value));
    CHECK(execution.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_rtp_reset_command(&execution);
    CHECK(layouts.empty());
    CHECK(vkrt_rtp_replay_layout_native(&execution, first, &native) == VK_ERROR_FEATURE_NOT_PRESENT && !native);
    CHECK(!execution.rtp);
    CHECK(vkrt_rtp_replay_layout_capture(&cb, public_layout, &repeated) == VK_ERROR_FEATURE_NOT_PRESENT && !repeated);
    CHECK(!cb.rtp);
    alias_handle = VK_NULL_HANDLE;
    alias_private = false;
}
unsigned NativeReferences() {
    std::lock_guard<std::mutex> lock(native_layout_lock);
    unsigned result = 0;
    for (auto &entry : layouts) result += entry.second.references;
    return result;
}
void DescriptorHistoryAllocationFailures(CallbackOwner &allocator) {
    auto make = (PFN_vkCreatePipelineLayout)vkrt_rtp_proc("vkCreatePipelineLayout");
    auto destroy = (PFN_vkDestroyPipelineLayout)vkrt_rtp_proc("vkDestroyPipelineLayout");
    auto push = (PFN_vkCmdPushDescriptorSetKHR)vkrt_rtp_proc("vkCmdPushDescriptorSetKHR");
    const auto set = Handle<VkDescriptorSetLayout>(90001);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    definition.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    definition.bindingCount = 1;
    definition.pBindings = &binding;
    CHECK(vkrt_rtp_track_descriptor_layout(&dev, set, &definition) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set;
    const VkDescriptorBufferInfo buffers[] = {{Handle<VkBuffer>(9100), 16, 32}, {Handle<VkBuffer>(9101), 48, 64}};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 2;
    write.pBufferInfo = buffers;
    const auto saved = Snapshot(write);
    bool succeeded = false;
    unsigned failures_seen = 0;
    for (unsigned allowance = 0; allowance < 32 && !succeeded; ++allowance) {
        const unsigned prior_allocations = allocator.allocations, prior_frees = allocator.frees;
        VkPipelineLayout layout{}, identity{};
        CHECK(make(device, &ci, nullptr, &layout) == VK_SUCCESS);
        const auto native = vkrt_rtp_native_layout(&dev, layout);
        CHECK(vkrt_rtp_replay_layout_capture(&cb, layout, &identity) == VK_SUCCESS);
        destroy(device, layout, nullptr);
        descriptor_history_test = true;
        descriptor_events.clear();
        cb.replaying = true;
        allocator.budget = allowance;
        push(command, VK_PIPELINE_BIND_POINT_COMPUTE, identity, 0, 1, &write);
        allocator.budget = -1;
        succeeded = cb.record_result == VK_SUCCESS;
        if (succeeded) {
            const std::vector<DescriptorEvent> expected = {
                {"push",
                 native,
                 {VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, 9100, 16, 32, 9101, 48, 64}}};
            CHECK(descriptor_events == expected);
        } else {
            CHECK(cb.record_result == VK_ERROR_OUT_OF_HOST_MEMORY && descriptor_events.empty());
            ++failures_seen;
        }
        // A failed nested-copy transaction owns neither borrowed input nor a
        // native command. Reset must free the journal, all successful partial
        // allocations, and the last retained layout using its DEVICE owner.
        vkrt_rtp_reset_command(&cb);
        cb.replaying = false;
        cb.record_result = VK_SUCCESS;
        descriptor_history_test = false;
        descriptor_events.clear();
        CHECK(!NativeReferences() && allocator.allocations - prior_allocations == allocator.frees - prior_frees);
        CHECK(!std::memcmp(&write, saved.data(), sizeof(write)));
    }
    CHECK(succeeded && failures_seen >= 4);
    vkrt_rtp_forget_descriptor_layout(&dev, set);
    std::printf("Compute descriptor history: owned nested payload, ordered repeated restore, allocation failures %u\n",
                failures_seen);
}
#include "rtp_layout_ownership_tests.h"
void PipelineLayoutAliases() { LayoutOwnership::Run(); }
#include "rtp_mixed_entry_tests.h"
#include "rtp_shader_ownership_tests.h"
}  // namespace
// One-shot, thread-local fault in the real C++ ownership-map allocation. Normal
// allocations retain the same malloc/free contract throughout this CPU fixture.
void *operator new(std::size_t size) {
    if (ShaderOwnership::fail_next_new) {
        ShaderOwnership::fail_next_new = false;
        ++ShaderOwnership::bookkeeping_faults;
        throw std::bad_alloc();
    }
    if (void *memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
extern "C" VkResult vkrt_rewrite_shader_module(vkrt_device_data *, const VkShaderModuleCreateInfo *source,
                                               VkShaderModuleCreateInfo *rewritten, bool *query, bool *pipeline) {
    if (ShaderOwnership::active) return ShaderOwnership::Rewrite(source, rewritten, query, pipeline);
    // Adapter spy only: real Ray Query lowering has public GPU/CTS coverage.
    // Every selected mixed-entry module must pass through this normal hook.
    CheckSelectedModule(MixedWords(source->pCode, source->pCode + source->codeSize / 4));
    ++selected_rewrites;
    *rewritten = *source;
    *query = *pipeline = false;
    return VK_SUCCESS;
}
extern "C" VkDeviceAddress vkrt_as_registry_address(vkrt_device_data *data) {
    CHECK(data == &dev);
    return 0;
}
extern "C" VkResult vkrt_private_pool_allocate_ex(vkrt_device_data *data, VkDeviceSize size, vkrt_private_memory_class memory_class,
                                                  VkDeviceSize physical_alignment, VkDeviceSize native_offset_alignment,
                                                  vkrt_private_slice *out) {
    CHECK(data == &dev && native_offset_alignment == 1);
    *out = {};
    if (memory_class == VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED) {
        CHECK(physical_alignment == 16);
        ++arena_attempts;
        if (size > arena_ceiling) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    } else {
        CHECK(memory_class == VKRT_PRIVATE_HOST_COHERENT && physical_alignment == 8);
        ++context_attempts;
    }
    auto identity = reinterpret_cast<vkrt_private_allocation *>(next_native++);
    auto &lease = leases[identity];
    lease.memory_class = memory_class;
    if (memory_class == VKRT_PRIVATE_HOST_COHERENT) lease.host.resize(size_t(size) + 1);
    lease.slice.buffer = Handle<VkBuffer>(next_native++);
    lease.slice.offset = 13;  // Native offset alignment and physical BDA are independent.
    lease.slice.size = size;
    lease.slice.address = uint64_t(next_native++) << 32;
    lease.slice.mapping = lease.host.empty() ? nullptr : lease.host.data() + 1;
    lease.slice.allocation = identity;
    *out = lease.slice;
    return VK_SUCCESS;
}
extern "C" void vkrt_private_pool_release(vkrt_device_data *data, vkrt_private_slice *slice) {
    CHECK(data == &dev);
    if (slice->allocation) CHECK(leases.erase(slice->allocation) == 1);
    *slice = {};
}
extern "C" bool vkrt_deferred_valid(vkrt_device_data *data, VkDeferredOperationKHR value) {
    CHECK(data == &dev);
    return !value;
}
extern "C" vkrt_replay_context *vkrt_replay_lookup_context(VkCommandBuffer command_buffer) {
    auto *state = VKRT_FIND(vkrt_cmd_buffer_data, command_buffer);
    if (!state) return nullptr;
    state->replay.user = state;
    state->replay.resolve = [](void *, const char *name) {
        auto call = vkrt_rtp_proc(name);
        return call ? call : NativeProc(device, name);
    };
    state->replay.normalize_pipeline_layout = [](void *user, VkPipelineLayout source, VkPipelineLayout *stable) {
        if (!source) {
            *stable = VK_NULL_HANDLE;
            return VK_SUCCESS;
        }
        return vkrt_rtp_replay_layout_capture(static_cast<vkrt_cmd_buffer_data *>(user), source, stable);
    };
    state->replay.emit_pipeline_layout = [](void *user, const char *name, VkPipelineLayout identity, VkPipelineLayout *forward) {
        *forward = identity;
        if (!identity) return VK_SUCCESS;
        // Mirror the command adapter's distinction: translating RTP hooks must
        // receive the retained semantic identity; other consumers get an ICD
        // handle. The real replay library invokes this callback, not a mock.
        for (const char *translating :
             {"vkCmdBindDescriptorSets", "vkCmdBindDescriptorSets2", "vkCmdBindDescriptorSets2KHR", "vkCmdPushConstants",
              "vkCmdPushConstants2", "vkCmdPushConstants2KHR", "vkCmdPushDescriptorSet", "vkCmdPushDescriptorSetKHR",
              "vkCmdPushDescriptorSet2", "vkCmdPushDescriptorSet2KHR", "vkCmdSetDescriptorBufferOffsetsEXT",
              "vkCmdSetDescriptorBufferOffsets2EXT", "vkCmdBindDescriptorBufferEmbeddedSamplersEXT",
              "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT"})
            if (!std::strcmp(name, translating)) return VK_SUCCESS;
        return vkrt_rtp_replay_layout_native(static_cast<vkrt_cmd_buffer_data *>(user), identity, forward);
    };
    return &state->replay;
}
int main() {
    instance.vtable.GetPhysicalDeviceProperties = Properties;
    dev.device = device;
    dev.physical_device = physical;
    dev.instance_data = &instance;
    dev.vtable.CreatePipelineLayout = CreateLayout;
    dev.vtable.DestroyPipelineLayout = DestroyLayout;
    dev.vtable.CmdBindDescriptorSets2 = Bind2;
    dev.vtable.CmdBindDescriptorSets = BindLegacy;
    dev.vtable.CreateShaderModule = CreateNativeShader;
    dev.vtable.DestroyShaderModule = DestroyNativeShader;
    dev.vtable.CreateComputePipelinesInternal = CreateNativePipeline;
    dev.vtable.DestroyPipeline = DestroyNativePipeline;
    dev.vtable.CmdBindPipeline = BindNativeCompute;
    dev.vtable.CmdPushConstants = TracePush;
    dev.vtable.CmdDispatch = TraceDispatch;
    dev.vtable.CmdPipelineBarrier = TraceBarrier;
    dev.vtable.GetDeviceProcAddr = NativeProc;
    cb.cmd = command;
    cb.device_data = &dev;
    vkrt_map_object(VKRT_HKEY(device), &dev);
    vkrt_map_object(VKRT_HKEY(command), &cb);
    CHECK(vkrt_rtp_init(&dev, nullptr) == VK_SUCCESS);
    DescriptorDefinitions();
    MixedEntryPipelines();
    ShaderOwnership::Run();
    PipelineDerivatives();
    const std::string bank_source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability PushConstantBanksNV
OpExtension "SPV_KHR_ray_tracing"
OpExtension "SPV_NV_push_constant_bank"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %pc
OpMemberDecorate %Block 0 Offset 0
OpDecorate %Block Block
OpDecorate %pc BankNV 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%Block = OpTypeStruct %uint
%ptr = OpTypePointer PushConstant %Block
%pc = OpVariable %ptr PushConstant
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    for (bool raygen : {true, false}) {
        auto source = bank_source;
        if (!raygen) {
            for (const std::string &line :
                 {std::string("OpCapability RayTracingKHR\n"), std::string("OpExtension \"SPV_KHR_ray_tracing\"\n")})
                source.erase(source.find(line), line.size());
            const std::string entry = "OpEntryPoint RayGenerationKHR %main \"main\" %pc";
            source.replace(source.find(entry), entry.size(),
                           "OpEntryPoint GLCompute %main \"main\" %pc\nOpExecutionMode %main LocalSize 1 1 1");
        }
        std::vector<uint32_t> words;
        CHECK(tools.Assemble(source, &words) && tools.Validate(words));
        const auto saved = words;
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = words.size() * sizeof(uint32_t);
        shader_info.pCode = words.data();
        VkShaderModule shader = Handle<VkShaderModule>(99);
        bool handled = false;
        VkResult result = vkrt_rtp_create_shader(&dev, &shader_info, nullptr, &shader, &handled);
        CHECK(result == (raygen ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS));
        CHECK(handled == raygen && shader == (raygen ? VK_NULL_HANDLE : Handle<VkShaderModule>(99)) && words == saved);
    }
    auto core = (PFN_vkCmdBindDescriptorSets2)vkrt_rtp_proc("vkCmdBindDescriptorSets2");
    auto khr = (PFN_vkCmdBindDescriptorSets2)vkrt_rtp_proc("vkCmdBindDescriptorSets2KHR");
    CHECK(core && khr);
    const VkDescriptorSetLayout set_layouts[] = {Handle<VkDescriptorSetLayout>(40), Handle<VkDescriptorSetLayout>(41)};
    VkDescriptorSetLayoutCreateInfo empty_definition{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    for (auto handle : set_layouts) CHECK(vkrt_rtp_track_descriptor_layout(&dev, handle, &empty_definition) == VK_SUCCESS);
    const VkPushConstantRange ranges[] = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256}, {VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4}};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = set_layouts;
    ci.pushConstantRangeCount = 2;
    ci.pPushConstantRanges = ranges;
    const auto saved_ci = Snapshot(ci);
    const VkDescriptorSet sets[] = {Handle<VkDescriptorSet>(50), Handle<VkDescriptorSet>(51)};
    const uint32_t offsets[] = {256, 512};
    VkBindDescriptorSetsInfo info{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};
    info.pNext = &ci;
    info.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    info.firstSet = 0;
    info.descriptorSetCount = 2;
    info.pDescriptorSets = sets;
    info.dynamicOffsetCount = 2;
    info.pDynamicOffsets = offsets;
    const auto saved_info = Snapshot(info);
    expected_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    core(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == 1);
    CHECK(!std::memcmp(&info, saved_info.data(), sizeof(info)) && !std::memcmp(&ci, saved_ci.data(), sizeof(ci)));
    CHECK(observed_sets == std::vector<VkDescriptorSet>(sets, sets + 2));
    CHECK(observed_offsets == std::vector<uint32_t>(offsets, offsets + 2));
    // The temporary public handle has been destroyed. The native layout used
    // for the captured binding remains live until command-buffer reset.
    CHECK(!destroyed.empty() && layouts.count(observed.layout));
    Reset();
    CHECK(destroyed.size() == next_layout - 100);

    unsigned before = forwarded;
    info.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    khr(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before && !layouts.empty());
    Reset();

    info.stageFlags = VK_SHADER_STAGE_ALL;
    expected_stages = VK_SHADER_STAGE_ALL;
    core(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before + 1);
    Reset();

    for (unsigned allocation = 1; allocation <= 3; ++allocation) {
        unsigned calls = forwarded;
        fail_at = create_attempts + allocation;
        info.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        core(command, &info);
        CHECK(cb.record_result == VK_ERROR_OUT_OF_HOST_MEMORY && forwarded == calls && layouts.empty());
        Reset();
    }
    fail_at = 0;

    struct Unknown {
        VkStructureType sType;
        const void *pNext;
        uint64_t sentinel;
    } unknown{(VkStructureType)0x7ffffffe, &ci, UINT64_C(0xabcdef1234567890)};
    const auto saved_unknown = Snapshot(unknown);
    info.pNext = &unknown;
    before = forwarded;
    khr(command, &info);
    CHECK(cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT && forwarded == before && layouts.empty());
    CHECK(!std::memcmp(&unknown, saved_unknown.data(), sizeof(unknown)));
    Reset();

    ReplayLayoutLifetimes();
    vkrt_rtp_finish(&dev);
    PipelineLayoutAliases();
    passthrough = &info;
    before = forwarded;
    core(command, &info);
    CHECK(cb.record_result == VK_SUCCESS && forwarded == before + 1 && !cb.rtp && layouts.empty());
    CHECK(!std::memcmp(&unknown, saved_unknown.data(), sizeof(unknown)));
    vkrt_unmap_object(VKRT_HKEY(command));
    vkrt_unmap_object(VKRT_HKEY(device));
    std::printf("RTP bindings/pipeline derivation host contracts %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
