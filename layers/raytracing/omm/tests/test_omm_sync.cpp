// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_sync.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

static thread_local int allocationBudget = -1;
void* operator new(std::size_t size) {
    if (allocationBudget >= 0 && allocationBudget-- == 0) throw std::bad_alloc();
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
using namespace vel::omm;
namespace {
unsigned checks = 0, failures = 0;
void expect(bool condition, const char* text) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", text);
    }
}
template <typename T>
T init(VkStructureType type) {
    T result{};
    result.sType = type;
    return result;
}
template <typename T>
T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}
void masks() {
    constexpr auto microStage = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
    constexpr auto microRead = VK_ACCESS_2_MICROMAP_READ_BIT_EXT, microWrite = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
    expect(map_stage2(0) == 0 && map_access2(0) == 0, "NONE stage and zero access remain zero");
    expect(map_stage2(microStage) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, "micromap build executes as compute");
    expect(map_access2(microRead) == VK_ACCESS_2_SHADER_READ_BIT && map_access2(microWrite) == VK_ACCESS_2_SHADER_WRITE_BIT,
           "micromap read/write map to generic shader read/write");
    constexpr VkPipelineStageFlags2 otherStages = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                                  VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | (1ull << 63);
    expect(map_stage2(otherStages) == otherStages, "unrelated stages, ALL_GRAPHICS and unknown high bits preserved exactly");
    expect(map_stage2(otherStages | microStage) == (otherStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
           "mixed stage mask retains all unrelated bits");
    expect(map_stage2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
           "ALL_COMMANDS already includes compute");
    constexpr VkAccessFlags2 otherAccess = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                           (1ull << 63);
    expect(map_access2(otherAccess) == otherAccess, "specific shader access aliases, global memory and unknown bits preserved");
    expect(map_access2(otherAccess | microRead | microWrite) ==
               (otherAccess | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
           "mixed access masks remove only micromap accesses");
    for (unsigned bit = 0; bit < 64; ++bit) {
        const auto mask = VkPipelineStageFlags2(1) << bit;
        expect(map_stage2(map_stage2(mask)) == map_stage2(mask), "stage translation idempotent for every bit");
        expect(map_access2(map_access2(mask)) == map_access2(mask), "access translation idempotent for every bit");
        if (!(mask & microStage)) expect(map_stage2(mask) == mask, "each unrelated stage bit survives");
        if (!(mask & (microRead | microWrite))) expect(map_access2(mask) == mask, "each unrelated access bit survives");
    }
    const auto wire = static_cast<VkPipelineStageFlags>(microStage);
    expect(map_stage(wire | VK_PIPELINE_STAGE_HOST_BIT) == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT),
           "legacy defensive stage wire mapping (not an advertised legacy enumerant)");
    expect(map_stage(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT) ==
               (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT),
           "legacy aggregate stage masks unchanged");
    expect(map_access(0xffffffffu) == 0xffffffffu, "legacy access has no representable micromap bits and is unchanged");
}
struct Input {
    // Opaque extension sentinels are deliberately never interpreted by the
    // translator; these host copies do not claim native pNext validity.
    VkBaseInStructure chain[4]{};
    VkMemoryBarrier2 memory[2]{};
    VkBufferMemoryBarrier2 buffer{};
    VkImageMemoryBarrier2 image{};
    VkDependencyInfo dependency{};
    Input() {
        for (auto& value : chain) value.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        memory[0] = init<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        memory[0].pNext = &chain[1];
        memory[0].srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT | VK_PIPELINE_STAGE_2_HOST_BIT;
        memory[0].srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_HOST_WRITE_BIT;
        memory[0].dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        memory[0].dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        memory[1] = init<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        memory[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        memory[1].srcAccessMask = 0;
        memory[1].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        memory[1].dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MEMORY_READ_BIT;
        buffer = init<VkBufferMemoryBarrier2>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
        buffer.pNext = &chain[2];
        buffer.srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
        buffer.srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
        buffer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        buffer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        buffer.srcQueueFamilyIndex = 3;
        buffer.dstQueueFamilyIndex = 5;
        buffer.buffer = handle<VkBuffer>(0x1234);
        buffer.offset = 64;
        buffer.size = VK_WHOLE_SIZE;
        image = init<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        image.pNext = &chain[3];
        image.srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        image.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        image.dstStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
        image.dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
        image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        image.dstQueueFamilyIndex = 7;
        image.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        image.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image.image = handle<VkImage>(0x5678);
        image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 4, 5};
        dependency = init<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.pNext = &chain[0];
        dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_DEVICE_GROUP_BIT;
        dependency.memoryBarrierCount = 2;
        dependency.pMemoryBarriers = memory;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &buffer;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &image;
    }
};
void dependencyCopies() {
    Input input;
    const auto saved = input;
    TranslatedDependency output;
    expect(output.assign(input.dependency) == VK_SUCCESS, "translate memory, buffer and image dependency arrays");
    const auto view = output.view();
    expect(!std::memcmp(&input, &saved, sizeof(input)), "original dependency and all pointed-to arrays remain unchanged");
    expect(view.sType == input.dependency.sType && view.pNext == input.dependency.pNext &&
               view.dependencyFlags == input.dependency.dependencyFlags,
           "dependency header, flags and borrowed pNext preserved");
    expect(view.memoryBarrierCount == 2 && view.bufferMemoryBarrierCount == 1 && view.imageMemoryBarrierCount == 1 &&
               view.pMemoryBarriers != input.memory && view.pBufferMemoryBarriers != &input.buffer &&
               view.pImageMemoryBarriers != &input.image,
           "every nonempty barrier array is deeply copied");
    const auto& memory = view.pMemoryBarriers[0];
    expect(memory.sType == input.memory[0].sType && memory.pNext == input.memory[0].pNext &&
               memory.srcStageMask == (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT) &&
               memory.srcAccessMask == (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT),
           "memory source mask maps to compute shader writes");
    expect(memory.dstStageMask ==
                   (VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) &&
               memory.dstAccessMask == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR),
           "native AS stage remains while translated micromap read adds compute stage support");
    expect(view.pMemoryBarriers[1].srcStageMask == 0 && view.pMemoryBarriers[1].srcAccessMask == 0 &&
               view.pMemoryBarriers[1].dstStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
               view.pMemoryBarriers[1].dstAccessMask == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT),
           "NONE side and ALL_COMMANDS aggregate retain their scopes");
    const auto& buffer = view.pBufferMemoryBarriers[0];
    expect(buffer.pNext == input.buffer.pNext && buffer.buffer == input.buffer.buffer && buffer.offset == 64 &&
               buffer.size == VK_WHOLE_SIZE && buffer.srcQueueFamilyIndex == 3 && buffer.dstQueueFamilyIndex == 5,
           "buffer handle, range, queue ownership and pNext preserved");
    expect(buffer.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && buffer.srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT &&
               buffer.dstStageMask == input.buffer.dstStageMask && buffer.dstAccessMask == input.buffer.dstAccessMask,
           "buffer barrier leaves unrelated transfer/host destinations unchanged");
    const auto& image = view.pImageMemoryBarriers[0];
    expect(image.pNext == input.image.pNext && image.image == input.image.image && image.oldLayout == input.image.oldLayout &&
               image.newLayout == input.image.newLayout && image.srcQueueFamilyIndex == input.image.srcQueueFamilyIndex &&
               image.dstQueueFamilyIndex == 7 &&
               !std::memcmp(&image.subresourceRange, &input.image.subresourceRange, sizeof(VkImageSubresourceRange)),
           "image layout transition, subresource range, queue ownership and pNext preserved");
    expect(image.srcStageMask == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT && image.srcAccessMask == VK_ACCESS_2_SHADER_READ_BIT &&
               image.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
               image.dstAccessMask == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
           "image barrier source untouched and destination masks translated");
    auto copy = output;
    expect(copy.view().pMemoryBarriers != view.pMemoryBarriers && copy.view().pBufferMemoryBarriers != view.pBufferMemoryBarriers &&
               copy.view().pImageMemoryBarriers != view.pImageMemoryBarriers,
           "copied translator owns independent arrays");
    TranslatedDependency moved = std::move(copy);
    const auto movedView = moved.view();
    expect(movedView.memoryBarrierCount == 2 && movedView.pMemoryBarriers[0].dstAccessMask == memory.dstAccessMask,
           "moved translator view refreshes array pointers");
    input.memory[0].srcAccessMask = 0;
    input.buffer.offset = 4096;
    input.image.subresourceRange.baseMipLevel = 9;
    expect(view.pMemoryBarriers[0].srcAccessMask != 0 && view.pBufferMemoryBarriers[0].offset == 64 &&
               view.pImageMemoryBarriers[0].subresourceRange.baseMipLevel == 2,
           "subsequent application array reuse cannot change owned barrier records");
    input.chain[1].pNext = &input.chain[0];
    expect(static_cast<const VkBaseInStructure*>(view.pMemoryBarriers[0].pNext)->pNext == &input.chain[0],
           "pNext explicitly remains borrowed rather than cloned or interpreted");
    auto empty = init<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    empty.pNext = &input.chain[0];
    empty.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    empty.pMemoryBarriers = input.memory;
    empty.pBufferMemoryBarriers = &input.buffer;
    empty.pImageMemoryBarriers = &input.image;
    allocationBudget = 0;
    const auto result = output.assign(empty);
    allocationBudget = -1;
    const auto emptyView = output.view();
    expect(result == VK_SUCCESS && emptyView.pNext == empty.pNext && emptyView.memoryBarrierCount == 0 &&
               emptyView.bufferMemoryBarrierCount == 0 && emptyView.imageMemoryBarrierCount == 0 && !emptyView.pMemoryBarriers &&
               !emptyView.pBufferMemoryBarriers && !emptyView.pImageMemoryBarriers,
           "zero counts require no allocation or pointer dereference");
    expect(moved.view().pMemoryBarriers[0].srcAccessMask == (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT),
           "independent move survives another translator reset");
}
void failuresAndLegacy() {
    Input input;
    TranslatedDependency translated;
    expect(translated.assign(input.dependency) == VK_SUCCESS, "seed translated dependency before failures");
    auto before = translated.view();
    const auto savedMemory = before.pMemoryBarriers[0];
    const auto savedBuffer = before.pBufferMemoryBarriers[0];
    for (int budget : {0, 1, 2}) {
        allocationBudget = budget;
        const auto result = translated.assign(input.dependency);
        allocationBudget = -1;
        const auto after = translated.view();
        expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && after.pMemoryBarriers == before.pMemoryBarriers &&
                   after.pBufferMemoryBarriers == before.pBufferMemoryBarriers &&
                   after.pImageMemoryBarriers == before.pImageMemoryBarriers &&
                   after.pMemoryBarriers[0].srcAccessMask == savedMemory.srcAccessMask &&
                   after.pBufferMemoryBarriers[0].offset == savedBuffer.offset,
               "each deep-copy allocation failure preserves the complete previous dependency atomically");
    }
    auto invalid = input.dependency;
    invalid.pBufferMemoryBarriers = nullptr;
    expect(
        translated.assign(invalid) == VK_ERROR_INITIALIZATION_FAILED && translated.view().pMemoryBarriers == before.pMemoryBarriers,
        "nonzero null array rejected without replacing previous output");
    auto memory = init<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    memory.pNext = &input.chain[1];
    memory.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    auto buffer = init<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    buffer.pNext = &input.chain[2];
    buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    buffer.srcQueueFamilyIndex = 2;
    buffer.dstQueueFamilyIndex = 4;
    buffer.buffer = handle<VkBuffer>(99);
    buffer.offset = 12;
    buffer.size = VK_WHOLE_SIZE;
    auto image = init<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    image.pNext = &input.chain[3];
    image.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    image.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    image.image = handle<VkImage>(100);
    image.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 3, 4};
    const auto savedLegacyMemory = memory;
    const auto savedLegacyBuffer = buffer;
    const auto savedLegacyImage = image;
    TranslatedLegacyBarriers legacy;
    const auto source = static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT) | VK_PIPELINE_STAGE_HOST_BIT;
    const auto destination = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    expect(legacy.assign(source, destination, VK_DEPENDENCY_BY_REGION_BIT, 1, &memory, 1, &buffer, 1, &image) == VK_SUCCESS,
           "legacy helper clones all barrier arrays");
    const auto view = legacy.view();
    expect(view.src_stage_mask == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT) &&
               view.dst_stage_mask == destination && view.dependency_flags == VK_DEPENDENCY_BY_REGION_BIT,
           "legacy outer stage masks translated and flags preserved");
    expect(view.memory_barriers != &memory && view.buffer_memory_barriers != &buffer && view.image_memory_barriers != &image &&
               !std::memcmp(view.memory_barriers, &savedLegacyMemory, sizeof(memory)) &&
               !std::memcmp(view.buffer_memory_barriers, &savedLegacyBuffer, sizeof(buffer)) &&
               !std::memcmp(view.image_memory_barriers, &savedLegacyImage, sizeof(image)),
           "legacy structures copied exactly because no legacy micromap access exists");
    expect(!std::memcmp(&memory, &savedLegacyMemory, sizeof(memory)) && !std::memcmp(&buffer, &savedLegacyBuffer, sizeof(buffer)) &&
               !std::memcmp(&image, &savedLegacyImage, sizeof(image)),
           "original legacy arrays immutable");
    auto independent = legacy;
    auto moved = std::move(independent);
    expect(moved.view().memory_barriers != view.memory_barriers && moved.view().memory_barriers[0].pNext == memory.pNext,
           "legacy copy/move refreshes owned arrays and retains borrowed pNext");
    for (int budget : {0, 1, 2}) {
        allocationBudget = budget;
        const auto result = legacy.assign(source, destination, 0, 1, &memory, 1, &buffer, 1, &image);
        allocationBudget = -1;
        const auto after = legacy.view();
        expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && after.dependency_flags == view.dependency_flags &&
                   after.memory_barriers == view.memory_barriers && after.buffer_memory_barriers == view.buffer_memory_barriers &&
                   after.image_memory_barriers == view.image_memory_barriers,
               "legacy deep-copy allocation errors leave old view untouched");
    }
    expect(legacy.assign(source, destination, 0, 1, nullptr, 0, nullptr, 0, nullptr) == VK_ERROR_INITIALIZATION_FAILED &&
               legacy.view().memory_barriers == view.memory_barriers,
           "invalid legacy array rejected atomically");
    allocationBudget = 0;
    const auto result = legacy.assign(0, 0, 0, 0, &memory, 0, &buffer, 0, &image);
    allocationBudget = -1;
    const auto empty = legacy.view();
    expect(result == VK_SUCCESS && empty.src_stage_mask == 0 && empty.dst_stage_mask == 0 && !empty.memory_barrier_count &&
               !empty.buffer_memory_barrier_count && !empty.image_memory_barrier_count && !empty.memory_barriers &&
               !empty.buffer_memory_barriers && !empty.image_memory_barriers,
           "legacy zero-count arrays neither allocate nor dereference ignored pointers");
}
}  // namespace
int main() {
    masks();
    dependencyCopies();
    failuresAndLegacy();
    std::printf("OMM synchronization translation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
