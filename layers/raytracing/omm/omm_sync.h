// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace vel::omm {
// EXT micromap build/copy executes in compute. Native AS build/copy also has
// compute metadata work: retain its native stage/access bits and add shader
// scopes. ALL_COMMANDS/ALL_GRAPHICS, zero masks and unknown bits are retained.
VkPipelineStageFlags2 map_stage2(VkPipelineStageFlags2 stages) noexcept;
VkAccessFlags2 map_access2(VkAccessFlags2 accesses) noexcept;
// Micromap/AS-copy stage wire bits fit in 32 bits; mapping these defensively is
// not a claim that synchronization2-only names are legacy enumerants. Stage
// mapping can return multiple bits and must not be used for a single-bit API.
VkPipelineStageFlags map_stage(VkPipelineStageFlags stages) noexcept;
VkAccessFlags map_access(VkAccessFlags accesses) noexcept;

// Owns barrier arrays. Every pNext remains borrowed, unchanged, and must remain
// live through the downstream command call. This does not translate pNext data.
// view() refreshes owned array pointers, so copies/moves have independent views.
// AS-access buffer barriers additionally produce global metadata memory barriers:
// their original VkBuffers do not cover separate private sidecar allocations.
// The original buffer ranges and queue-family ownership indices are preserved.
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
