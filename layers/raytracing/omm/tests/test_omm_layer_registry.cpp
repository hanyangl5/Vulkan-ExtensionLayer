// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <new>

namespace registry_fault {
bool enabled = false, triggered = false;
int budget = 0;
}  // namespace registry_fault
void* operator new(size_t size) {
    if (registry_fault::enabled && registry_fault::budget-- == 0) {
        registry_fault::triggered = true;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

// Exercise the actual loader registry/factory and its ownership paths rather
// than reimplementing their bookkeeping in the test. All Vulkan calls below are
// deliberately mocked; the native alias behavior is legal for immutable handles.
#include "../omm_layer.cpp"

namespace {
using namespace vel::omm;
namespace layer = vel::omm::layer;
unsigned checks = 0;
struct NativeState {
    unsigned creates = 0, destroys = 0, pushes = 0, device_destroys = 0;
    int references = 0;
    bool invalid_destroy = false, invalid_push = false;
} native_state;
struct Dispatchable {
    uintptr_t dispatch;
} device_object{0x5a001000}, command_object{0x5a001000};
VkDevice fake_device = reinterpret_cast<VkDevice>(&device_object);
VkCommandBuffer fake_command = reinterpret_cast<VkCommandBuffer>(&command_object);
#if VK_USE_64_BIT_PTR_DEFINES
VkPipelineLayout alias_layout = reinterpret_cast<VkPipelineLayout>(uintptr_t(0x5a002000));
#else
VkPipelineLayout alias_layout = VkPipelineLayout(0x5a002000);
#endif

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL registry: %s\n", message);
        std::exit(1);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*,
                                             VkPipelineLayout* output) {
    ++native_state.creates;
    ++native_state.references;
    *output = alias_layout;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice, VkPipelineLayout handle, const VkAllocationCallbacks*) {
    ++native_state.destroys;
    if (handle != alias_layout || native_state.references <= 0) native_state.invalid_destroy = true;
    --native_state.references;
}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer, VkPipelineLayout handle, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                const void* values) {
    ++native_state.pushes;
    const uint32_t expected = 0x1234abcd;
    if (handle != alias_layout || native_state.references <= 0 || stages != VK_SHADER_STAGE_COMPUTE_BIT || offset ||
        size != sizeof(expected) || std::memcmp(values, &expected, sizeof(expected)))
        native_state.invalid_push = true;
}
VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice, const VkAllocationCallbacks*) { ++native_state.device_destroys; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc(VkDevice, const char* name) {
    if (!std::strcmp(name, "vkCreatePipelineLayout")) return reinterpret_cast<PFN_vkVoidFunction>(create_layout);
    if (!std::strcmp(name, "vkDestroyPipelineLayout")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_layout);
    if (!std::strcmp(name, "vkCmdPushConstants")) return reinterpret_cast<PFN_vkVoidFunction>(push);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_instance_proc(VkInstance, const char* name) { return get_proc(fake_device, name); }
std::shared_ptr<layer::Device> install() {
    native_state = {};
    auto device = std::make_shared<layer::Device>();
    device->handle = fake_device;
    device->next = get_proc;
    device->software = true;
    auto command = std::make_shared<layer::Command>();
    command->state.begin();
    device->commands.emplace(fake_command, command);
    layer::devices.emplace(layer::key(fake_device), device);
    return device;
}
VkPipelineLayoutCreateInfo info(VkPushConstantRange* range) {
    *range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo result{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    result.pushConstantRangeCount = 1;
    result.pPushConstantRanges = range;
    return result;
}
void finish(const std::shared_ptr<layer::Device>& device) {
    layer::DestroyDevice(fake_device, nullptr);
    require(device->commands.empty() && device->layouts.empty(), "device registry resources retired");
    require(layer::devices.empty() && native_state.device_destroys == 1, "native device destroyed once after registry erasure");
    require(native_state.references == 0 && native_state.creates == native_state.destroys && !native_state.invalid_destroy,
            "all native public/private layout refs balanced");
}
void alias_lifetime() {
    auto device = install();
    VkPushConstantRange range;
    auto create_info = info(&range);
    VkPipelineLayout first{}, second{};
    require(layer::CreatePipelineLayout(fake_device, &create_info, nullptr, &first) == VK_SUCCESS, "first immutable layout");
    require(layer::CreatePipelineLayout(fake_device, &create_info, nullptr, &second) == VK_SUCCESS && first == second,
            "driver aliases two public creates");
    require(device->layouts.size() == 1 && native_state.references == 3, "two public references plus one canonical snapshot");
    layer::DestroyPipelineLayout(fake_device, first, nullptr);
    require(device->layouts.size() == 1 && native_state.references == 2, "first destroy preserves surviving alias registry entry");
    const uint32_t value = 0x1234abcd;
    layer::CmdPushConstants(fake_command, second, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(value), &value);
    auto command = layer::command(*device, fake_command);
    require(command->state.status() == VK_SUCCESS && native_state.pushes == 1 && !native_state.invalid_push,
            "valid survivor push uses the canonical snapshot");
    layer::DestroyPipelineLayout(fake_device, second, nullptr);
    require(device->layouts.empty() && native_state.references == 1, "CB retains private snapshot after final public destroy");
    require(command->state.restore(fake_command, {nullptr, nullptr, push}) == VK_SUCCESS && native_state.pushes == 2 &&
                !native_state.invalid_push,
            "restore remains legal after application layout destruction");
    command->state.reset();
    require(native_state.references == 0, "CB reset releases last native snapshot reference");
    finish(device);
}
void allocation_failures() {
    unsigned failed_budgets = 0, successful_budgets = 0;
    for (int budget = 0; budget < 9; ++budget) {
        auto device = install();
        VkPushConstantRange range;
        auto create_info = info(&range);
        VkPipelineLayout handle{};
        registry_fault::budget = budget;
        registry_fault::triggered = false;
        registry_fault::enabled = true;
        const auto result = layer::CreatePipelineLayout(fake_device, &create_info, nullptr, &handle);
        registry_fault::enabled = false;
        if (registry_fault::triggered) {
            ++failed_budgets;
            require(result == VK_ERROR_OUT_OF_HOST_MEMORY && !handle && device->layouts.empty(), "factory OOM is atomic");
            require(native_state.references == 0 && native_state.creates == native_state.destroys && !native_state.invalid_destroy,
                    "snapshot/control-block/map OOM has no native leak or double destroy");
        } else {
            ++successful_budgets;
            require(result == VK_SUCCESS && handle && device->layouts.size() == 1, "factory succeeds beyond allocation budget");
            layer::DestroyPipelineLayout(fake_device, handle, nullptr);
        }
        finish(device);
    }
    require(failed_budgets >= 3 && successful_budgets >= 1, "fault campaign reaches snapshot, control block and map allocations");
    std::printf("Registry factory fault budgets: %u failures, %u successes\n", failed_budgets, successful_budgets);
}
void procedure_surface() {
    auto device = install();
    Dispatchable instance_object{0x5a003000};
    const auto fake_instance = reinterpret_cast<VkInstance>(&instance_object);
    auto instance = std::make_shared<layer::Instance>();
    instance->handle = fake_instance;
    instance->next = get_instance_proc;
    instance->experimental_software = true;
    layer::instances.emplace(layer::key(fake_instance), instance);
    require(!layer::GetDeviceProcAddr(fake_device, "vkCmdBindShadersEXT"), "unavailable shader-object command remains null");
    require(!layer::GetDeviceProcAddr(fake_device, "vkCmdPipelineBarrier2"), "unavailable core command remains null");
    require(!layer::GetDeviceProcAddr(fake_device, "vkCmdPipelineBarrier2KHR"), "unavailable extension alias remains null");
    require(layer::GetDeviceProcAddr(fake_device, "vkCreateMicromapEXT") != nullptr &&
                layer::GetDeviceProcAddr(fake_device, "vkCopyMicromapEXT") != nullptr,
            "enabled emulated EXT functions remain available");
    require(layer::GetDeviceProcAddr(fake_device, "vkCmdPushConstants") != nullptr &&
                layer::GetDeviceProcAddr(fake_device, "vkCmdPushConstants") != reinterpret_cast<PFN_vkVoidFunction>(push),
            "available ordinary command is intercepted");
    require(!layer::GetInstanceProcAddr(VK_NULL_HANDLE, "vkCmdPushConstants"), "null-instance device command remains null");
    for (const auto& hook : layer::hooks)
        if (std::strstr(hook.symbol, "Micromap"))
            require(layer::GetInstanceProcAddr(fake_instance, hook.symbol) == hook.function &&
                        layer::GetDeviceProcAddr(fake_device, hook.symbol) == hook.function,
                    "GIPA/GDPA expose every software micromap hook without a native implementation");
    require(!layer::GetInstanceProcAddr(fake_instance, "vkCmdBindShadersEXT") &&
                !layer::GetInstanceProcAddr(fake_instance, "vkCmdPipelineBarrier2KHR"),
            "software GIPA preserves availability gating for unrelated commands");
    device->software = false;
    instance->experimental_software = false;
    require(!layer::GetDeviceProcAddr(fake_device, "vkCreateMicromapEXT") &&
                layer::GetDeviceProcAddr(fake_device, "vkCmdPushConstants") == reinterpret_cast<PFN_vkVoidFunction>(push),
            "native mode preserves original procedure availability and identity");
    require(!layer::GetInstanceProcAddr(fake_instance, "vkCreateMicromapEXT"),
            "native instance preserves unavailable micromap command");
    layer::instances.erase(layer::key(fake_instance));
    finish(device);
}
}  // namespace

int main() {
    alias_lifetime();
    allocation_failures();
    procedure_surface();
    std::printf("OMM real layer registry: %u checks, alias lifetime/OOM/procedure surface PASS\n", checks);
    return 0;
}
