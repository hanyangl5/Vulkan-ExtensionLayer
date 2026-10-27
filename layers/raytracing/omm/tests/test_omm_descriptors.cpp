// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_descriptors.h"
#include <memory>
#include <atomic>
#include <new>
#include <string>

// This executable links the production tracker directly. Fault every ordinary
// C++ allocation during prepare, and prohibit ANY such allocation at commit.
static std::atomic<long long> fail_new_after{-1};
void* operator new(std::size_t bytes) {
    const auto remaining = fail_new_after.load();
    if (remaining >= 0 && fail_new_after.fetch_sub(1) == 0) throw std::bad_alloc();
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace vel::omm;
namespace {
unsigned assertions = 0;
unsigned private_allocations = 0;
bool fail_private_buffer = false;
std::vector<uint32_t> expected_families;
PFN_vkCreateBuffer native_create_buffer{};
VKAPI_ATTR VkResult VKAPI_CALL failing_native_allocate(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*) {
    return VK_ERROR_OUT_OF_POOL_MEMORY;
}
void require(bool condition, const char* why) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL descriptor context: %s\n", why);
        std::exit(1);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL checked_create_buffer(VkDevice device, const VkBufferCreateInfo* info,
                                                     const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    ++private_allocations;
    if (expected_families.size() > 1) {
        require(info->sharingMode == VK_SHARING_MODE_CONCURRENT && info->queueFamilyIndexCount == expected_families.size() &&
                    std::equal(expected_families.begin(), expected_families.end(), info->pQueueFamilyIndices),
                "private sidecars concurrently shared across physical queue families");
    } else
        require(info->sharingMode == VK_SHARING_MODE_EXCLUSIVE, "single-family private buffer sharing");
    if (fail_private_buffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return native_create_buffer(device, info, allocator, buffer);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL descriptor_proc(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCreateBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(checked_create_buffer);
    return vkGetDeviceProcAddr(device, name);
}
struct Resolver {
    Fixture* fixture;
    VkAccelerationStructureKHR fail{};
    std::vector<std::pair<VkAccelerationStructureKHR, DescriptorASIdentity>> values;
};
VkResult resolve(void* user, VkAccelerationStructureKHR handle, DescriptorASIdentity* output) {
    auto& resolver = *static_cast<Resolver*>(user);
    if (handle == resolver.fail) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (const auto& value : resolver.values)
        if (value.first == handle) {
            *output = value.second;
            return VK_SUCCESS;
        }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}
VkAccelerationStructureKHR make_as(Fixture& fixture) {
    auto storage = fixture.buffer(
        4096, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    auto create = init<VkAccelerationStructureCreateInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
    create.buffer = storage.handle;
    create.size = 4096;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR handle{};
    CHECK(fixture.createAS(fixture.device, &create, nullptr, &handle));
    fixture.structures.push_back(handle);
    return handle;
}
void write(DescriptorBackend* backend, Fixture& fixture, VkDescriptorSet set, uint32_t binding, uint32_t element,
           const std::vector<VkAccelerationStructureKHR>& structures) {
    auto as = init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    as.accelerationStructureCount = uint32_t(structures.size());
    as.pAccelerationStructures = structures.data();
    auto update = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    update.pNext = &as;
    update.dstSet = set;
    update.dstBinding = binding;
    update.dstArrayElement = element;
    update.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    update.descriptorCount = uint32_t(structures.size());
    CHECK(descriptor_update(backend, 1, &update, 0, nullptr));
    vkUpdateDescriptorSets(fixture.device, 1, &update, 0, nullptr);
}
void expected(const DescriptorContextRef& context, uint32_t set, uint32_t binding, uint64_t index,
              const DescriptorASIdentity& wanted) {
    DescriptorASIdentity found;
    CHECK(descriptor_context_fetch(context, set, binding, index, &found));
    require(found.native_address == wanted.native_address && found.root_cell_address == wanted.root_cell_address,
            "native AS/root identity pair");
}
std::vector<uint32_t> words(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input) && input.tellg() > 0 && input.tellg() % 4 == 0, "SPIR-V file");
    std::vector<uint32_t> result(size_t(input.tellg()) / 4);
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(result.data()), result.size() * 4)), "SPIR-V read");
    return result;
}
constexpr uint64_t marker = 0x1122334455667788ull;
const char shader[] = R"GLSL(#version 460
#extension GL_GOOGLE_include_directive : require
#include "omm_context.glsl"
layout(local_size_x=1) in;
layout(set=0,binding=3,std430) buffer ApplicationOutput { uvec4 data[]; } outputData;
void main() {
    uint64_t context = 0x1122334455667788ul;
    outputData.data[0] = ommFetchDescriptor(context,0u,0u,uvec2(0u));
    outputData.data[1] = ommFetchDescriptor(context,0u,2u,uvec2(0u));
    outputData.data[2] = ommFetchDescriptor(context,0u,2u,uvec2(1u,0u));
    uvec4 sentinel = uvec4(0xabcdef01u);
    outputData.data[3] = uvec4(ommFetchDescriptorChecked(context,0u,2u,uvec2(2u,0u),sentinel),
                              ommFetchDescriptorChecked(context,0u,2u,uvec2(0u,1u),sentinel),
                              ommFetchDescriptorChecked(context,1u,0u,uvec2(0u),sentinel),sentinel.x);
}
)GLSL";
VkPipeline pipeline(Fixture& fixture, VkPipelineLayout layout, const std::vector<uint32_t>& original, uint64_t context) {
    auto code = original;
    unsigned replaced = 0;
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        require((code[i] >> 16) != 0, "SPIR-V instruction length");
        if ((code[i] & 0xffffu) == 43u && (code[i] >> 16) == 5u && code[i + 3] == uint32_t(marker) &&
            code[i + 4] == uint32_t(marker >> 32)) {
            code[i + 3] = uint32_t(context);
            code[i + 4] = uint32_t(context >> 32);
            ++replaced;
        }
    }
    require(replaced == 1, "ordinary 64-bit context constant patch");
    auto create = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    create.codeSize = code.size() * 4;
    create.pCode = code.data();
    VkShaderModule module{};
    CHECK(vkCreateShaderModule(fixture.device, &create, nullptr, &module));
    auto compute = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    compute.layout = layout;
    compute.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute.stage.module = module;
    compute.stage.pName = "main";
    VkPipeline result{};
    CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &compute, nullptr, &result));
    vkDestroyShaderModule(fixture.device, module, nullptr);
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--write-shader")) {
        std::ofstream output(argv[2]);
        output << shader;
        return output ? 0 : 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_omm_descriptors provider.comp.spv | --write-shader provider.comp\n");
        return 1;
    }
    Fixture fixture;
    if (!fixture.open()) return 77;
    Resolver resolver{&fixture, {}, {}};
    for (unsigned i = 0; i < 4; ++i) {
        const auto handle = make_as(fixture);
        auto address =
            init<VkAccelerationStructureDeviceAddressInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR);
        address.accelerationStructure = handle;
        auto root = fixture.buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
        resolver.values.push_back(
            {handle, {fixture.addressAS(fixture.device, &address), root.address, std::make_shared<unsigned>(i)}});
    }
    DescriptorBackend* backend{};
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(fixture.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> family_properties(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(fixture.physical, &family_count, family_properties.data());
    for (uint32_t i = 0; i < family_count; ++i)
        if (family_properties[i].queueCount) expected_families.push_back(i);
    native_create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(vkGetDeviceProcAddr(fixture.device, "vkCreateBuffer"));
    CHECK(descriptor_backend_create({fixture.physical, fixture.device, descriptor_proc, nullptr, resolve, &resolver}, &backend));
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 0, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    auto layout_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    layout_info.bindingCount = 4;
    layout_info.pBindings = bindings;
    VkDescriptorSetLayout layout{};
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &layout_info, nullptr, &layout));
    CHECK(descriptor_track_layout(backend, layout, &layout_info));
    CHECK(descriptor_track_layout(backend, layout, &layout_info));  // Immutable handle alias.
    descriptor_untrack_layout(backend, layout);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 12}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(fixture.device, &pool_info, nullptr, &pool));
    CHECK(descriptor_track_pool(backend, pool, &pool_info));
    VkDescriptorSetLayout layouts[] = {layout, layout, layout, layout};
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = layouts;
    VkDescriptorSet sets[4]{};
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, sets));
    CHECK(descriptor_allocate_sets(backend, &allocate, sets));
    descriptor_untrack_layout(backend, layout);  // Sets retain the layout definition.
    write(backend, fixture, sets[0], 0, 0, {resolver.values[0].first, resolver.values[1].first, resolver.values[2].first});
    write(backend, fixture, sets[3], 0, 0, {resolver.values[3].first, resolver.values[2].first, resolver.values[1].first});
    auto copy = init<VkCopyDescriptorSet>(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
    copy.srcSet = sets[3];
    copy.srcBinding = 0;
    copy.dstSet = sets[1];
    copy.dstBinding = 0;
    copy.descriptorCount = 3;
    CHECK(descriptor_update(backend, 0, nullptr, 1, &copy));
    vkUpdateDescriptorSets(fixture.device, 0, nullptr, 1, &copy);
    DescriptorContextRef first, second, reused;
    CHECK(descriptor_build_context(backend, {sets[0]}, &first));
    CHECK(descriptor_build_context(backend, {sets[1]}, &second));
    CHECK(descriptor_build_context(backend, {sets[0]}, &reused));
    require(first.address == reused.address && first.resource == reused.resource && first.address != second.address,
            "immutable distinct contexts/cache");
    expected(first, 0, 0, 0, resolver.values[0].second);
    expected(first, 0, 2, 0, resolver.values[1].second);
    expected(second, 0, 0, 0, resolver.values[3].second);
    copy.srcSet = sets[0];
    copy.srcBinding = 0;
    copy.dstSet = sets[2];
    copy.dstBinding = 0;
    copy.descriptorCount = 3;
    CHECK(descriptor_update(backend, 0, nullptr, 1, &copy));
    vkUpdateDescriptorSets(fixture.device, 0, nullptr, 1, &copy);
    DescriptorContextRef copied;
    CHECK(descriptor_build_context(backend, {VK_NULL_HANDLE, sets[2]}, &copied));
    expected(copied, 1, 2, 1, resolver.values[2].second);
    copy.srcSet = copy.dstSet = sets[0];
    copy.srcBinding = copy.dstBinding = 2;
    copy.descriptorCount = 1;
    require(descriptor_update(backend, 0, nullptr, 1, &copy) == VK_ERROR_VALIDATION_FAILED_EXT, "overlapping copies reject");
    copy.srcSet = sets[0];
    copy.dstSet = sets[2];
    copy.srcBinding = copy.dstBinding = 2;
    copy.srcArrayElement = 1;
    copy.descriptorCount = 2;
    require(descriptor_update(backend, 0, nullptr, 1, &copy) == VK_ERROR_VALIDATION_FAILED_EXT,
            "rollover into incompatible type rejects atomically");
    expected(copied, 1, 2, 1, resolver.values[2].second);
    DescriptorASIdentity missing;
    require(descriptor_context_fetch(first, 0, 2, 2, &missing) == VK_ERROR_VALIDATION_FAILED_EXT, "known bounds enforced");
    require(descriptor_context_fetch(first, 0, 2, uint64_t(1) << 32, &missing) == VK_ERROR_VALIDATION_FAILED_EXT,
            "64-bit index bounds enforced");
    require(descriptor_context_fetch(first, 1, 0, 0, &missing) == VK_ERROR_VALIDATION_FAILED_EXT, "unbound set fails explicitly");
    // Update already-recorded immutable contexts; their set data remains current.
    write(backend, fixture, sets[0], 2, 1, {resolver.values[0].first});
    expected(first, 0, 2, 1, resolver.values[0].second);
    expected(second, 0, 2, 1, resolver.values[1].second);
    expected(copied, 1, 2, 1, resolver.values[2].second);

    // vkUpdateDescriptorSets is void: all valid write/copy staging must already
    // exist. Prohibit C++ allocations across a write followed by chained copies.
    VkAccelerationStructureKHR noalloc_value = resolver.values[2].first;
    auto noalloc_as =
        init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    noalloc_as.accelerationStructureCount = 1;
    noalloc_as.pAccelerationStructures = &noalloc_value;
    auto noalloc_write = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    noalloc_write.pNext = &noalloc_as;
    noalloc_write.dstSet = sets[3];
    noalloc_write.dstBinding = 0;
    noalloc_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    noalloc_write.descriptorCount = 1;
    VkCopyDescriptorSet chained[] = {init<VkCopyDescriptorSet>(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET),
                                     init<VkCopyDescriptorSet>(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET)};
    chained[0].srcSet = sets[3];
    chained[0].dstSet = sets[0];
    chained[0].descriptorCount = 3;
    chained[1].srcSet = sets[0];
    chained[1].dstSet = sets[2];
    chained[1].descriptorCount = 3;
    fail_new_after.store(0);
    const auto noalloc_result = descriptor_update(backend, 1, &noalloc_write, 2, chained);
    const auto noalloc_remaining = fail_new_after.load();
    fail_new_after.store(-1);
    require(noalloc_result == VK_SUCCESS && noalloc_remaining == 0, "valid writes and chained copies allocate no memory");
    expected(first, 0, 0, 0, resolver.values[2].second);
    expected(first, 0, 2, 0, resolver.values[2].second);
    expected(copied, 1, 0, 0, resolver.values[2].second);
    expected(copied, 1, 2, 1, resolver.values[1].second);
    // Restore the exact native descriptors expected by the GPU fixture.
    VkAccelerationStructureKHR restore_values[][3] = {
        {resolver.values[0].first, resolver.values[1].first, resolver.values[0].first},
        {resolver.values[0].first, resolver.values[1].first, resolver.values[2].first}};
    VkWriteDescriptorSetAccelerationStructureKHR restore_as[2]{};
    VkWriteDescriptorSet restore[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        restore_as[i] =
            init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        restore_as[i].accelerationStructureCount = 3;
        restore_as[i].pAccelerationStructures = restore_values[i];
        restore[i] = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        restore[i].pNext = &restore_as[i];
        restore[i].dstSet = sets[i ? 2 : 0];
        restore[i].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        restore[i].descriptorCount = 3;
    }
    fail_new_after.store(0);
    const auto restore_result = descriptor_update(backend, 2, restore, 0, nullptr);
    fail_new_after.store(-1);
    CHECK(restore_result);
    // Invalid late copy rolls back earlier staged writes, also without memory.
    chained[0].srcSet = sets[0];
    chained[0].dstSet = sets[2];
    chained[0].srcBinding = 2;
    chained[0].srcArrayElement = 1;
    chained[0].descriptorCount = 2;
    noalloc_write.dstSet = sets[0];
    fail_new_after.store(0);
    const auto rollback_result = descriptor_update(backend, 1, &noalloc_write, 1, chained);
    fail_new_after.store(-1);
    require(rollback_result == VK_ERROR_VALIDATION_FAILED_EXT, "invalid late batch member rolls back without allocation");
    expected(first, 0, 0, 0, resolver.values[0].second);
    VkAccelerationStructureKHR null_as = VK_NULL_HANDLE;
    noalloc_as.pAccelerationStructures = &null_as;
    noalloc_write.dstSet = sets[3];
    chained[0] = init<VkCopyDescriptorSet>(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
    chained[0].srcSet = sets[3];
    chained[0].dstSet = sets[2];
    chained[0].descriptorCount = 1;
    fail_new_after.store(0);
    const auto null_result = descriptor_update(backend, 1, &noalloc_write, 1, chained);
    fail_new_after.store(-1);
    CHECK(null_result);
    DescriptorASIdentity null_identity;
    CHECK(descriptor_context_fetch(copied, 1, 0, 0, &null_identity));
    require(!null_identity.native_address && !null_identity.root_cell_address,
            "explicit NULL write/copy preserves native null identity");

    // These unsupported live-update flags must not be accepted with torn pairs.
    VkDescriptorBindingFlags flags[] = {VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0, 0};
    auto binding_flags =
        init<VkDescriptorSetLayoutBindingFlagsCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    binding_flags.bindingCount = 4;
    binding_flags.pBindingFlags = flags;
    layout_info.pNext = &binding_flags;
    require(descriptor_track_layout(backend, layout, &layout_info) == VK_ERROR_FEATURE_NOT_PRESENT,
            "AS update-after-bind explicitly unsupported");
    flags[0] = 0;
    flags[3] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    CHECK(descriptor_track_layout(backend, layout, &layout_info));
    descriptor_untrack_layout(backend, layout);
    layout_info.pNext = nullptr;

    // Tracker-only identities exercise legal variable allocation and invalid
    // spans without sending deliberately invalid layouts/updates to the driver.
    auto tracked_layout = reinterpret_cast<VkDescriptorSetLayout>(uintptr_t(0x12340000));
    auto tracked_set = reinterpret_cast<VkDescriptorSet>(uintptr_t(0x12350000));
    VkDescriptorSetLayoutBinding variable_bindings[] = {
        {9, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorBindingFlags variable_flags[] = {VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, 0};
    binding_flags.bindingCount = 2;
    binding_flags.pBindingFlags = variable_flags;
    auto variable_layout = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    variable_layout.pNext = &binding_flags;
    variable_layout.bindingCount = 2;
    variable_layout.pBindings = variable_bindings;
    CHECK(descriptor_track_layout(backend, tracked_layout, &variable_layout));
    uint32_t variable_count = 2;
    auto variable_allocate = init<VkDescriptorSetVariableDescriptorCountAllocateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);
    variable_allocate.descriptorSetCount = 1;
    variable_allocate.pDescriptorCounts = &variable_count;
    auto tracked_allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    tracked_allocate.pNext = &variable_allocate;
    tracked_allocate.descriptorPool = pool;
    tracked_allocate.descriptorSetCount = 1;
    tracked_allocate.pSetLayouts = &tracked_layout;
    variable_count = 5;
    require(descriptor_allocate_sets(backend, &tracked_allocate, &tracked_set) == VK_ERROR_VALIDATION_FAILED_EXT,
            "variable count cannot exceed layout maximum");
    variable_count = 2;
    CHECK(descriptor_allocate_sets(backend, &tracked_allocate, &tracked_set));
    DescriptorContextRef variable_context;
    CHECK(descriptor_build_context(backend, {tracked_set}, &variable_context));
    auto tracked_as =
        init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    VkAccelerationStructureKHR tracked_values[] = {resolver.values[1].first, resolver.values[2].first};
    tracked_as.accelerationStructureCount = 2;
    tracked_as.pAccelerationStructures = tracked_values;
    auto tracked_write = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    tracked_write.pNext = &tracked_as;
    tracked_write.dstSet = tracked_set;
    tracked_write.dstBinding = 9;
    tracked_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    tracked_write.descriptorCount = 2;
    CHECK(descriptor_update(backend, 1, &tracked_write, 0, nullptr));
    expected(variable_context, 0, 9, 1, resolver.values[2].second);
    require(descriptor_context_fetch(variable_context, 0, 9, 2, &missing) == VK_ERROR_VALIDATION_FAILED_EXT,
            "context uses allocated variable count, not layout maximum");
    tracked_write.dstBinding = 2;
    require(descriptor_update(backend, 1, &tracked_write, 0, nullptr) == VK_ERROR_VALIDATION_FAILED_EXT,
            "numeric binding gap is not a zero-count binding");
    tracked_write.dstBinding = 9;
    resolver.fail = tracked_values[1];
    tracked_values[0] = resolver.values[0].first;
    require(descriptor_update(backend, 1, &tracked_write, 0, nullptr) == VK_ERROR_FEATURE_NOT_PRESENT,
            "identity resolver failure propagates");
    expected(variable_context, 0, 9, 0, resolver.values[1].second);
    resolver.fail = VK_NULL_HANDLE;
    descriptor_free_sets(backend, pool, 1, &tracked_set);
    descriptor_untrack_layout(backend, tracked_layout);
    variable_context = {};

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    auto empty_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    VkDescriptorSetLayout empty{};
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &empty_info, nullptr, &empty));
    std::vector<VkDescriptorSetLayout> full_sets(properties.limits.maxBoundDescriptorSets, empty);
    full_sets[0] = layout;
    VkPushConstantRange full_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, properties.limits.maxPushConstantsSize};
    auto pipeline_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pipeline_info.setLayoutCount = uint32_t(full_sets.size());
    pipeline_info.pSetLayouts = full_sets.data();
    pipeline_info.pushConstantRangeCount = 1;
    pipeline_info.pPushConstantRanges = &full_push;
    VkPipelineLayout pipeline_layout{};
    CHECK(vkCreatePipelineLayout(fixture.device, &pipeline_info, nullptr, &pipeline_layout));
    auto code = words(argv[1]);
    VkPipeline pipelines[] = {pipeline(fixture, pipeline_layout, code, first.address),
                              pipeline(fixture, pipeline_layout, code, second.address)};
    Buffer output[] = {fixture.buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true),
                       fixture.buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)};
    for (unsigned i = 0; i < 2; ++i) {
        VkDescriptorBufferInfo buffer{output[i].handle, 0, 64};
        auto update = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        update.dstSet = sets[i];
        update.dstBinding = 3;
        update.descriptorCount = 1;
        update.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        update.pBufferInfo = &buffer;
        CHECK(descriptor_update(backend, 1, &update, 0, nullptr));
        vkUpdateDescriptorSets(fixture.device, 1, &update, 0, nullptr);
    }
    // Two command buffers record distinct immutable variant contexts BEFORE a
    // single submission. This catches a shared current-set/context head race.
    VkCommandBuffer commands[] = {fixture.begin(), fixture.begin()};
    for (unsigned i = 0; i < 2; ++i) {
        vkCmdBindPipeline(commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
        vkCmdBindDescriptorSets(commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &sets[i], 0, nullptr);
        vkCmdDispatch(commands[i], 1, 1, 1);
        Fixture::barrier(commands[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(commands[i]));
    }
    auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = commands;
    CHECK(vkQueueSubmit(fixture.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(fixture.queue));
    for (unsigned i = 0; i < 2; ++i) {
        const DescriptorASIdentity* wanted[] = {&resolver.values[i ? 3 : 0].second, &resolver.values[i ? 2 : 1].second,
                                                &resolver.values[i ? 1 : 0].second};
        const auto* data = static_cast<const uint64_t*>(output[i].mapped);
        for (unsigned j = 0; j < 3; ++j)
            require(data[j * 2] == wanted[j]->native_address && data[j * 2 + 1] == wanted[j]->root_cell_address,
                    "GPU explicit-context fetch matches native descriptor writes/copies");
        const auto* status = static_cast<const uint32_t*>(output[i].mapped) + 12;
        require(status[0] == 2 && status[1] == 2 && status[2] == 2 && status[3] == 0xabcdef01u,
                "GPU bounds fail explicitly without touching result");
    }
    vkFreeCommandBuffers(fixture.device, fixture.pool, 2, commands);
    for (auto value : pipelines) vkDestroyPipeline(fixture.device, value, nullptr);
    vkDestroyPipelineLayout(fixture.device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, empty, nullptr);

    // Pools WITHOUT FREE_DESCRIPTOR_SET: all shadow allocations must precede
    // native allocation, so a private OOM/native failure cannot consume a slot.
    CHECK(descriptor_track_layout(backend, layout, &layout_info));
    VkDescriptorPool transaction_pools[2]{};
    auto transaction_pool_info = pool_info;
    transaction_pool_info.flags = 0;
    transaction_pool_info.maxSets = 1;
    for (auto& value : transaction_pools) {
        CHECK(vkCreateDescriptorPool(fixture.device, &transaction_pool_info, nullptr, &value));
        CHECK(descriptor_track_pool(backend, value, &transaction_pool_info));
    }
    auto transaction_info = allocate;
    transaction_info.descriptorSetCount = 1;
    transaction_info.pSetLayouts = &layout;
    transaction_info.descriptorPool = transaction_pools[0];
    DescriptorAllocation* transaction{};
    fail_private_buffer = true;
    const auto private_failure = descriptor_prepare_allocate_sets(backend, &transaction_info, &transaction);
    fail_private_buffer = false;
    require(private_failure == VK_ERROR_OUT_OF_DEVICE_MEMORY && !transaction, "private GPU OOM leaves allocation unpublished");
    unsigned fault_positions = 0;
    for (long long position = 0; position < 128; ++position) {
        fail_new_after.store(position);
        const auto result = descriptor_prepare_allocate_sets(backend, &transaction_info, &transaction);
        fail_new_after.store(-1);
        if (result == VK_SUCCESS) {
            descriptor_abort_allocate_sets(transaction);
            transaction = nullptr;
            break;
        }
        ++fault_positions;
        require(result == VK_ERROR_OUT_OF_HOST_MEMORY && !transaction, "host prepare OOM is transactional at every allocation");
    }
    require(fault_positions >= 5 && fault_positions < 128, "all CPU allocation fault positions exercised");
    CHECK(descriptor_prepare_allocate_sets(backend, &transaction_info, &transaction));
    // Emulate a failing downstream allocation; no native set exists to free.
    VkDescriptorSet failed_native_set{};
    PFN_vkAllocateDescriptorSets failing_allocate = failing_native_allocate;
    const auto native_failure = failing_allocate(fixture.device, &transaction_info, &failed_native_set);
    require(native_failure == VK_ERROR_OUT_OF_POOL_MEMORY && !failed_native_set, "mock downstream allocation failure");
    fail_new_after.store(0);
    descriptor_abort_allocate_sets(transaction);
    const auto abort_remaining = fail_new_after.load();
    fail_new_after.store(-1);
    require(abort_remaining == 0, "abort performs no C++ allocations");
    transaction = nullptr;
    DescriptorAllocation* outstanding[2]{};
    VkDescriptorSet transaction_sets[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        transaction_info.descriptorPool = transaction_pools[i];
        CHECK(descriptor_prepare_allocate_sets(backend, &transaction_info, &outstanding[i]));
    }
    // Both reservations coexist while another failed prepare leaves them intact.
    fail_new_after.store(0);
    const auto concurrent_failure = descriptor_prepare_allocate_sets(backend, &transaction_info, &transaction);
    fail_new_after.store(-1);
    require(concurrent_failure == VK_ERROR_OUT_OF_HOST_MEMORY && !transaction,
            "failed prepare preserves other pools' reservations");
    for (unsigned i = 0; i < 2; ++i) {
        transaction_info.descriptorPool = transaction_pools[i];
        CHECK(vkAllocateDescriptorSets(fixture.device, &transaction_info, &transaction_sets[i]));
    }
    fail_new_after.store(0);
    descriptor_commit_allocate_sets(outstanding[1], &transaction_sets[1]);
    descriptor_commit_allocate_sets(outstanding[0], &transaction_sets[0]);
    const auto commit_remaining = fail_new_after.load();
    fail_new_after.store(-1);
    require(commit_remaining == 0, "commit performs no C++ allocations across outstanding pools");
    DescriptorContextRef transaction_contexts[2];
    for (unsigned i = 0; i < 2; ++i) {
        write(backend, fixture, transaction_sets[i], 0, 0, {resolver.values[i].first});
        CHECK(descriptor_build_context(backend, {transaction_sets[i]}, &transaction_contexts[i]));
        expected(transaction_contexts[i], 0, 0, 0, resolver.values[i].second);
        descriptor_untrack_pool(backend, transaction_pools[i]);
        vkDestroyDescriptorPool(fixture.device, transaction_pools[i], nullptr);
        transaction_contexts[i] = {};
    }
    expected(first, 0, 2, 1, resolver.values[0].second);
    expected(second, 0, 0, 0, resolver.values[3].second);
    descriptor_untrack_layout(backend, layout);

    descriptor_free_sets(backend, pool, 1, &sets[0]);
    CHECK(vkFreeDescriptorSets(fixture.device, pool, 1, &sets[0]));
    expected(first, 0, 0, 0, resolver.values[0].second);  // Context owns allocation.
    descriptor_reset_pool(backend, pool);
    CHECK(vkResetDescriptorPool(fixture.device, pool, 0));
    expected(second, 0, 0, 0, resolver.values[3].second);
    DescriptorContextRef invalid_context;
    require(descriptor_build_context(backend, {sets[1]}, &invalid_context) == VK_ERROR_VALIDATION_FAILED_EXT,
            "freed pool sets cannot create new contexts");
    descriptor_untrack_pool(backend, pool);
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, layout, nullptr);
    descriptor_backend_destroy(backend);
    expected(copied, 1, 2, 1, resolver.values[2].second);  // Native allocator lifetime retained.
    first = {};
    second = {};
    reused = {};
    copied = {};
    require(private_allocations != 0, "private allocation sharing intercepted");
    std::printf(
        "OMM descriptor contexts: %u checks, two GPU command buffers, %u application set slots and %u push bytes occupied, %zu "
        "queue families\n",
        assertions, properties.limits.maxBoundDescriptorSets, properties.limits.maxPushConstantsSize, expected_families.size());
    return 0;
}
