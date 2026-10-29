// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#define main omm_serialization_fixture_main
#include "test_omm_gpu_build.cpp"
#undef main
#include "../omm_device.h"

namespace {
unsigned assertions = 0, binds = 0, properties_calls = 0, unbound_queries = 0;
VkBuffer unbound_buffer{};
VkPhysicalDevice expected_physical{};
uint8_t driver_uuid[VK_UUID_SIZE]{};
PFN_vkCmdBindPipeline native_bind{};
PFN_vkGetBufferDeviceAddress native_address{};
void expect(bool condition, const char* reason) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL OMM serialization: %s\n", reason);
        std::exit(1);
    }
}
VKAPI_ATTR void VKAPI_CALL spy_bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    ++binds;
    native_bind(command, point, pipeline);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL spy_address(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    if (info->buffer == unbound_buffer) ++unbound_queries;
    return native_address(device, info);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL downstream(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(spy_bind);
    if (!std::strcmp(name, "vkGetBufferDeviceAddress")) return reinterpret_cast<PFN_vkVoidFunction>(spy_address);
    return vkGetDeviceProcAddr(device, name);
}
VKAPI_ATTR void VKAPI_CALL downstream_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    ++properties_calls;
    expect(physical == expected_physical, "downstream callback receives the correct physical handle");
    vkGetPhysicalDeviceProperties2(physical, properties);
    const auto* identifiers = reinterpret_cast<const VkPhysicalDeviceIDProperties*>(properties->pNext);
    expect(identifiers && identifiers->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
           "driver identity queried through supplied downstream callback");
    std::memcpy(driver_uuid, identifiers->driverUUID, VK_UUID_SIZE);
}
std::vector<uint32_t> words(const char* path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    expect(bool(stream) && stream.tellg() > 0 && stream.tellg() % 4 == 0, "SPIR-V file");
    std::vector<uint32_t> result(size_t(stream.tellg()) / 4);
    stream.seekg(0);
    expect(bool(stream.read(reinterpret_cast<char*>(result.data()), result.size() * 4)), "SPIR-V read");
    return result;
}
void track(DeviceBackend* backend, const Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool bound = true) {
    auto info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = size;
    info.usage = usage;
    CHECK(device_track_buffer(backend, buffer.handle, &info));
    if (bound) device_buffer_bound(backend, buffer.handle);
}
void reusable_submit(Fixture& fixture, VkCommandBuffer command) {
    auto info = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    info.commandBufferCount = 1;
    info.pCommandBuffers = &command;
    auto create = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence{};
    CHECK(vkCreateFence(fixture.device, &create, nullptr, &fence));
    CHECK(vkQueueSubmit(fixture.queue, 1, &info, fence));
    CHECK(vkWaitForFences(fixture.device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(fixture.device, fence, nullptr);
}
void compute_dependency(VkCommandBuffer command) {
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
void fills_to_compute(VkCommandBuffer command) {
    Fixture::barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
void to_host(VkCommandBuffer command) {
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
}
void canaries(const Buffer& buffer, size_t size, size_t offset, size_t active) {
    const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
    expect(std::all_of(bytes, bytes + offset, [](uint8_t value) { return value == 0xa5; }), "untouched backing prefix");
    expect(std::all_of(bytes + offset + active, bytes + size, [](uint8_t value) { return value == 0xa5; }),
           "only the actual GPU active span is written");
}
VkBuffer overlapping_alias(Fixture& fixture, const Buffer& backing, VkDeviceSize size, VkBufferUsageFlags usage) {
    auto create = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    create.size = size;
    create.usage = usage;
    VkBuffer alias{};
    CHECK(vkCreateBuffer(fixture.device, &create, nullptr, &alias));
    VkMemoryRequirements backing_requirements, alias_requirements;
    vkGetBufferMemoryRequirements(fixture.device, backing.handle, &backing_requirements);
    vkGetBufferMemoryRequirements(fixture.device, alias, &alias_requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(fixture.physical, &properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((backing_requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            memory_type = i;  // Same selection as Fixture::buffer(host=true).
            break;
        }
    }
    if (alias_requirements.size > backing_requirements.size || memory_type == UINT32_MAX ||
        !(alias_requirements.memoryTypeBits & (1u << memory_type))) {
        std::puts("Native alias has incompatible memory requirements; overlapping-address case skipped");
        vkDestroyBuffer(fixture.device, alias, nullptr);
        return VK_NULL_HANDLE;
    }
    CHECK(vkBindBufferMemory(fixture.device, alias, backing.memory, 0));
    auto address = init<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    address.buffer = alias;
    if (vkGetBufferDeviceAddress(fixture.device, &address) != backing.address) {
        // Vulkan permits distinct address ranges even for aliased memory. The
        // overlap-specific regression is only applicable when addresses match.
        std::puts("Native same-memory aliases expose distinct addresses; overlapping-address case skipped");
        vkDestroyBuffer(fixture.device, alias, nullptr);
        return VK_NULL_HANDLE;
    }
    std::puts("Native same-memory large/small aliases expose the same device address");
    return alias;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr, "usage: test_omm_serialization build.spv copy.spv serialize.spv generator.spv [max-level]\n");
        return 1;
    }
    Fixture fixture;
    if (!fixture.open()) return 77;
    expected_physical = fixture.physical;
    native_bind = reinterpret_cast<PFN_vkCmdBindPipeline>(vkGetDeviceProcAddr(fixture.device, "vkCmdBindPipeline"));
    native_address =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(fixture.device, "vkGetBufferDeviceAddress"));
    auto build_code = words(argv[1]), copy_code = words(argv[2]), serialize_code = words(argv[3]);
    DeviceBackendCreateInfo backend_info{fixture.physical,  fixture.device,        downstream,       nullptr,
                                         build_code.data(), build_code.size() * 4, copy_code.data(), copy_code.size() * 4};
    backend_info.serialize_code = serialize_code.data();
    backend_info.serialize_code_size = serialize_code.size() * 4;
    DeviceBackend* backend = nullptr;
    expect(device_backend_create(backend_info, &backend) == VK_ERROR_INITIALIZATION_FAILED && !backend,
           "serialization cannot fabricate a global physical dispatch callback");
    backend_info.get_properties2 = downstream_properties;
    CHECK(device_backend_create(backend_info, &backend));
    expect(properties_calls == 1, "native driverUUID captured once at device construction");

    constexpr VkBufferUsageFlags native_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkBufferUsageFlags map_usage = native_usage | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT;
    constexpr uint32_t count = 17, stride = 13, seed = 1;
    const uint32_t max_level = argc == 6 ? uint32_t(std::strtoul(argv[5], nullptr, 10)) : 6;
    expect(max_level <= 12, "supported maximum subdivision");
    const uint64_t data_size = ((uint64_t(1) << (max_level * 2)) * 2 + 7) / 8 + 17;
    const uint64_t data_accessible = align4(data_size);
    constexpr uint64_t triangles_size = (count - 1) * stride + 8, triangles_accessible = (triangles_size + 3) & ~uint64_t(3);
    std::vector<uint8_t> records(triangles_size, 0xcd), data(data_size);
    std::vector<VkMicromapUsageEXT> usages;
    for (uint32_t i = 0; i < data_size; ++i) data[i] = uint8_t((i * 37u + seed * 19u + 0xa7u) ^ (i >> 3));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t level = (i + seed) % (max_level + 1), format = 1 + ((i ^ seed) & 1);
        usages.push_back({1, level, format});
        put32(records, i * stride, (i % 3) * 3 + 7);
        records[i * stride + 4] = uint8_t(level);
        records[i * stride + 5] = 0;
        records[i * stride + 6] = uint8_t(format);
        records[i * stride + 7] = 0;
    }
    std::vector<uint8_t> expected;
    expect(storage_build({records.data(), records.size(), count, stride, data.data(), data.size()}, &expected) ==
               StorageResult::Success,
           "independent canonical storage oracle");
    const VkDeviceSize capacity = expected.size() + 256, map_bytes = 256 + capacity + 32;
    const VkDeviceSize serialized_bytes = 512 + 32 + expected.size() + 32;
    auto triangles = fixture.buffer(triangles_accessible, native_usage, false);
    auto states = fixture.buffer(data_accessible, native_usage, false);
    auto scratch = fixture.buffer(512, native_usage, false);
    auto source = fixture.buffer(map_bytes, native_usage, false);
    auto restored = fixture.buffer(map_bytes, native_usage, true);
    auto replay_destination = fixture.buffer(map_bytes, native_usage, true);
    auto serialized = fixture.buffer(serialized_bytes, native_usage, true);
    const VkDeviceSize serialized_offset = ((serialized.address + 255) & ~uint64_t(255)) - serialized.address + 256;
    const VkDeviceAddress serialized_address = serialized.address + serialized_offset;
    track(backend, triangles, triangles_accessible, native_usage);
    track(backend, states, data_accessible, native_usage);
    track(backend, scratch, 512, native_usage);
    track(backend, source, map_bytes, map_usage);
    track(backend, restored, map_bytes, map_usage);
    track(backend, replay_destination, map_bytes, map_usage);
    track(backend, serialized, serialized_bytes, native_usage);
    // A later tracked short alias must not truncate the valid large buffer's
    // range. Keep it live through both actual GPU serialization directions.
    const VkDeviceSize small_alias_size = serialized_offset + 96;
    const auto small_alias = overlapping_alias(fixture, serialized, small_alias_size, native_usage);
    if (small_alias) {
        auto alias_info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        alias_info.size = small_alias_size;
        alias_info.usage = native_usage;
        CHECK(device_track_buffer(backend, small_alias, &alias_info));
        device_buffer_bound(backend, small_alias);
        VkDeviceSize logical = 0, accessible = 0;
        CHECK(device_address_span(backend, serialized_address, &logical, &accessible));
        expect(logical == serialized_bytes - serialized_offset && accessible == logical,
               "larger valid BDA alias supplies both logical and accessible span despite newer short alias");
        CHECK(device_address_span(backend, serialized_address + 16, &logical, &accessible));
        expect(logical == serialized_bytes - serialized_offset - 16 && accessible == logical,
               "largest alias range selection preserves a nonzero queried byte offset");
    }
    auto create = init<VkMicromapCreateInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT);
    create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    create.buffer = source.handle;
    create.offset = 256;
    create.size = capacity;
    VkMicromapEXT source_map{}, restored_map{}, replay_map{};
    CHECK(device_create_micromap(backend, &create, nullptr, &source_map));
    create.buffer = restored.handle;
    CHECK(device_create_micromap(backend, &create, nullptr, &restored_map));
    create.buffer = replay_destination.handle;
    CHECK(device_create_micromap(backend, &create, nullptr, &replay_map));

    auto unbound_info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    unbound_info.size = 96;
    unbound_info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    CHECK(vkCreateBuffer(fixture.device, &unbound_info, nullptr, &unbound_buffer));
    CHECK(device_track_buffer(backend, unbound_buffer, &unbound_info));
    expect(device_buffer_bound_span(backend, unbound_buffer, 0, 96) == VK_ERROR_VALIDATION_FAILED_EXT,
           "AS storage check rejects unbound buffer without querying BDA");
    expect(device_buffer_bound_span(backend, source.handle, 256, capacity) == VK_SUCCESS &&
               device_buffer_bound_span(backend, source.handle, map_bytes, 0) == VK_SUCCESS,
           "bound logical spans support full range and an empty end range");
    expect(device_buffer_bound_span(backend, source.handle, map_bytes - 1, 2) == VK_ERROR_VALIDATION_FAILED_EXT &&
               device_buffer_bound_span(backend, source.handle, UINT64_MAX, UINT64_MAX) == VK_ERROR_VALIDATION_FAILED_EXT &&
               device_buffer_bound_span(backend, VK_NULL_HANDLE, 0, 1) == VK_ERROR_VALIDATION_FAILED_EXT,
           "range overflow and unknown buffers fail closed");

    auto serialize = init<VkCopyMicromapToMemoryInfoEXT>(VK_STRUCTURE_TYPE_COPY_MICROMAP_TO_MEMORY_INFO_EXT);
    serialize.src = source_map;
    serialize.dst.deviceAddress = serialized_address;
    serialize.mode = VK_COPY_MICROMAP_MODE_SERIALIZE_EXT;
    auto deserialize = init<VkCopyMemoryToMicromapInfoEXT>(VK_STRUCTURE_TYPE_COPY_MEMORY_TO_MICROMAP_INFO_EXT);
    deserialize.src.deviceAddress = serialized_address;
    deserialize.dst = restored_map;
    deserialize.mode = VK_COPY_MICROMAP_MODE_DESERIALIZE_EXT;
    auto serialize_command = fixture.begin();
    const unsigned prefix = binds;
    auto invalid_serialize = serialize;
    invalid_serialize.dst.deviceAddress++;
    expect(device_cmd_serialize(backend, serialize_command, &invalid_serialize) == VK_ERROR_VALIDATION_FAILED_EXT,
           "serialized destination requires 256-byte alignment");
    invalid_serialize = serialize;
    invalid_serialize.src = VK_NULL_HANDLE;
    expect(device_cmd_serialize(backend, serialize_command, &invalid_serialize) == VK_ERROR_VALIDATION_FAILED_EXT,
           "unknown source micromap rejected before native prefix");
    invalid_serialize = serialize;
    invalid_serialize.mode = VK_COPY_MICROMAP_MODE_CLONE_EXT;
    expect(device_cmd_serialize(backend, serialize_command, &invalid_serialize) == VK_ERROR_FEATURE_NOT_PRESENT,
           "wrong public serialization mode rejected");
    invalid_serialize = serialize;
    invalid_serialize.dst.deviceAddress = 256;
    expect(device_cmd_serialize(backend, serialize_command, &invalid_serialize) == VK_ERROR_VALIDATION_FAILED_EXT,
           "unknown serialized BDA rejected");
    auto invalid_deserialize = deserialize;
    invalid_deserialize.src.deviceAddress += 4;
    expect(device_cmd_deserialize(backend, serialize_command, &invalid_deserialize) == VK_ERROR_VALIDATION_FAILED_EXT,
           "serialized source requires 256-byte alignment");
    invalid_deserialize = deserialize;
    invalid_deserialize.dst = VK_NULL_HANDLE;
    expect(device_cmd_deserialize(backend, serialize_command, &invalid_deserialize) == VK_ERROR_VALIDATION_FAILED_EXT,
           "unknown restore destination rejected");
    expect(binds == prefix && unbound_queries == 0, "all static failures precede native commands and unbound BDA queries");

    // Both copies are recorded before any source build. No CPU-side build
    // history or mapped input content can determine their actual copy length.
    vkCmdFillBuffer(serialize_command, serialized.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    fills_to_compute(serialize_command);
    CHECK(device_cmd_serialize(backend, serialize_command, &serialize));
    to_host(serialize_command);
    Fixture::barrier(serialize_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(serialize_command, source.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    CHECK(vkEndCommandBuffer(serialize_command));
    auto deserialize_command = fixture.begin();
    vkCmdFillBuffer(deserialize_command, restored.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    fills_to_compute(deserialize_command);
    compute_dependency(deserialize_command);
    CHECK(device_cmd_deserialize(backend, deserialize_command, &deserialize));
    to_host(deserialize_command);
    CHECK(vkEndCommandBuffer(deserialize_command));
    Program generate(fixture, argv[4], sizeof(GeneratePush));
    auto build = init<VkMicromapBuildInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT);
    build.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    build.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    build.usageCountsCount = count;
    build.pUsageCounts = usages.data();
    build.dstMicromap = source_map;
    build.triangleArrayStride = stride;
    build.triangleArray.deviceAddress = triangles.address;
    build.data.deviceAddress = states.address;
    build.scratchData.deviceAddress = (scratch.address + 255) & ~uint64_t(255);
    auto build_command = fixture.begin();
    GeneratePush generator{triangles.address,    states.address,  count,    stride, seed,
                           triangles_accessible, data_accessible, max_level};
    generate.dispatch(build_command, &generator, sizeof(generator), 128);
    compute_dependency(build_command);
    CHECK(device_cmd_build(backend, build_command, 1, &build));
    compute_dependency(build_command);
    Fixture::barrier(build_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(build_command, triangles.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    vkCmdFillBuffer(build_command, states.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    fixture.submit(build_command);
    device_untrack_buffer(backend, triangles.handle);
    device_untrack_buffer(backend, states.handle);
    release_buffer(fixture, triangles);
    release_buffer(fixture, states);
    reusable_submit(fixture, serialize_command);
    vkFreeCommandBuffers(fixture.device, fixture.pool, 1, &serialize_command);
    device_destroy_micromap(backend, source_map, nullptr);
    device_untrack_buffer(backend, source.handle);
    release_buffer(fixture, source);
    reusable_submit(fixture, deserialize_command);
    vkFreeCommandBuffers(fixture.device, fixture.pool, 1, &deserialize_command);
    const auto* serialized_data = static_cast<const uint8_t*>(serialized.mapped) + serialized_offset;
    expect(!std::memcmp(serialized_data, driver_uuid, VK_UUID_SIZE), "public header contains the actual downstream driverUUID");
    expect(!std::memcmp(serialized_data + 32, expected.data(), expected.size()), "public envelope contains canonical active bytes");
    expect(!std::memcmp(static_cast<uint8_t*>(restored.mapped) + 256, expected.data(), expected.size()) &&
               storage_validate(static_cast<uint8_t*>(restored.mapped) + 256, expected.size()) == StorageResult::Success,
           "GPU restore survives input overwrite, original MM destruction, and original storage destruction");
    canaries(serialized, serialized_bytes, serialized_offset, 32 + expected.size());
    canaries(restored, map_bytes, 256, expected.size());
    auto version = init<VkMicromapVersionInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_VERSION_INFO_EXT);
    version.pVersionData = serialized_data;
    VkAccelerationStructureCompatibilityKHR compatible{};
    device_get_compatibility(backend, &version, &compatible);
    expect(compatible == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR, "serialized public header is compatible");
    uint8_t changed_header[32];
    std::memcpy(changed_header, serialized_data, 32);
    version.pVersionData = changed_header;
    for (uint32_t i = 0; i < 32; ++i) {
        changed_header[i] ^= 1;
        device_get_compatibility(backend, &version, &compatible);
        expect(compatible == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR,
               "every byte of both public UUIDs participates in compatibility");
        changed_header[i] ^= 1;
    }

    // An unknown/malformed envelope is rejected on the device before any
    // destination write. These inputs never need a host read in the backend.
    auto damaged = fixture.buffer(serialized_bytes, native_usage, false);
    const VkDeviceSize damaged_offset = ((damaged.address + 255) & ~uint64_t(255)) - damaged.address + 256;
    track(backend, damaged, serialized_bytes, native_usage);
    auto small_create = create;
    small_create.size = 64;
    VkMicromapEXT small_map{};
    CHECK(device_create_micromap(backend, &small_create, nullptr, &small_map));
    auto short_input = fixture.buffer(256, native_usage, false);
    track(backend, short_input, 95, native_usage);
    auto invalid_command = fixture.begin();
    const unsigned invalid_prefix = binds;
    auto short_serialize = serialize;
    short_serialize.src = restored_map;
    short_serialize.dst.deviceAddress = short_input.address;
    auto short_deserialize = deserialize;
    short_deserialize.src.deviceAddress = short_input.address;
    expect((short_input.address & 255) == 0, "short-span test has an aligned native base");
    expect(device_cmd_serialize(backend, invalid_command, &short_serialize) == VK_ERROR_VALIDATION_FAILED_EXT &&
               device_cmd_deserialize(backend, invalid_command, &short_deserialize) == VK_ERROR_VALIDATION_FAILED_EXT &&
               binds == invalid_prefix,
           "a padded native allocation does not extend the known logical serialized span");
    fixture.submit(invalid_command);
    device_untrack_buffer(backend, short_input.handle);
    release_buffer(fixture, short_input);

    auto truncated = fixture.buffer(512, native_usage, true);
    const VkDeviceSize truncated_offset = ((truncated.address + 255) & ~uint64_t(255)) - truncated.address;
    track(backend, truncated, truncated_offset + 96, native_usage);
    auto truncated_command = fixture.begin();
    vkCmdFillBuffer(truncated_command, truncated.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    fills_to_compute(truncated_command);
    auto truncated_serialize = serialize;
    truncated_serialize.src = restored_map;
    truncated_serialize.dst.deviceAddress = truncated.address + truncated_offset;
    CHECK(device_cmd_serialize(backend, truncated_command, &truncated_serialize));
    to_host(truncated_command);
    fixture.submit(truncated_command);
    expect(std::all_of(static_cast<uint8_t*>(truncated.mapped), static_cast<uint8_t*>(truncated.mapped) + 512,
                       [](uint8_t value) { return value == 0xa5; }),
           "GPU active size cannot overrun a minimal known serialization destination span");
    truncated_command = fixture.begin();
    vkCmdFillBuffer(truncated_command, replay_destination.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    VkBufferCopy truncated_copy{serialized_offset, truncated_offset, 96};
    vkCmdCopyBuffer(truncated_command, serialized.handle, truncated.handle, 1, &truncated_copy);
    fills_to_compute(truncated_command);
    auto truncated_deserialize = deserialize;
    truncated_deserialize.src.deviceAddress = truncated.address + truncated_offset;
    truncated_deserialize.dst = replay_map;
    CHECK(device_cmd_deserialize(backend, truncated_command, &truncated_deserialize));
    to_host(truncated_command);
    fixture.submit(truncated_command);
    expect(std::all_of(static_cast<uint8_t*>(replay_destination.mapped),
                       static_cast<uint8_t*>(replay_destination.mapped) + map_bytes, [](uint8_t value) { return value == 0xa5; }),
           "GPU header active size cannot extend a truncated known serialized source span");
    device_untrack_buffer(backend, truncated.handle);
    release_buffer(fixture, truncated);
    constexpr uint32_t patch_offsets[] = {0, 16, 32, 48, 56, UINT32_MAX};
    for (uint32_t damage : patch_offsets) {
        auto invalid_gpu = fixture.begin();
        Fixture::barrier(invalid_gpu, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(invalid_gpu, replay_destination.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
        VkBufferCopy copy{serialized_offset, damaged_offset, 32 + expected.size()};
        vkCmdCopyBuffer(invalid_gpu, serialized.handle, damaged.handle, 1, &copy);
        if (damage != UINT32_MAX) {
            Fixture::barrier(invalid_gpu, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            const uint32_t replacement = UINT32_MAX;
            vkCmdUpdateBuffer(invalid_gpu, damaged.handle, damaged_offset + damage, 4, &replacement);
        }
        fills_to_compute(invalid_gpu);
        auto rejected_restore = deserialize;
        rejected_restore.src.deviceAddress = damaged.address + damaged_offset;
        rejected_restore.dst = damage == UINT32_MAX ? small_map : replay_map;
        CHECK(device_cmd_deserialize(backend, invalid_gpu, &rejected_restore));
        to_host(invalid_gpu);
        fixture.submit(invalid_gpu);
        expect(
            std::all_of(static_cast<uint8_t*>(replay_destination.mapped),
                        static_cast<uint8_t*>(replay_destination.mapped) + map_bytes, [](uint8_t value) { return value == 0xa5; }),
            "GPU rejects incompatible UUID, malformed header/count/active length, or a too-small destination atomically");
    }
    device_destroy_micromap(backend, small_map, nullptr);
    device_untrack_buffer(backend, damaged.handle);
    release_buffer(fixture, damaged);

    // Replay one unchanged serialize+restore CB after a different active build.
    auto replay = fixture.begin();
    vkCmdFillBuffer(replay, serialized.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    vkCmdFillBuffer(replay, replay_destination.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    fills_to_compute(replay);
    serialize.src = restored_map;
    CHECK(device_cmd_serialize(backend, replay, &serialize));
    compute_dependency(replay);
    deserialize.dst = replay_map;
    CHECK(device_cmd_deserialize(backend, replay, &deserialize));
    to_host(replay);
    CHECK(vkEndCommandBuffer(replay));
    reusable_submit(fixture, replay);
    expect(!std::memcmp(static_cast<uint8_t*>(replay_destination.mapped) + 256, expected.data(), expected.size()),
           "reusable public roundtrip initially preserves mixed two/four-state micromaps");
    build.dstMicromap = restored_map;
    build.usageCountsCount = 0;
    build.pUsageCounts = nullptr;
    build.triangleArrayStride = 0;
    build.triangleArray.deviceAddress = build.data.deviceAddress = 0;
    auto empty = fixture.begin();
    CHECK(device_cmd_build(backend, empty, 1, &build));
    compute_dependency(empty);
    fixture.submit(empty);
    reusable_submit(fixture, replay);
    expect(storage_validate(static_cast<uint8_t*>(replay_destination.mapped) + 256, 64) == StorageResult::Success,
           "unchanged command replay reads the new GPU active size after empty rebuild");
    canaries(serialized, serialized_bytes, serialized_offset, 96);
    canaries(replay_destination, map_bytes, 256, 64);
    vkFreeCommandBuffers(fixture.device, fixture.pool, 1, &replay);
    device_destroy_micromap(backend, restored_map, nullptr);
    device_destroy_micromap(backend, replay_map, nullptr);
    device_untrack_buffer(backend, unbound_buffer);
    vkDestroyBuffer(fixture.device, unbound_buffer, nullptr);
    expect(unbound_queries == 0, "no native address request was issued for unbound storage");
    if (small_alias) {
        device_untrack_buffer(backend, small_alias);
        vkDestroyBuffer(fixture.device, small_alias, nullptr);
    }
    device_backend_destroy(backend);
    std::printf("OMM public device serialization: %u assertions, GPU lifetime/UUID/active-size replay PASS\n", assertions);
    return 0;
}
