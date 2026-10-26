// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_command_state.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

// Fault only the record operation being tested, never the mock or assertions.
static thread_local int allocationBudget = -1;
void* operator new(std::size_t size) {
    if (allocationBudget >= 0 && allocationBudget-- == 0) throw std::bad_alloc();
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

using namespace vel::omm;
namespace {
unsigned checks = 0, failures = 0;
void expect(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
template <typename T>
T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}
enum class Kind { Pipeline, Shader, Push };
struct Call {
    Kind kind{};
    VkCommandBuffer cb{};
    VkPipeline pipeline{};
    VkShaderEXT shader{};
    VkPipelineLayout layout{};
    VkShaderStageFlags stages{};
    uint32_t offset = 0, size = 0;
    std::array<uint8_t, 256> bytes{};
};
std::vector<Call> calls;
constexpr VkShaderStageFlags observedStages[] = {VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_VERTEX_BIT,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_RAYGEN_BIT_KHR};
using Values = std::array<std::array<uint8_t, 128>, 4>;
Values nativeValues;
void apply(Values& values, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* bytes) {
    if (offset + size > 128) std::abort();
    for (unsigned stage = 0; stage < 4; ++stage)
        if (stages & observedStages[stage]) std::memcpy(values[stage].data() + offset, bytes, size);
}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (point != VK_PIPELINE_BIND_POINT_COMPUTE) std::abort();
    Call call{};
    call.kind = Kind::Pipeline;
    call.cb = cb;
    call.pipeline = pipeline;
    calls.push_back(call);
}
VKAPI_ATTR void VKAPI_CALL bindShaders(VkCommandBuffer cb, uint32_t count, const VkShaderStageFlagBits* stages,
                                       const VkShaderEXT* shaders) {
    if (count != 1 || stages[0] != VK_SHADER_STAGE_COMPUTE_BIT || !shaders) std::abort();
    Call call{};
    call.kind = Kind::Shader;
    call.cb = cb;
    call.shader = shaders[0];
    call.stages = stages[0];
    calls.push_back(call);
}
VKAPI_ATTR void VKAPI_CALL pushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                         uint32_t size, const void* values) {
    if (size > 256) std::abort();
    Call call{};
    call.kind = Kind::Push;
    call.cb = cb;
    call.layout = layout;
    call.stages = stages;
    call.offset = offset;
    call.size = size;
    std::memcpy(call.bytes.data(), values, size);
    calls.push_back(call);
    apply(nativeValues, stages, offset, size, values);
}
NativeComputeDispatch dispatch{bindPipeline, bindShaders, pushConstants};
PushLayoutRef snapshot(uintptr_t value, unsigned& destroyed) {
    return PushLayoutRef(new PushLayoutSnapshot{handle<VkPipelineLayout>(value)}, [&destroyed](const PushLayoutSnapshot* value) {
        ++destroyed;
        delete value;
    });
}
void clearCalls() { calls.clear(); }
void bindings() {
    CommandState state;
    const auto cb = handle<VkCommandBuffer>(1);
    expect(state.restore(cb, {}) == VK_SUCCESS && calls.empty(), "unbound compute state emits no invented pipeline/shader bind");
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, handle<VkPipeline>(11));
    expect(state.restore(cb, {}) == VK_SUCCESS && calls.empty(), "graphics pipeline does not create a compute binding");
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(12));
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].kind == Kind::Pipeline &&
               calls[0].pipeline == handle<VkPipeline>(12),
           "restore application compute pipeline");
    clearCalls();
    const VkShaderStageFlagBits graphics[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkShaderEXT graphicsShaders[] = {handle<VkShaderEXT>(21), handle<VkShaderEXT>(22)};
    state.bind_shaders(2, graphics, graphicsShaders);
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, handle<VkPipeline>(13));
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].pipeline == handle<VkPipeline>(12),
           "graphics shader and RTP pipeline commands preserve compute selection");
    clearCalls();
    const VkShaderStageFlagBits mixed[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkShaderEXT shaders[] = {handle<VkShaderEXT>(31), handle<VkShaderEXT>(32), handle<VkShaderEXT>(33)};
    state.bind_shaders(3, mixed, shaders);
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].kind == Kind::Shader &&
               calls[0].shader == handle<VkShaderEXT>(32) && calls[0].stages == VK_SHADER_STAGE_COMPUTE_BIT,
           "compute shader object supersedes the earlier pipeline without rebinding graphics stages");
    clearCalls();
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(14));
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].kind == Kind::Pipeline &&
               calls[0].pipeline == handle<VkPipeline>(14),
           "compute pipeline supersedes an earlier shader object");
    clearCalls();
    const VkShaderStageFlagBits compute = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkShaderEXT nullShader = VK_NULL_HANDLE;
    state.bind_shaders(1, &compute, &nullShader);
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].kind == Kind::Shader &&
               calls[0].shader == VK_NULL_HANDLE,
           "explicit NULL compute shader unbind does not resurrect an old pipeline");
    clearCalls();
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(15));
    state.bind_shaders(1, &compute, nullptr);
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].kind == Kind::Shader &&
               calls[0].shader == VK_NULL_HANDLE,
           "NULL shader array has the specified unbind semantics");
    clearCalls();
    state.begin();
    expect(state.status() == VK_SUCCESS && state.restore(cb, {}) == VK_SUCCESS && calls.empty(),
           "new recording discards old compute bindings");
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(16));
    state.reset();
    expect(state.restore(cb, {}) == VK_SUCCESS && calls.empty(), "command buffer reset discards compute bindings");
}
void historyAndLifetime() {
    CommandState state;
    const auto cb = handle<VkCommandBuffer>(2);
    unsigned destroyed = 0;
    auto all = snapshot(1001, destroyed), fragment = snapshot(1002, destroyed), vertex = snapshot(1003, destroyed),
         compute = snapshot(1004, destroyed), raygen = snapshot(1005, destroyed);
    const std::weak_ptr<const PushLayoutSnapshot> weakAll = all, weakCompute = compute;
    Values expected{};
    for (auto& stage : expected) stage.fill(0xcd);
    nativeValues = expected;
    struct Expected {
        VkPipelineLayout layout;
        VkShaderStageFlags stages;
        uint32_t offset, size;
        std::array<uint8_t, 128> data;
    };
    std::vector<Expected> history;
    auto record = [&](PushLayoutRef layout, VkShaderStageFlags flags, uint32_t offset, uint32_t size, uint8_t seed) {
        std::array<uint8_t, 128> data{};
        for (uint32_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(seed + i);
        expect(state.push(layout, flags, offset, size, data.data()) == VK_SUCCESS, "record original push call");
        apply(expected, flags, offset, size, data.data());
        history.push_back({layout->native_layout, flags, offset, size, data});
        data.fill(0xff);  // The captured bytes must not retain an application pointer.
    };
    record(all, VK_SHADER_STAGE_ALL, 0, 128, 1);
    record(fragment, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, 21);
    record(vertex, VK_SHADER_STAGE_VERTEX_BIT, 4, 8, 41);
    record(compute, VK_SHADER_STAGE_COMPUTE_BIT, 32, 16, 61);
    record(all, VK_SHADER_STAGE_ALL, 64, 4, 81);
    record(raygen, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 8, 12, 101);
    record(fragment, VK_SHADER_STAGE_FRAGMENT_BIT, 8, 4, 121);
    all.reset();
    fragment.reset();
    vertex.reset();
    compute.reset();
    raygen.reset();
    expect(destroyed == 0 && !weakAll.expired() && !weakCompute.expired(),
           "recorded commands retain private compatible layouts after public layout destruction");
    state.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(99));
    std::array<uint8_t, 96> internal;
    internal.fill(0xee);
    nativeValues = expected;
    apply(nativeValues, VK_SHADER_STAGE_COMPUTE_BIT, 0, 96, internal.data());
    clearCalls();
    // Native callbacks use preallocated fixed-size records, so this also checks
    // that the production restore path does not perform a host allocation.
    allocationBudget = 0;
    const auto result = state.restore(cb, dispatch);
    allocationBudget = -1;
    expect(result == VK_SUCCESS && nativeValues == expected,
           "full chronological replay restores all stages after internal compute PC96");
    expect(calls.size() == history.size() + 1 && calls[0].kind == Kind::Pipeline, "restore selected backend before push history");
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& actual = calls[i + 1];
        const auto& original = history[i];
        expect(actual.kind == Kind::Push && actual.cb == cb && actual.layout == original.layout &&
                   actual.stages == original.stages && actual.offset == original.offset && actual.size == original.size &&
                   !std::memcmp(actual.bytes.data(), original.data.data(), original.size),
               "replay uses exact native snapshot, stage flags including ALL, offset and copied bytes in original order");
    }
    clearCalls();
    expect(state.restore(cb, dispatch) == VK_SUCCESS && calls.size() == history.size() + 1 && nativeValues == expected,
           "repeated internal work can restore without appending its own commands to history");
    state.reset();
    expect(destroyed == 5 && weakAll.expired() && weakCompute.expired(),
           "CB reset releases every retained private layout exactly once");
    clearCalls();
    expect(state.restore(cb, {}) == VK_SUCCESS && calls.empty(), "CB reuse cannot replay stale pushes");
    {
        CommandState other;
        auto layout = snapshot(1006, destroyed);
        uint32_t word = 4;
        expect(other.push(layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &word) == VK_SUCCESS,
               "graphics-only push before any compute bind");
        layout.reset();
        expect(other.restore(cb, dispatch) == VK_SUCCESS && calls.size() == 1 && calls[0].kind == Kind::Push,
               "no application compute binding still restores graphics push history without a fake bind");
    }
    expect(destroyed == 6, "freeing a command buffer destroys its state and releases layout tokens");
    clearCalls();
}
void independenceAndErrors() {
    const auto first = handle<VkCommandBuffer>(3), second = handle<VkCommandBuffer>(4);
    unsigned destroyed = 0;
    auto layout = snapshot(2001, destroyed);
    uint32_t word = 0x12345678;
    CommandState a, b;
    a.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(71));
    b.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(72));
    expect(a.push(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &word) == VK_SUCCESS, "record CB A push");
    word = 0xabcdef01;
    expect(b.push(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &word) == VK_SUCCESS, "record CB B push");
    clearCalls();
    a.restore(first, dispatch);
    b.restore(second, dispatch);
    expect(calls.size() == 4 && calls[0].cb == first && calls[0].pipeline == handle<VkPipeline>(71) && calls[1].cb == first &&
               calls[1].bytes[0] == 0x78 && calls[2].cb == second && calls[2].pipeline == handle<VkPipeline>(72) &&
               calls[3].cb == second && calls[3].bytes[0] == 0x01,
           "command buffers preserve independent bindings and byte histories");
    clearCalls();
    NativeComputeDispatch missingPush = dispatch;
    missingPush.push_constants = nullptr;
    expect(a.restore(first, missingPush) == VK_ERROR_INITIALIZATION_FAILED && a.status() == VK_ERROR_INITIALIZATION_FAILED &&
               calls.empty(),
           "missing native replay entry point fails before any partial command emission");
    a.fail(VK_ERROR_OUT_OF_HOST_MEMORY);
    expect(a.status() == VK_ERROR_INITIALIZATION_FAILED, "first record error remains sticky");
    a.begin();
    expect(a.status() == VK_SUCCESS, "successful begin clears an earlier sticky error");
    a.fail(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    expect(a.restore(first, dispatch) == VK_ERROR_OUT_OF_DEVICE_MEMORY && calls.empty(),
           "backend allocation error prevents restoration into a failed CB");
    a.reset();
    NativeComputeDispatch onlyPipeline{};
    onlyPipeline.bind_pipeline = bindPipeline;
    a.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(73));
    expect(a.restore(first, onlyPipeline) == VK_SUCCESS && calls.size() == 1, "unused native entry points need not be present");
    clearCalls();
    expect(a.push({}, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &word) == VK_ERROR_INITIALIZATION_FAILED,
           "missing layout snapshot fails closed before recording an unsafe public handle");
    expect(a.restore(first, dispatch) == VK_ERROR_INITIALIZATION_FAILED && calls.empty(), "invalid snapshot error is sticky");
}
void allocationFailures() {
    const auto cb = handle<VkCommandBuffer>(5);
    unsigned destroyed = 0;
    auto layout = snapshot(3001, destroyed);
    uint32_t word = 7;
    for (int budget : {0, 1}) {
        CommandState state;
        state.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle<VkPipeline>(91));
        allocationBudget = budget;
        const auto result = state.push(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &word);
        allocationBudget = -1;
        expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && state.status() == VK_ERROR_OUT_OF_HOST_MEMORY,
               "byte-copy and history-growth allocation failures become sticky OOM");
        clearCalls();
        expect(state.restore(cb, dispatch) == VK_ERROR_OUT_OF_HOST_MEMORY && calls.empty(), "OOM causes no partial state replay");
        expect(state.push(layout, VK_SHADER_STAGE_ALL, 0, 4, &word) == VK_ERROR_OUT_OF_HOST_MEMORY,
               "subsequent record cannot hide an earlier OOM");
        state.reset();
        expect(
            state.push(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &word) == VK_SUCCESS && state.restore(cb, dispatch) == VK_SUCCESS,
            "reset makes the CB reusable after allocation failure");
    }
    clearCalls();
    CommandState state;
    expect(state.push(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &word) == VK_SUCCESS, "capture a prefix before OOM");
    auto failedLayout = snapshot(3002, destroyed);
    std::weak_ptr<const PushLayoutSnapshot> failedWeak = failedLayout;
    allocationBudget = 1;  // bytes succeed, vector growth fails; the old history remains owned.
    const auto result = state.push(failedLayout, VK_SHADER_STAGE_COMPUTE_BIT, 4, 4, &word);
    allocationBudget = -1;
    failedLayout.reset();
    expect(result == VK_ERROR_OUT_OF_HOST_MEMORY && failedWeak.expired() && destroyed == 1,
           "failed append releases its own layout without leaking or replacing the prefix");
    layout.reset();
    expect(destroyed == 1, "valid history prefix stays alive until reset despite later OOM");
    state.reset();
    expect(destroyed == 2, "reset releases the retained prefix after OOM");
}
}  // namespace
int main() {
    calls.reserve(1024);
    bindings();
    historyAndLifetime();
    independenceAndErrors();
    allocationFailures();
    std::printf("Command state preservation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
