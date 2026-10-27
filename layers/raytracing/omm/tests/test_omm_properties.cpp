// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_properties.h"
#include <new>
#include <string>
static thread_local int allocation_budget = -1;
void* operator new(std::size_t bytes) {
    if (allocation_budget >= 0 && allocation_budget-- == 0) throw std::bad_alloc();
    if (auto* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
using namespace vel::omm;
namespace {
unsigned assertions = 0, native_prefixes = 0, pools_created = 0, native_gets = 0, native_copies = 0;
PFN_vkCreateQueryPool native_create{};
PFN_vkCmdBindPipeline native_bind{};
PFN_vkGetQueryPoolResults native_get{};
PFN_vkCmdCopyQueryPoolResults native_copy{};
void require(bool value, const char* reason) {
    ++assertions;
    if (!value) {
        std::fprintf(stderr, "FAIL properties: %s\n", reason);
        std::exit(1);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL create_spy(VkDevice device, const VkQueryPoolCreateInfo* info,
                                          const VkAllocationCallbacks* allocator, VkQueryPool* out) {
    require(info->queryType == VK_QUERY_TYPE_TIMESTAMP, "native carrier query type is timestamp, never unsupported MM type");
    ++pools_created;
    return native_create(device, info, allocator, out);
}
VKAPI_ATTR void VKAPI_CALL bind_spy(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    ++native_prefixes;
    native_bind(cb, point, pipeline);
}
VKAPI_ATTR VkResult VKAPI_CALL get_spy(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void* data,
                                       VkDeviceSize stride, VkQueryResultFlags flags) {
    require((flags & (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)) ==
                    (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) &&
                !(flags & VK_QUERY_RESULT_PARTIAL_BIT),
            "host native availability fetch strips timestamp-forbidden partial flag");
    ++native_gets;
    return native_get(device, pool, first, count, size, data, stride, flags);
}
VKAPI_ATTR void VKAPI_CALL copy_spy(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer buffer,
                                    VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags) {
    require((flags & (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)) ==
                    (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) &&
                !(flags & VK_QUERY_RESULT_PARTIAL_BIT),
            "GPU native availability copy strips timestamp-forbidden partial flag");
    ++native_copies;
    native_copy(cb, pool, first, count, buffer, offset, stride, flags);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proc(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCreateQueryPool")) return reinterpret_cast<PFN_vkVoidFunction>(create_spy);
    if (!std::strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(bind_spy);
    if (!std::strcmp(name, "vkGetQueryPoolResults")) return reinterpret_cast<PFN_vkVoidFunction>(get_spy);
    if (!std::strcmp(name, "vkCmdCopyQueryPoolResults")) return reinterpret_cast<PFN_vkVoidFunction>(copy_spy);
    return vkGetDeviceProcAddr(device, name);
}
VKAPI_ATTR void VKAPI_CALL no_timestamps(VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties* properties) {
    vkGetPhysicalDeviceQueueFamilyProperties(physical, count, properties);
    if (properties)
        for (uint32_t i = 0; i < *count; ++i) properties[i].timestampValidBits = 0;
}
std::vector<uint32_t> shader(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file) && file.tellg() > 0 && file.tellg() % 4 == 0, "shader file");
    const size_t bytes = size_t(file.tellg());
    std::vector<uint32_t> words(bytes / 4);
    file.seekg(0);
    require(bool(file.read(reinterpret_cast<char*>(words.data()), bytes)), "shader read");
    return words;
}
void track(DeviceBackend* mm, const Buffer& value, uint64_t size, VkBufferUsageFlags usage) {
    auto info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = size;
    info.usage = usage;
    CHECK(device_track_buffer(mm, value.handle, &info, size));
    device_buffer_bound(mm, value.handle);
}
void release(Fixture& f, DeviceBackend* mm, Buffer buffer) {
    device_untrack_buffer(mm, buffer.handle);
    auto found =
        std::find_if(f.buffers.begin(), f.buffers.end(), [&](const Buffer& value) { return value.handle == buffer.handle; });
    require(found != f.buffers.end(), "source buffer tracked in fixture");
    if (buffer.mapped) vkUnmapMemory(f.device, buffer.memory);
    vkDestroyBuffer(f.device, buffer.handle, nullptr);
    vkFreeMemory(f.device, buffer.memory, nullptr);
    f.buffers.erase(found);
}
uint64_t read64(const void* source, size_t offset) {
    uint64_t value;
    std::memcpy(&value, static_cast<const uint8_t*>(source) + offset, 8);
    return value;
}
uint32_t read32(const void* source, size_t offset) {
    uint32_t value;
    std::memcpy(&value, static_cast<const uint8_t*>(source) + offset, 4);
    return value;
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    Fixture f;
    if (!f.open()) return 77;
    const std::string directory = argv[1];
    const auto build = shader(directory + "/omm_build.comp.spv"), copy = shader(directory + "/omm_copy.comp.spv"),
               property = shader(directory + "/omm_property.comp.spv");
    native_create = reinterpret_cast<PFN_vkCreateQueryPool>(vkGetDeviceProcAddr(f.device, "vkCreateQueryPool"));
    native_bind = reinterpret_cast<PFN_vkCmdBindPipeline>(vkGetDeviceProcAddr(f.device, "vkCmdBindPipeline"));
    native_get = reinterpret_cast<PFN_vkGetQueryPoolResults>(vkGetDeviceProcAddr(f.device, "vkGetQueryPoolResults"));
    native_copy = reinterpret_cast<PFN_vkCmdCopyQueryPoolResults>(vkGetDeviceProcAddr(f.device, "vkCmdCopyQueryPoolResults"));
    DeviceBackend* mm{};
    CHECK(device_backend_create(
        {f.physical, f.device, vkGetDeviceProcAddr, nullptr, build.data(), build.size() * 4, copy.data(), copy.size() * 4}, &mm));
    PropertyBackendCreateInfo create{f.physical,
                                     f.device,
                                     proc,
                                     mm,
                                     nullptr,
                                     property.data(),
                                     property.size() * 4,
                                     vkGetPhysicalDeviceMemoryProperties,
                                     vkGetPhysicalDeviceQueueFamilyProperties};
    PropertyBackend* backend{};
    CHECK(properties_create(create, &backend));
    VkQueryPool compact{}, serial{};
    auto pool_info = init<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
    pool_info.queryCount = 4;
    pool_info.queryType = VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT;
    CHECK(properties_create_pool(backend, &pool_info, nullptr, &compact));
    pool_info.queryCount = 2;
    pool_info.queryType = VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT;
    CHECK(properties_create_pool(backend, &pool_info, nullptr, &serial));
    require(properties_is_pool(backend, compact) && properties_is_pool(backend, serial) && pools_created == 2,
            "logical MM pools retain native timestamp handles");
    auto command = f.begin();
    std::vector<PropertyRecordingRef> recordings(2);
    CHECK(properties_cmd_reset(backend, command, f.family, compact, 0, 4, &recordings[0]));
    CHECK(properties_cmd_reset(backend, command, f.family, serial, 0, 2, &recordings[1]));
    f.submit(command);
    recordings.clear();
    uint64_t result64[2] = {0xccccccccccccccccull, 0xccccccccccccccccull};
    require(properties_get_results(backend, compact, 0, 1, sizeof(result64), result64, 16,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY &&
                result64[0] == 0xccccccccccccccccull && result64[1] == 0,
            "unavailable host query preserves value and writes zero availability");
    uint8_t partial[32];
    std::memset(partial, 0xcc, sizeof(partial));
    require(properties_get_results(backend, compact, 0, 2, sizeof(partial), partial, 12,
                                   VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT) == VK_NOT_READY &&
                read32(partial, 0) == 0 && read32(partial, 4) == 0 && read32(partial, 12) == 0 && read32(partial, 16) == 0 &&
                read32(partial, 8) == 0xccccccccu,
            "32-bit partial host results use safe zero without reading unavailable GPU values and preserve stride padding");
    const auto gets_before_oom = native_gets;
    result64[0] = 0xccccccccccccccccull;
    allocation_budget = 0;
    const auto host_oom = properties_get_results(backend, compact, 0, 1, sizeof(result64), result64, 16, VK_QUERY_RESULT_64_BIT);
    allocation_budget = -1;
    require(host_oom == VK_ERROR_OUT_OF_HOST_MEMORY && native_gets == gets_before_oom && result64[0] == 0xccccccccccccccccull,
            "host availability-array OOM preserves output and performs no native query wait");
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto triangles = f.buffer(8, usage, false), data = f.buffer(4, usage, false), scratch = f.buffer(512, usage, false);
    track(mm, triangles, 8, usage);
    track(mm, data, 4, usage);
    track(mm, scratch, 512, usage);
    auto mm_info = init<VkMicromapBuildInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT);
    mm_info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    mm_info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    mm_info.flags = VK_BUILD_MICROMAP_ALLOW_COMPACTION_BIT_EXT;
    VkMicromapUsageEXT mm_usage{1, 0, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT};
    mm_info.usageCountsCount = 1;
    mm_info.pUsageCounts = &mm_usage;
    auto sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
    CHECK(device_get_build_sizes(mm, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mm_info, &sizes));
    const uint64_t active = sizes.micromapSize, capacity = active + 64;
    auto storage = f.buffer(capacity, usage, false);
    track(mm, storage, capacity, usage | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT);
    auto mm_create = init<VkMicromapCreateInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT);
    mm_create.buffer = storage.handle;
    mm_create.size = capacity;
    mm_create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    VkMicromapEXT micromap{};
    CHECK(device_create_micromap(mm, &mm_create, nullptr, &micromap));
    // Record query writes before recording/executing the actual MM build. Only
    // GPU execution order, never host last-build bookkeeping, supplies the size.
    auto query_command = f.begin();
    recordings.resize(2);
    const auto write_prefix = native_prefixes;
    for (int budget : {0, 1}) {
        allocation_budget = budget;
        const auto result = properties_cmd_write(backend, query_command, f.family, 1, &micromap,
                                                 VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT, compact, 1, &recordings[0]);
        allocation_budget = -1;
        require(result == VK_ERROR_OUT_OF_HOST_MEMORY && native_prefixes == write_prefix,
                "property-write token/source-plan OOM records no native prefix");
    }
    CHECK(properties_cmd_write(backend, query_command, f.family, 1, &micromap, VK_QUERY_TYPE_MICROMAP_COMPACTED_SIZE_EXT, compact,
                               1, &recordings[0]));
    CHECK(properties_cmd_write(backend, query_command, f.family, 1, &micromap, VK_QUERY_TYPE_MICROMAP_SERIALIZATION_SIZE_EXT,
                               serial, 0, &recordings[1]));
    command = f.begin();
    const uint32_t triangle_record[] = {0, 0x00020000}, states = 3;
    vkCmdUpdateBuffer(command, triangles.handle, 0, 8, triangle_record);
    vkCmdUpdateBuffer(command, data.handle, 0, 4, &states);
    Fixture::barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    mm_info.dstMicromap = micromap;
    mm_info.triangleArray.deviceAddress = triangles.address;
    mm_info.triangleArrayStride = 8;
    mm_info.data.deviceAddress = data.address;
    mm_info.scratchData.deviceAddress = (scratch.address + 255) & ~uint64_t(255);
    CHECK(device_cmd_build(mm, command, 1, &mm_info));
    f.submit(command);
    f.submit(query_command);
    recordings.clear();
    require(properties_get_results(backend, compact, 1, 1, sizeof(result64), result64, 16,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                    VK_SUCCESS &&
                result64[0] == active && result64[0] != capacity && result64[1] != 0,
            "GPU compacted size reads actual active header bytes rather than creation capacity or host recording order");
    require(properties_get_results(backend, serial, 0, 1, sizeof(result64), result64, 16,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                    VK_SUCCESS &&
                result64[0] == active + 2 * VK_UUID_SIZE && result64[1] != 0,
            "serialization query includes exactly public two-UUID header plus pointer-free canonical blob");
    uint8_t mixed[72];
    std::memset(mixed, 0xcc, sizeof(mixed));
    require(properties_get_results(backend, compact, 0, 3, sizeof(mixed), mixed, 24,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY &&
                read64(mixed, 0) == 0xccccccccccccccccull && read64(mixed, 8) == 0 && read64(mixed, 24) == active &&
                read64(mixed, 32) != 0 && read64(mixed, 48) == 0xccccccccccccccccull && read64(mixed, 56) == 0 &&
                read64(mixed, 40) == 0xccccccccccccccccull,
            "mixed availability reads only completed values and preserves unavailable values plus all stride padding");
    device_destroy_micromap(mm, micromap, nullptr);
    release(f, mm, storage);
    release(f, mm, triangles);
    release(f, mm, data);
    release(f, mm, scratch);
    require(properties_get_results(backend, compact, 1, 1, sizeof(result64), result64, 16, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                result64[0] == active,
            "query result survives micromap and input-buffer destruction");
    auto destination = f.buffer(128, usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    track(mm, destination, 128, usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memset(destination.mapped, 0xcc, 128);
    command = f.begin();
    recordings.resize(4);
    CHECK(properties_cmd_copy_results(backend, command, f.family, compact, 1, 1, destination.handle, 8, 24,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT,
                                      &recordings[0]));
    CHECK(properties_cmd_copy_results(backend, command, f.family, serial, 0, 1, destination.handle, 32, 8, VK_QUERY_RESULT_WAIT_BIT,
                                      &recordings[1]));
    CHECK(properties_cmd_copy_results(backend, command, f.family, compact, 0, 3, destination.handle, 48, 12,
                                      VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT, &recordings[2]));
    CHECK(properties_cmd_copy_results(backend, command, f.family, compact, 0, 1, destination.handle, 96, 16,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, &recordings[3]));
    Fixture::barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    f.submit(command);
    recordings.clear();
    require(read64(destination.mapped, 8) == active && read64(destination.mapped, 16) == 1 &&
                read32(destination.mapped, 32) == active + 32,
            "GPU copy writes 64-bit available compacted value and 32-bit serialization value after source destruction");
    require(read32(destination.mapped, 48) == 0 && read32(destination.mapped, 52) == 0 &&
                read32(destination.mapped, 60) == active && read32(destination.mapped, 64) == 1 &&
                read32(destination.mapped, 72) == 0 && read32(destination.mapped, 76) == 0 && read64(destination.mapped, 104) == 0,
            "GPU copy formats mixed availability and partial values without unsupported native timestamp partial flag");
    require(read64(destination.mapped, 0) == 0xccccccccccccccccull && read64(destination.mapped, 24) == 0xccccccccccccccccull &&
                read32(destination.mapped, 56) == 0xccccccccu && read32(destination.mapped, 68) == 0xccccccccu &&
                read64(destination.mapped, 112) == 0xccccccccccccccccull,
            "native final copy preserves all destination offsets, stride gaps and tail canaries");
    command = f.begin();
    PropertyRecordingRef ignored;
    const auto prefix = native_prefixes;
    for (int budget : {0, 1, 2, 3}) {
        allocation_budget = budget;
        const auto result = properties_cmd_copy_results(backend, command, f.family, compact, 1, 1, destination.handle, 0, 16,
                                                        VK_QUERY_RESULT_64_BIT, &ignored);
        allocation_budget = -1;
        require(result == VK_ERROR_OUT_OF_HOST_MEMORY && native_prefixes == prefix,
                "query-copy allocation failure records no native command prefix");
    }
    auto no_timestamp_create = create;
    no_timestamp_create.get_queue_family_properties = no_timestamps;
    PropertyBackend* unsupported{};
    CHECK(properties_create(no_timestamp_create, &unsupported));
    require(properties_cmd_reset(unsupported, command, f.family, compact, 0, 1, &ignored) == VK_ERROR_FEATURE_NOT_PRESENT &&
                native_prefixes == prefix,
            "timestamp-less queue family rejects emulation before recording, leaving hardware fallback explicitly open");
    properties_destroy(unsupported);
    CHECK(properties_cmd_reset(backend, command, f.family, compact, 1, 1, &ignored));
    f.submit(command);
    ignored.reset();
    result64[0] = 0xccccccccccccccccull;
    require(properties_get_results(backend, compact, 1, 1, sizeof(result64), result64, 16,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY &&
                result64[0] == 0xccccccccccccccccull && result64[1] == 0,
            "GPU reset clears availability without reading or returning stale private value");
    require(native_gets >= 7 && native_copies == 4, "host and GPU copies delegate real native query availability");
    properties_destroy_pool(backend, compact, nullptr);
    properties_destroy_pool(backend, serial, nullptr);
    require(!properties_is_pool(backend, compact), "destroy removes logical property-pool tracking");
    properties_destroy(backend);
    device_backend_destroy(mm);
    std::printf("PASS GPU micromap property queries: %u assertions, compact=%llu serialized=%llu\n", assertions,
                static_cast<unsigned long long>(active), static_cast<unsigned long long>(active + 32));
}
