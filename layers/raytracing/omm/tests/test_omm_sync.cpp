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
    expect(map_stage2(otherStages) == (otherStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
           "native AS stage gains compute and unrelated/unknown stages remain");
    expect(map_stage2(otherStages | microStage) == (otherStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
           "mixed stage mask retains all unrelated bits");
    expect(map_stage2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
           "ALL_COMMANDS already includes compute");
    constexpr VkAccessFlags2 otherAccess = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                           (1ull << 63);
    expect(map_access2(otherAccess) == (otherAccess | VK_ACCESS_2_SHADER_READ_BIT),
           "native AS read gains shader read and specific/global/unknown accesses remain");
    expect(map_access2(otherAccess | microRead | microWrite) ==
               (otherAccess | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
           "mixed access masks remove only micromap accesses");
    for (unsigned bit = 0; bit < 64; ++bit) {
        const auto mask = VkPipelineStageFlags2(1) << bit;
        expect(map_stage2(map_stage2(mask)) == map_stage2(mask), "stage translation idempotent for every bit");
        expect(map_access2(map_access2(mask)) == map_access2(mask), "access translation idempotent for every bit");
        if (!(mask & (microStage | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR)))
            expect(map_stage2(mask) == mask, "each unrelated stage bit survives");
        if (!(mask & (microRead | microWrite | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)))
            expect(map_access2(mask) == mask, "each unrelated access bit survives");
    }
    const auto wire = static_cast<VkPipelineStageFlags>(microStage);
    expect(map_stage(wire | VK_PIPELINE_STAGE_HOST_BIT) == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT),
           "legacy defensive stage wire mapping (not an advertised legacy enumerant)");
    expect(map_stage(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT) ==
               (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT),
           "legacy aggregate stage masks unchanged");
    expect(map_access(0xffffffffu) == 0xffffffffu, "legacy access has no representable micromap bits and is unchanged");
    expect(map_stage2(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR) ==
               (VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
           "native AS copy scope includes compute sidecar work while retaining native copy");
    expect(map_stage(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR) ==
                   (VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) &&
               map_access(VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) ==
                   (VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_WRITE_BIT),
           "legacy AS build release includes native and private metadata writers");
    expect(map_stage2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR) ==
               (VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR),
           "ALL_COMMANDS already covers private compute without extra stage bits");
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
    expect(view.src_stage_mask ==
                   (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT) &&
               view.dst_stage_mask == destination && view.dependency_flags == VK_DEPENDENCY_BY_REGION_BIT,
           "legacy outer stage masks translated and flags preserved");
    auto expectedLegacyBuffer = savedLegacyBuffer;
    expectedLegacyBuffer.srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT;
    expect(view.memory_barriers != &memory && view.buffer_memory_barriers != &buffer && view.image_memory_barriers != &image &&
               !std::memcmp(view.memory_barriers, &savedLegacyMemory, sizeof(memory)) &&
               !std::memcmp(view.buffer_memory_barriers, &expectedLegacyBuffer, sizeof(buffer)) &&
               !std::memcmp(view.image_memory_barriers, &savedLegacyImage, sizeof(image)),
           "legacy structures preserve ranges and borrowed chains while serialized transfer writes gain shader scope");
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
void nativeSidecarScopes() {
    VkBufferMemoryBarrier2 buffers[3]{};
    for (auto& value : buffers) {
        value = init<VkBufferMemoryBarrier2>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
        value.buffer = handle<VkBuffer>(0x1234);
        value.offset = 96;
        value.size = 128;
        value.srcQueueFamilyIndex = 1;
        value.dstQueueFamilyIndex = 2;
    }
    // A release event source scope. The hidden buffers use concurrent sharing,
    // so their availability must be global without inventing ownership changes.
    buffers[0].srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    buffers[0].srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    buffers[0].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    buffers[0].dstAccessMask = 0;
    // A distinct acquire destination, deliberately not paired with release0.
    buffers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    buffers[1].srcAccessMask = 0;
    buffers[1].dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    buffers[1].dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    buffers[2].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    buffers[2].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    buffers[2].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    buffers[2].dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    const auto saved0 = buffers[0], saved1 = buffers[1], saved2 = buffers[2];
    auto memory = init<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    memory.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    memory.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    auto image = init<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    auto dependency = init<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    dependency.bufferMemoryBarrierCount = 3;
    dependency.pBufferMemoryBarriers = buffers;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &image;
    TranslatedDependency translated;
    expect(translated.assign(dependency) == VK_SUCCESS, "AS release/acquire event dependency translates");
    const auto view = translated.view();
    expect(view.memoryBarrierCount == 3 && view.bufferMemoryBarrierCount == 3,
           "each AS buffer scope adds global metadata barrier and keeps every original buffer record");
    expect(!std::memcmp(view.pMemoryBarriers, &memory, sizeof(memory)),
           "unrelated global host-to-transfer barrier remains exact and precedes appended metadata");
    const auto& release = view.pMemoryBarriers[1];
    expect(release.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 && !release.pNext &&
               release.srcStageMask ==
                   (VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) &&
               release.srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT && release.dstStageMask == 0 && release.dstAccessMask == 0,
           "AS release makes hidden compute writes available globally and retains an empty event destination");
    const auto& acquire = view.pMemoryBarriers[2];
    expect(acquire.srcStageMask == 0 && acquire.srcAccessMask == 0 &&
               acquire.dstStageMask == VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR &&
               acquire.dstAccessMask == VK_ACCESS_2_SHADER_READ_BIT,
           "AS acquire reaches classifier shader reads without manufacturing a release-to-acquire pair");
    expect(view.pBufferMemoryBarriers[0].srcAccessMask ==
                   (VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_WRITE_BIT) &&
               view.pBufferMemoryBarriers[1].dstAccessMask ==
                   (VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT),
           "native AS memory access bits remain in original buffer scopes");
    for (unsigned i = 0; i < 3; ++i) {
        const auto& actual = view.pBufferMemoryBarriers[i];
        expect(actual.buffer == buffers[i].buffer && actual.offset == 96 && actual.size == 128 && actual.srcQueueFamilyIndex == 1 &&
                   actual.dstQueueFamilyIndex == 2,
               "native buffer ranges and ownership transfer indices are preserved");
    }
    expect(!std::memcmp(&view.pBufferMemoryBarriers[2], &saved2, sizeof(saved2)),
           "unrelated transfer-to-vertex buffer barrier gains no shader scopes");
    expect(!std::memcmp(&buffers[0], &saved0, sizeof(saved0)) && !std::memcmp(&buffers[1], &saved1, sizeof(saved1)),
           "AS event dependency input arrays remain untouched");
    for (int budget : {0, 1, 2}) {
        allocationBudget = budget;
        const auto result = translated.assign(dependency);
        allocationBudget = -1;
        const auto after = translated.view();
        expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && after.memoryBarrierCount == 3 &&
                   after.pMemoryBarriers == view.pMemoryBarriers && after.pBufferMemoryBarriers == view.pBufferMemoryBarriers &&
                   after.pImageMemoryBarriers == view.pImageMemoryBarriers &&
                   after.pMemoryBarriers[1].srcAccessMask == VK_ACCESS_2_SHADER_WRITE_BIT,
               "OOM including appended global scopes atomically preserves the prior complete dependency");
    }
    auto legacyBuffer = init<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    legacyBuffer.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    legacyBuffer.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    legacyBuffer.srcQueueFamilyIndex = 4;
    legacyBuffer.dstQueueFamilyIndex = 8;
    legacyBuffer.buffer = handle<VkBuffer>(0x4567);
    legacyBuffer.offset = 192;
    legacyBuffer.size = 256;
    TranslatedLegacyBarriers legacy;
    expect(legacy.assign(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                         nullptr, 1, &legacyBuffer, 0, nullptr) == VK_SUCCESS,
           "legacy AS barrier/wait-event translation succeeds");
    const auto lv = legacy.view();
    expect(lv.src_stage_mask == (VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) &&
               lv.dst_stage_mask == VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR && lv.memory_barrier_count == 1 &&
               lv.memory_barriers[0].srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT &&
               lv.memory_barriers[0].dstAccessMask == VK_ACCESS_SHADER_READ_BIT,
           "legacy AS buffer barrier globally synchronizes hidden compute writes with ray tracing classifier reads");
    expect(lv.buffer_memory_barrier_count == 1 && lv.buffer_memory_barriers[0].buffer == legacyBuffer.buffer &&
               lv.buffer_memory_barriers[0].offset == 192 && lv.buffer_memory_barriers[0].size == 256 &&
               lv.buffer_memory_barriers[0].srcQueueFamilyIndex == 4 && lv.buffer_memory_barriers[0].dstQueueFamilyIndex == 8 &&
               lv.buffer_memory_barriers[0].srcAccessMask ==
                   (VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_WRITE_BIT),
           "legacy native AS barrier retains its range, ownership and native AS access");
    for (int budget : {0, 1}) {
        allocationBudget = budget;
        const auto result =
            legacy.assign(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
                          0, nullptr, 1, &legacyBuffer, 0, nullptr);
        allocationBudget = -1;
        expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && legacy.view().memory_barriers == lv.memory_barriers &&
                   legacy.view().buffer_memory_barriers == lv.buffer_memory_barriers,
               "legacy global sidecar barrier and native buffer copy commit atomically under OOM");
    }
    buffers[0].dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    buffers[0].dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    buffers[1].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    buffers[1].srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    dependency.memoryBarrierCount = 0;
    dependency.pMemoryBarriers = nullptr;
    dependency.bufferMemoryBarrierCount = 2;
    dependency.imageMemoryBarrierCount = 0;
    dependency.pImageMemoryBarriers = nullptr;
    expect(translated.assign(dependency) == VK_SUCCESS, "ownership scopes with ignored nonzero access masks translate");
    const auto ignored = translated.view();
    expect(ignored.memoryBarrierCount == 2 && ignored.pMemoryBarriers[0].dstStageMask == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT &&
               ignored.pMemoryBarriers[0].dstAccessMask == 0 &&
               ignored.pMemoryBarriers[1].srcStageMask == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT &&
               ignored.pMemoryBarriers[1].srcAccessMask == 0 &&
               ignored.pBufferMemoryBarriers[0].dstAccessMask ==
                   (VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT),
           "global sidecar barriers empty ignored TOP/BOTTOM memory scopes while native ownership accesses stay preserved");
    expect(legacy.assign(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 1,
                         &legacyBuffer, 0, nullptr) == VK_SUCCESS &&
               legacy.view().src_stage_mask == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT &&
               legacy.view().memory_barriers[0].srcAccessMask == 0 &&
               legacy.view().memory_barriers[0].dstAccessMask == VK_ACCESS_SHADER_READ_BIT,
           "legacy acquire keeps ignored source empty without expanding event execution scopes");
}
void serializedBufferScopes() {
    VkMemoryBarrier2 memory = init<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
    memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkBufferMemoryBarrier2 buffer = init<VkBufferMemoryBarrier2>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
    buffer.srcStageMask = memory.srcStageMask;
    buffer.dstStageMask = memory.dstStageMask;
    buffer.srcAccessMask = memory.srcAccessMask;
    buffer.dstAccessMask = memory.dstAccessMask;
    buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer.buffer = handle<VkBuffer>(0x1234);
    buffer.offset = 256;
    buffer.size = 144;
    auto dependency = init<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.memoryBarrierCount = dependency.bufferMemoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    dependency.pBufferMemoryBarriers = &buffer;
    TranslatedDependency translated;
    expect(translated.assign(dependency) == VK_SUCCESS, "serialized buffer dependency translates");
    auto view = translated.view();
    const auto stages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    expect(view.pMemoryBarriers[0].srcStageMask == stages && view.pMemoryBarriers[0].dstStageMask == stages &&
               view.pMemoryBarriers[0].srcAccessMask == (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT) &&
               view.pMemoryBarriers[0].dstAccessMask == (VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT),
           "MM-stage serialized transfer accesses include actual compute writers and readers");
    expect(view.pBufferMemoryBarriers[0].srcStageMask == stages &&
               view.pBufferMemoryBarriers[0].srcAccessMask == view.pMemoryBarriers[0].srcAccessMask &&
               view.pBufferMemoryBarriers[0].dstAccessMask == view.pMemoryBarriers[0].dstAccessMask &&
               view.pBufferMemoryBarriers[0].buffer == buffer.buffer && view.pBufferMemoryBarriers[0].offset == 256 &&
               view.pBufferMemoryBarriers[0].size == 144 && view.memoryBarrierCount == 1,
           "serialization buffer range is retained without an unrelated sidecar barrier");
    TranslatedDependency twice;
    expect(twice.assign(view) == VK_SUCCESS && twice.view().pMemoryBarriers[0].srcStageMask == stages &&
               twice.view().pMemoryBarriers[0].dstAccessMask == view.pMemoryBarriers[0].dstAccessMask,
           "serialized scope translation is idempotent");
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    expect(
        translated.assign(dependency, true) == VK_SUCCESS &&
            translated.view().pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
            translated.view().pMemoryBarriers[0].srcAccessMask == (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT),
        "ALL_COMMANDS includes shader serialization on a compute queue");
    expect(translated.assign(dependency, false) == VK_SUCCESS &&
               translated.view().pMemoryBarriers[0].srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
               translated.view().pMemoryBarriers[0].dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT,
           "ALL_COMMANDS on a non-compute queue does not invent unsupported shader accesses");
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    expect(translated.assign(dependency, true) == VK_SUCCESS &&
               translated.view().pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT &&
               translated.view().pMemoryBarriers[0].srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT,
           "ordinary transfer scope remains ordinary transfer");
    VkMemoryBarrier legacy = init<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    legacy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    legacy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    TranslatedLegacyBarriers old;
    expect(old.assign(VkPipelineStageFlags(VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT), VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &legacy,
                      0, nullptr, 0, nullptr) == VK_SUCCESS &&
               old.view().src_stage_mask == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT) &&
               old.view().memory_barriers[0].srcAccessMask == (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT) &&
               old.view().memory_barriers[0].dstAccessMask == VK_ACCESS_HOST_READ_BIT,
           "legacy serialized release covers actual shader writes and preserves host acquire");
    expect(old.assign(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &legacy, 0, nullptr, 0, nullptr,
                      false) == VK_SUCCESS &&
               old.view().memory_barriers[0].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT,
           "legacy ALL_COMMANDS does not add compute accesses on a non-compute queue");
}
}  // namespace
int main() {
    masks();
    dependencyCopies();
    failuresAndLegacy();
    nativeSidecarScopes();
    serializedBufferScopes();
    std::printf("OMM synchronization translation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
