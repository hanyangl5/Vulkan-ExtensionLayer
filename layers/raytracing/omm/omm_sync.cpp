// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_sync.h"
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vel::omm {
static_assert(std::is_nothrow_move_assignable_v<TranslatedDependency>);
static_assert(std::is_nothrow_move_assignable_v<TranslatedLegacyBarriers>);
namespace {
constexpr VkAccessFlags2 micromap_access = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
constexpr VkAccessFlags2 as_access =
    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
constexpr VkPipelineStageFlags2 as_stages =
    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
constexpr VkAccessFlags2 metadata_access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
// Queue ownership transfers may supply ignored access masks on non-shader
// sides. A global barrier has no ownership-transfer ignore rule. Keep those
// execution-only sides empty rather than creating an invalid memory scope.
VkAccessFlags2 sidecar_access(VkPipelineStageFlags2 stages, VkAccessFlags2 accesses) noexcept {
    constexpr VkPipelineStageFlags2 non_shader =
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_HOST_BIT |
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
        VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT |
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    if ((stages & ~non_shader) == 0) return 0;
    return map_access2(accesses) & metadata_access;
}
// A micromap read can originally target AS_BUILD. The translated ShaderRead
// needs compute in its stage scope as well; keep native AS/unrelated stages.
VkPipelineStageFlags2 barrier_stage(VkPipelineStageFlags2 stages, VkAccessFlags2 accesses) noexcept {
    auto result = map_stage2(stages);
    if ((accesses & micromap_access) && !(result & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
        result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    return result;
}
template <typename Barrier>
void translate(Barrier& barrier) noexcept {
    barrier.srcStageMask = barrier_stage(barrier.srcStageMask, barrier.srcAccessMask);
    barrier.dstStageMask = barrier_stage(barrier.dstStageMask, barrier.dstAccessMask);
    barrier.srcAccessMask = map_access2(barrier.srcAccessMask);
    barrier.dstAccessMask = map_access2(barrier.dstAccessMask);
}
template <typename T>
bool valid_array(uint32_t count, const T* pointer) noexcept {
    return count == 0 || pointer;
}
template <typename T>
void copy_array(std::vector<T>& destination, uint32_t count, const T* source) {
    if (count) destination.assign(source, source + count);
}
}  // namespace
VkPipelineStageFlags2 map_stage2(VkPipelineStageFlags2 stages) noexcept {
    auto result = stages & ~VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
    if ((stages & (VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT | as_stages)) && !(stages & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
        result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    return result;
}
VkAccessFlags2 map_access2(VkAccessFlags2 accesses) noexcept {
    auto result = accesses & ~micromap_access;
    if (accesses & (VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR))
        result |= VK_ACCESS_2_SHADER_READ_BIT;
    if (accesses & (VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR))
        result |= VK_ACCESS_2_SHADER_WRITE_BIT;
    return result;
}
VkPipelineStageFlags map_stage(VkPipelineStageFlags stages) noexcept {
    static_assert(VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT <= std::numeric_limits<VkPipelineStageFlags>::max());
    const auto micromap = static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT);
    auto result = stages & ~micromap;
    if ((stages & (micromap | static_cast<VkPipelineStageFlags>(as_stages))) && !(stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))
        result |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    return result;
}
VkAccessFlags map_access(VkAccessFlags accesses) noexcept {
    auto result = accesses;
    if (accesses & VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR) result |= VK_ACCESS_SHADER_READ_BIT;
    if (accesses & VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) result |= VK_ACCESS_SHADER_WRITE_BIT;
    return result;
}
TranslatedDependency::TranslatedDependency() noexcept { source_.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; }
VkResult TranslatedDependency::assign(const VkDependencyInfo& source) noexcept {
    if (!valid_array(source.memoryBarrierCount, source.pMemoryBarriers) ||
        !valid_array(source.bufferMemoryBarrierCount, source.pBufferMemoryBarriers) ||
        !valid_array(source.imageMemoryBarrierCount, source.pImageMemoryBarriers))
        return VK_ERROR_INITIALIZATION_FAILED;
    try {
        TranslatedDependency next;
        next.source_ = source;
        size_t metadata_count = 0;
        for (uint32_t i = 0; i < source.bufferMemoryBarrierCount; ++i)
            if ((source.pBufferMemoryBarriers[i].srcAccessMask | source.pBufferMemoryBarriers[i].dstAccessMask) & as_access)
                ++metadata_count;
        if (metadata_count > UINT32_MAX - source.memoryBarrierCount) return VK_ERROR_OUT_OF_HOST_MEMORY;
        next.memory_.reserve(size_t(source.memoryBarrierCount) + metadata_count);
        copy_array(next.memory_, source.memoryBarrierCount, source.pMemoryBarriers);
        copy_array(next.buffers_, source.bufferMemoryBarrierCount, source.pBufferMemoryBarriers);
        copy_array(next.images_, source.imageMemoryBarrierCount, source.pImageMemoryBarriers);
        for (auto& barrier : next.memory_) translate(barrier);
        for (auto& barrier : next.buffers_) translate(barrier);
        for (auto& barrier : next.images_) translate(barrier);
        // Keep source/destination scopes paired per barrier. Merging masks from
        // separate barriers could invent dependencies that the app did not ask
        // for, particularly across event release/acquire NONE scopes.
        for (uint32_t i = 0; i < source.bufferMemoryBarrierCount; ++i) {
            const auto& original = source.pBufferMemoryBarriers[i];
            if (!((original.srcAccessMask | original.dstAccessMask) & as_access)) continue;
            VkMemoryBarrier2 metadata{};
            metadata.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            metadata.srcStageMask = original.srcStageMask;
            metadata.dstStageMask = original.dstStageMask;
            metadata.srcAccessMask = sidecar_access(original.srcStageMask, original.srcAccessMask);
            metadata.dstAccessMask = sidecar_access(original.dstStageMask, original.dstAccessMask);
            metadata.srcStageMask = map_stage2(metadata.srcStageMask);
            metadata.dstStageMask = map_stage2(metadata.dstStageMask);
            // The native AS access stays in the original buffer barrier. Only
            // shader/global-memory accesses are relevant to hidden sidecars.
            next.memory_.push_back(metadata);
        }
        // Raw original array pointers are never exposed by view().
        next.source_.pMemoryBarriers = nullptr;
        next.source_.pBufferMemoryBarriers = nullptr;
        next.source_.pImageMemoryBarriers = nullptr;
        *this = std::move(next);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkDependencyInfo TranslatedDependency::view() const noexcept {
    auto result = source_;
    result.memoryBarrierCount = static_cast<uint32_t>(memory_.size());
    result.bufferMemoryBarrierCount = static_cast<uint32_t>(buffers_.size());
    result.imageMemoryBarrierCount = static_cast<uint32_t>(images_.size());
    result.pMemoryBarriers = memory_.empty() ? nullptr : memory_.data();
    result.pBufferMemoryBarriers = buffers_.empty() ? nullptr : buffers_.data();
    result.pImageMemoryBarriers = images_.empty() ? nullptr : images_.data();
    return result;
}
VkResult TranslatedLegacyBarriers::assign(VkPipelineStageFlags source_stages, VkPipelineStageFlags destination_stages,
                                          VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier* memory,
                                          uint32_t buffer_count, const VkBufferMemoryBarrier* buffers, uint32_t image_count,
                                          const VkImageMemoryBarrier* images) noexcept {
    if (!valid_array(memory_count, memory) || !valid_array(buffer_count, buffers) || !valid_array(image_count, images))
        return VK_ERROR_INITIALIZATION_FAILED;
    try {
        TranslatedLegacyBarriers next;
        next.source_stages_ = map_stage(source_stages);
        next.destination_stages_ = map_stage(destination_stages);
        next.flags_ = flags;
        size_t metadata_count = 0;
        for (uint32_t i = 0; i < buffer_count; ++i)
            if ((buffers[i].srcAccessMask | buffers[i].dstAccessMask) & static_cast<VkAccessFlags>(as_access)) ++metadata_count;
        if (metadata_count > UINT32_MAX - memory_count) return VK_ERROR_OUT_OF_HOST_MEMORY;
        next.memory_.reserve(size_t(memory_count) + metadata_count);
        copy_array(next.memory_, memory_count, memory);
        copy_array(next.buffers_, buffer_count, buffers);
        copy_array(next.images_, image_count, images);
        for (auto& barrier : next.memory_) {
            barrier.srcAccessMask = map_access(barrier.srcAccessMask);
            barrier.dstAccessMask = map_access(barrier.dstAccessMask);
        }
        for (auto& barrier : next.buffers_) {
            barrier.srcAccessMask = map_access(barrier.srcAccessMask);
            barrier.dstAccessMask = map_access(barrier.dstAccessMask);
        }
        for (auto& barrier : next.images_) {
            barrier.srcAccessMask = map_access(barrier.srcAccessMask);
            barrier.dstAccessMask = map_access(barrier.dstAccessMask);
        }
        for (uint32_t i = 0; i < buffer_count; ++i) {
            if (!((buffers[i].srcAccessMask | buffers[i].dstAccessMask) & static_cast<VkAccessFlags>(as_access))) continue;
            VkMemoryBarrier metadata{};
            metadata.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            metadata.srcAccessMask = static_cast<VkAccessFlags>(sidecar_access(source_stages, buffers[i].srcAccessMask));
            metadata.dstAccessMask = static_cast<VkAccessFlags>(sidecar_access(destination_stages, buffers[i].dstAccessMask));
            next.memory_.push_back(metadata);
        }
        *this = std::move(next);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
LegacyBarrierView TranslatedLegacyBarriers::view() const noexcept {
    return {source_stages_,
            destination_stages_,
            flags_,
            static_cast<uint32_t>(memory_.size()),
            memory_.empty() ? nullptr : memory_.data(),
            static_cast<uint32_t>(buffers_.size()),
            buffers_.empty() ? nullptr : buffers_.data(),
            static_cast<uint32_t>(images_.size()),
            images_.empty() ? nullptr : images_.data()};
}
}  // namespace vel::omm
