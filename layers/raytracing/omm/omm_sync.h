// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace vel::omm {
// EXT micromap build/copy executes in compute in this backend. Other bits,
// including ALL_COMMANDS/ALL_GRAPHICS, zero masks and unknown bits, are retained.
VkPipelineStageFlags2 map_stage2(VkPipelineStageFlags2 stages) noexcept;
VkAccessFlags2 map_access2(VkAccessFlags2 accesses) noexcept;
// Micromap has no legacy access enumerants. Its stage wire bit fits in 32 bits;
// mapping it is defensive support, not a claim that it is a legacy enumerant.
VkPipelineStageFlags map_stage(VkPipelineStageFlags stages) noexcept;
VkAccessFlags map_access(VkAccessFlags accesses) noexcept;

// Owns barrier arrays. Every pNext remains borrowed, unchanged, and must remain
// live through the downstream command call. This does not translate pNext data.
// view() refreshes owned array pointers, so copies/moves have independent views.
class TranslatedDependency {
  public:
    TranslatedDependency() noexcept;
    VkResult assign(const VkDependencyInfo& source) noexcept;
    VkDependencyInfo view() const noexcept;

  private:
    VkDependencyInfo source_{};
    std::vector<VkMemoryBarrier2> memory_;
    std::vector<VkBufferMemoryBarrier2> buffers_;
    std::vector<VkImageMemoryBarrier2> images_;
};

struct LegacyBarrierView {
    VkPipelineStageFlags src_stage_mask;
    VkPipelineStageFlags dst_stage_mask;
    VkDependencyFlags dependency_flags;
    uint32_t memory_barrier_count;
    const VkMemoryBarrier* memory_barriers;
    uint32_t buffer_memory_barrier_count;
    const VkBufferMemoryBarrier* buffer_memory_barriers;
    uint32_t image_memory_barrier_count;
    const VkImageMemoryBarrier* image_memory_barriers;
};
class TranslatedLegacyBarriers {
  public:
    VkResult assign(VkPipelineStageFlags source_stages, VkPipelineStageFlags destination_stages, VkDependencyFlags flags,
                    uint32_t memory_count, const VkMemoryBarrier* memory, uint32_t buffer_count,
                    const VkBufferMemoryBarrier* buffers, uint32_t image_count, const VkImageMemoryBarrier* images) noexcept;
    LegacyBarrierView view() const noexcept;

  private:
    VkPipelineStageFlags source_stages_ = 0;
    VkPipelineStageFlags destination_stages_ = 0;
    VkDependencyFlags flags_ = 0;
    std::vector<VkMemoryBarrier> memory_;
    std::vector<VkBufferMemoryBarrier> buffers_;
    std::vector<VkImageMemoryBarrier> images_;
};
}  // namespace vel::omm
