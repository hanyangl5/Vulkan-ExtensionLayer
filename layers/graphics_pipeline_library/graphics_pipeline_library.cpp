/*
 * Copyright 2026 Xiaomi
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics_pipeline_library/graphics_pipeline_library.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_concurrent_unordered_map.hpp>

#include "cast_utils.h"
#include "log.h"
#include "vk_common.h"

namespace graphics_pipeline_library {

// Registries and small helpers

static const VkLayerProperties kGlobalLayer = {
    "VK_LAYER_PA_graphics_pipeline_library",
    VK_HEADER_VERSION_COMPLETE,
    1,
    "Transparent GPL pipeline-state-object acceleration layer",
};
static const char* const kLayerName = kGlobalLayer.layerName;

static vku::concurrent::unordered_map<void*, InstanceData*> instance_data_map;
static vku::concurrent::unordered_map<void*, DeviceData*> device_data_map;

static InstanceData* GetInstanceData(const void* object) {
    auto result = instance_data_map.find(DispatchKey(object));
    return result != instance_data_map.end() ? result->second : nullptr;
}

static DeviceData* GetDeviceData(const void* object) {
    auto result = device_data_map.find(DispatchKey(object));
    return result != device_data_map.end() ? result->second : nullptr;
}

#if defined(__GNUC__) && __GNUC__ >= 4
#define VEL_EXPORT __attribute__((visibility("default")))
#else
#define VEL_EXPORT
#endif

// Diagnostic logging. The repo's LOG() macro writes to stdout, which GUI-subsystem hosts discard;
// PA_GPL_LOG_FILE routes the layer's own messages to a file so such hosts can be inspected.
// Falls back to stdout otherwise. PA_GPL_LOG>=2 additionally enables per-pipeline debug lines.
// The original XIAOMI_GPL_LOG and XIAOMI_GPL_LOG_FILE variables remain supported as aliases.
static const char* LogSetting(const char* name, const char* legacy_name) {
    const char* value = std::getenv(name);
    return value ? value : std::getenv(legacy_name);
}

static bool DebugLoggingEnabled() {
    static const bool enabled = [] {
        const char* v = LogSetting("PA_GPL_LOG", "XIAOMI_GPL_LOG");
        return v && std::atoi(v) >= 2;
    }();
    return enabled;
}

static void LayerMessageV(const char* fmt, va_list args) {
    static std::mutex log_mutex;
    static FILE* file = [] {
        const char* path = LogSetting("PA_GPL_LOG_FILE", "XIAOMI_GPL_LOG_FILE");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    std::unique_lock<std::mutex> lock(log_mutex);
    std::vfprintf(file ? file : stdout, fmt, args);
    std::fflush(file ? file : stdout);
}

static void LayerMessage(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LayerMessageV(fmt, args);
    va_end(args);
}

// Per-pipeline / per-call detail; only emitted at PA_GPL_LOG>=2.
static void LayerDebug(const char* fmt, ...) {
    if (!DebugLoggingEnabled()) return;
    va_list args;
    va_start(args, fmt);
    LayerMessageV(fmt, args);
    va_end(args);
}

VkLayerInstanceCreateInfo* GetChainInfo(const VkInstanceCreateInfo* pCreateInfo, VkLayerFunction func) {
    auto chain_info = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(pCreateInfo->pNext));
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain_info->function == func)) {
        chain_info = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain_info->pNext));
    }
    return chain_info;
}

VkLayerDeviceCreateInfo* GetChainInfo(const VkDeviceCreateInfo* pCreateInfo, VkLayerFunction func) {
    auto chain_info = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(pCreateInfo->pNext));
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain_info->function == func)) {
        chain_info = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain_info->pNext));
    }
    return chain_info;
}

VkDynamicState DynamicStateForBit(StateBit bit) {
    switch (bit) {
        case kCullMode:
            return VK_DYNAMIC_STATE_CULL_MODE;
        case kFrontFace:
            return VK_DYNAMIC_STATE_FRONT_FACE;
        case kDepthTestEnable:
            return VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
        case kDepthWriteEnable:
            return VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
        case kDepthCompareOp:
            return VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
    }
    return VK_DYNAMIC_STATE_MAX_ENUM;
}

// Maps one of our five dynamic states back to its state bit, or 0 if it is not one of ours. The EXT
// aliases share the core enum values, so matching the core enum covers both.
static StateMask StateBitForDynamicState(VkDynamicState state) {
    switch (state) {
        case VK_DYNAMIC_STATE_CULL_MODE:
            return kCullMode;
        case VK_DYNAMIC_STATE_FRONT_FACE:
            return kFrontFace;
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
            return kDepthTestEnable;
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
            return kDepthWriteEnable;
        case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
            return kDepthCompareOp;
        default:
            return 0;
    }
}

GplSubset ClassifyDynamicState(VkDynamicState state) {
    switch (state) {
        // Vertex input interface.
        case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
        case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
        case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
        case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
            return GplSubset::kVertexInput;

        // Pre-rasterization shaders.
        case VK_DYNAMIC_STATE_VIEWPORT:
        case VK_DYNAMIC_STATE_SCISSOR:
        case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
        case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
        case VK_DYNAMIC_STATE_LINE_WIDTH:
        case VK_DYNAMIC_STATE_DEPTH_BIAS:
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
        case VK_DYNAMIC_STATE_CULL_MODE:
        case VK_DYNAMIC_STATE_FRONT_FACE:
        case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
        case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
        case VK_DYNAMIC_STATE_TESSELLATION_DOMAIN_ORIGIN_EXT:
        case VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_POLYGON_MODE_EXT:
        case VK_DYNAMIC_STATE_RASTERIZATION_STREAM_EXT:
        case VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT:
        case VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT:
        case VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT:
        case VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT:
        case VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_LINE_STIPPLE_EXT:  // core / EXT / KHR aliases have the same value
        case VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT:
        case VK_DYNAMIC_STATE_DEPTH_CLAMP_RANGE_EXT:
        case VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_NV:
        case VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_ENABLE_NV:
        case VK_DYNAMIC_STATE_VIEWPORT_SWIZZLE_NV:
        case VK_DYNAMIC_STATE_VIEWPORT_SHADING_RATE_PALETTE_NV:
        case VK_DYNAMIC_STATE_VIEWPORT_COARSE_SAMPLE_ORDER_NV:
        case VK_DYNAMIC_STATE_SHADING_RATE_IMAGE_ENABLE_NV:
        case VK_DYNAMIC_STATE_EXCLUSIVE_SCISSOR_ENABLE_NV:
        case VK_DYNAMIC_STATE_EXCLUSIVE_SCISSOR_NV:
        case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_EXT:
        case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_MODE_EXT:
            return GplSubset::kPreRaster;

        // Fragment shader (depth / stencil).
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
        case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
        case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
        case VK_DYNAMIC_STATE_STENCIL_OP:
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
        case VK_DYNAMIC_STATE_REPRESENTATIVE_FRAGMENT_TEST_ENABLE_NV:
            return GplSubset::kFragmentShader;

        // Fragment output interface.
        case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
        case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
        case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
        case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT:
        case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT:
            return GplSubset::kFragmentOutput;

        // These structs belong to both subsets. All applicable libraries must agree on dynamicness.
        case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT:
        case VK_DYNAMIC_STATE_SAMPLE_MASK_EXT:
        case VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT:
        case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
        case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COVERAGE_TO_COLOR_ENABLE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_TO_COLOR_LOCATION_NV:
        case VK_DYNAMIC_STATE_COVERAGE_MODULATION_MODE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_ENABLE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_NV:
        case VK_DYNAMIC_STATE_COVERAGE_REDUCTION_MODE_NV:
            return GplSubset::kFragmentShaderAndOutput;
        case VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR:
            return GplSubset::kPreRasterAndFragmentShader;

        default:
            return GplSubset::kUnknown;
    }
}

// Dispatch-table initialization

void LayerDispatchInstance::Initialize(VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    auto load = [&](const char* name) { return gipa(instance, name); };
    GetInstanceProcAddr = gipa;
    DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(load("vkDestroyInstance"));
    EnumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(load("vkEnumerateDeviceExtensionProperties"));
    GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(load("vkGetPhysicalDeviceProperties"));

    GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(load("vkGetPhysicalDeviceProperties2"));
    if (!GetPhysicalDeviceProperties2) {
        GetPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(load("vkGetPhysicalDeviceProperties2KHR"));
    }
    GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(load("vkGetPhysicalDeviceFeatures2"));
    if (!GetPhysicalDeviceFeatures2) {
        GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(load("vkGetPhysicalDeviceFeatures2KHR"));
    }
}

void LayerDispatchDevice::Initialize(VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    // Resolve the core name first, then the EXT alias, so the table works whether the states are
    // core 1.3 or provided by VK_EXT_extended_dynamic_state / VK_EXT_private_data.
    auto load = [&](const char* core, const char* ext) {
        PFN_vkVoidFunction fn = gdpa(device, core);
        if (!fn && ext) fn = gdpa(device, ext);
        return fn;
    };

    GetDeviceProcAddr = gdpa;
    DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(load("vkDestroyDevice", nullptr));
    CreateGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(load("vkCreateGraphicsPipelines", nullptr));
    DestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline", nullptr));
    CmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline", nullptr));
    DestroyRenderPass = reinterpret_cast<PFN_vkDestroyRenderPass>(load("vkDestroyRenderPass", nullptr));
    CreateRenderPass = reinterpret_cast<PFN_vkCreateRenderPass>(load("vkCreateRenderPass", nullptr));
    CreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(load("vkCreateRenderPass2", "vkCreateRenderPass2KHR"));
    DestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(load("vkDestroyShaderModule", nullptr));
    DestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(load("vkDestroyPipelineLayout", nullptr));

    CreatePrivateDataSlot =
        reinterpret_cast<PFN_vkCreatePrivateDataSlot>(load("vkCreatePrivateDataSlot", "vkCreatePrivateDataSlotEXT"));
    DestroyPrivateDataSlot =
        reinterpret_cast<PFN_vkDestroyPrivateDataSlot>(load("vkDestroyPrivateDataSlot", "vkDestroyPrivateDataSlotEXT"));
    SetPrivateData = reinterpret_cast<PFN_vkSetPrivateData>(load("vkSetPrivateData", "vkSetPrivateDataEXT"));
    GetPrivateData = reinterpret_cast<PFN_vkGetPrivateData>(load("vkGetPrivateData", "vkGetPrivateDataEXT"));

    CmdSetCullMode = reinterpret_cast<PFN_vkCmdSetCullMode>(load("vkCmdSetCullMode", "vkCmdSetCullModeEXT"));
    CmdSetFrontFace = reinterpret_cast<PFN_vkCmdSetFrontFace>(load("vkCmdSetFrontFace", "vkCmdSetFrontFaceEXT"));
    CmdSetDepthTestEnable =
        reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(load("vkCmdSetDepthTestEnable", "vkCmdSetDepthTestEnableEXT"));
    CmdSetDepthWriteEnable =
        reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(load("vkCmdSetDepthWriteEnable", "vkCmdSetDepthWriteEnableEXT"));
    CmdSetDepthCompareOp =
        reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(load("vkCmdSetDepthCompareOp", "vkCmdSetDepthCompareOpEXT"));
}

// RenderPass generation tracking

uint64_t DeviceData::RenderPassGeneration(VkRenderPass render_pass) {
    std::unique_lock<std::mutex> lock(render_pass_mutex);
    auto it = render_pass_generation.find(render_pass);
    if (it != render_pass_generation.end()) {
        return it->second;
    }
    uint64_t generation = next_render_pass_generation++;
    render_pass_generation.emplace(render_pass, generation);
    return generation;
}

void DeviceData::ForgetRenderPass(VkRenderPass render_pass) {
    std::unique_lock<std::mutex> lock(render_pass_mutex);
    render_pass_generation.erase(render_pass);
    render_pass_view_masks.erase(render_pass);
    render_pass_color_attachments.erase(render_pass);
}

void DeviceData::SetRenderPassViewMasks(VkRenderPass render_pass, std::vector<uint32_t> masks) {
    std::unique_lock<std::mutex> lock(render_pass_mutex);
    render_pass_view_masks[render_pass] = std::move(masks);
}

uint32_t DeviceData::RenderPassViewMask(VkRenderPass render_pass, uint32_t subpass) {
    std::unique_lock<std::mutex> lock(render_pass_mutex);
    auto it = render_pass_view_masks.find(render_pass);
    return it != render_pass_view_masks.end() && subpass < it->second.size() ? it->second[subpass] : 0;
}

void DeviceData::SetRenderPassColorAttachments(VkRenderPass render_pass, std::vector<bool> uses_color) {
    std::unique_lock<std::mutex> lock(render_pass_mutex);
    render_pass_color_attachments[render_pass] = std::move(uses_color);
}

bool DeviceData::RenderPassUsesColorAttachment(VkRenderPass render_pass, uint32_t subpass) {
    std::unique_lock<std::mutex> lock(render_pass_mutex);
    auto it = render_pass_color_attachments.find(render_pass);
    // Unknown render passes (including host-test fixtures) retain the conservative original state.
    return it == render_pass_color_attachments.end() || subpass >= it->second.size() || it->second[subpass];
}

uint64_t DeviceData::ShaderModuleGeneration(VkShaderModule shader_module) {
    std::unique_lock<std::mutex> lock(shader_module_mutex);
    auto it = shader_module_generation.find(shader_module);
    if (it != shader_module_generation.end()) return it->second;
    uint64_t generation = next_shader_module_generation++;
    shader_module_generation.emplace(shader_module, generation);
    return generation;
}

void DeviceData::ForgetShaderModule(VkShaderModule shader_module) {
    std::unique_lock<std::mutex> lock(shader_module_mutex);
    shader_module_generation.erase(shader_module);
}

uint64_t DeviceData::PipelineLayoutGeneration(VkPipelineLayout layout) {
    std::unique_lock<std::mutex> lock(pipeline_layout_mutex);
    auto it = pipeline_layout_generation.find(layout);
    if (it != pipeline_layout_generation.end()) return it->second;
    uint64_t generation = next_pipeline_layout_generation++;
    pipeline_layout_generation.emplace(layout, generation);
    return generation;
}

void DeviceData::ForgetPipelineLayout(VkPipelineLayout layout) {
    std::unique_lock<std::mutex> lock(pipeline_layout_mutex);
    pipeline_layout_generation.erase(layout);
}

// Single-flight library cache

VkPipeline LibraryCache::GetOrBuild(const std::string& key, DeviceData& device_data, const void* context, Builder builder,
                                    bool* was_built_out) {
    if (was_built_out) *was_built_out = false;

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            entries_.emplace(key, Entry{State::kBuilding, VK_NULL_HANDLE});
            lock.unlock();

            // Expensive work happens with the lock released so other keys can make progress.
            VkPipeline handle = builder(device_data, context);

            lock.lock();
            auto& entry = entries_[key];
            entry.handle = handle;
            entry.state = (handle != VK_NULL_HANDLE) ? State::kReady : State::kFailed;
            cv_.notify_all();
            if (was_built_out) *was_built_out = true;
            return handle;
        }
        if (it->second.state == State::kBuilding) {
            cv_.wait(lock);  // Woken on ready or failure; re-check the entry.
            continue;
        }
        // Ready or Failed: Failed carries VK_NULL_HANDLE, which the caller treats as a build failure.
        return it->second.handle;
    }
}

void LibraryCache::DestroyAll(DeviceData& device_data) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto& kv : entries_) {
        if (kv.second.handle != VK_NULL_HANDLE) {
            device_data.vtable.DestroyPipeline(device_data.device, kv.second.handle, nullptr);
        }
    }
    entries_.clear();
}

// Conversion: eligibility, promotion, and library building

namespace {

constexpr VkShaderStageFlags kPreRasterStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                                                VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

#include "dynamic_state_extensions.h"

// Everything the four builders need. Lives on the stack of the converting CreateGraphicsPipelines
// call; the single-flight cache only invokes a builder while that call is in progress.
struct LibBuildCtx {
    const VkGraphicsPipelineCreateInfo* ci = nullptr;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

    VkPipelineRasterizationStateCreateInfo raster_normalized{};
    VkPipelineDepthStencilStateCreateInfo depth_normalized{};
    bool has_depth = false;

    std::vector<VkPipelineShaderStageCreateInfo> pre_raster_stages;
    std::vector<VkPipelineShaderStageCreateInfo> fragment_stages;

    std::vector<VkDynamicState> vertex_input_dynamic;
    std::vector<VkDynamicState> pre_raster_dynamic;
    std::vector<VkDynamicState> fragment_shader_dynamic;
    std::vector<VkDynamicState> fragment_output_dynamic;
    LibraryExtensionChain vertex_input_extensions;
    LibraryExtensionChain pre_raster_extensions;
    LibraryExtensionChain fragment_shader_extensions;
    LibraryExtensionChain fragment_output_extensions;
    LibraryExtensionChain link_extensions;
};

bool SubStateChainSupported(const void* state, const VkGraphicsPipelineCreateInfo& ci) {
    // All of the VkPipeline*StateCreateInfo structs begin with { sType, pNext }.
    if (!state) return true;
    return SupportedExtensionChain(reinterpret_cast<const VkBaseInStructure*>(state)->pNext, false, ci);
}

// Returns nullptr when convertible, otherwise a short reason for diagnostics.
const char* ConvertBlocker(const VkGraphicsPipelineCreateInfo& ci, DeviceData* device_data = nullptr) {
    if (!SupportedExtensionChain(ci.pNext, true, ci)) return "unsupported top-level pNext";
    if (device_data && device_data->multiview_instance_input_workaround) {
        uint32_t view_mask = 0;
        if (ci.renderPass != VK_NULL_HANDLE) {
            view_mask = device_data->RenderPassViewMask(ci.renderPass, ci.subpass);
        } else {
            for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext)
                if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
                    view_mask = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(node)->viewMask;
        }
        if (view_mask) {
            bool mesh = false;
            for (uint32_t i = 0; i < ci.stageCount; ++i) mesh |= ci.pStages[i].stage == VK_SHADER_STAGE_MESH_BIT_EXT;
            if (!mesh) {
                if (HasDynamicState(ci, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT))
                    return "driver workaround: multiview dynamic vertex input";
                if (ci.pVertexInputState)
                    for (uint32_t i = 0; i < ci.pVertexInputState->vertexBindingDescriptionCount; ++i)
                        if (ci.pVertexInputState->pVertexBindingDescriptions[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
                            return "driver workaround: multiview instance input";
            }
        }
    }
    const auto flags = EffectivePipelineFlags(ci);
    if (flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) return "LIBRARY flag";
    if (flags & (VK_PIPELINE_CREATE_2_DERIVATIVE_BIT | VK_PIPELINE_CREATE_2_ALLOW_DERIVATIVES_BIT)) return "derivative flag";
    if (flags & (VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR))
        return "capture flag";
    // Catch-all for the above and anything else we cannot carry onto the libraries and the link.
    if (flags & ~kConvertiblePipelineFlags) return "unhandled create flag";

    // We rewrite/key the rasterization and depth/stencil structs, and we partition the remaining
    // structs across libraries by pointer. Known dynamic-state extension structs are keyed by
    // value (including deep arrays); unknown extension state still takes the native path.
    if (!SubStateChainSupported(ci.pRasterizationState, ci)) return "unsupported rasterization pNext";
    const bool rasterization = RasterizationEnabled(ci);
    if (rasterization && !SubStateChainSupported(ci.pDepthStencilState, ci)) return "unsupported depth/stencil pNext";
    const bool mesh = UsesMeshShader(ci);
    if (!mesh && !HasDynamicState(ci, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) && !SubStateChainSupported(ci.pVertexInputState, ci))
        return "unsupported vertex-input pNext";
    if (!mesh && !SubStateChainSupported(ci.pInputAssemblyState, ci)) return "unsupported input-assembly pNext";
    if (rasterization && !SubStateChainSupported(ci.pViewportState, ci)) return "unsupported viewport pNext";
    if (HasTessellation(ci) && !SubStateChainSupported(ci.pTessellationState, ci)) return "unsupported tessellation pNext";
    if (rasterization && !SubStateChainSupported(ci.pMultisampleState, ci)) return "unsupported multisample pNext";
    if (rasterization && !SubStateChainSupported(FragmentOutputColorBlendState(ci, device_data), ci))
        return "unsupported color-blend pNext";

    // Module lifetimes or inline SPIR-V bytes identify code; stage extension values identify variants.
    for (uint32_t i = 0; i < ci.stageCount; ++i) {
        const VkPipelineShaderStageCreateInfo& stage = ci.pStages[i];
        if (!rasterization && stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) continue;
        // Monolithic shaders need not agree on interpolation decorations. Without this property,
        // separate GPL shader libraries impose a stricter interface contract that we cannot verify
        // from opaque shader-module handles. Keep those pipelines native instead of attempting an
        // invalid link. Static discard and pipelines without a fragment shader have no such interface.
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT && device_data && !device_data->caps.gpl_independent_interp)
            return "independent interpolation decoration unsupported";
        if (!SupportedStageChain(stage)) return "unsupported shader stage/code";
        if (stage.pName == nullptr) return "null stage pName";
    }

    // Every application dynamic state must be classifiable so we can place it on the right library.
    if (ci.pDynamicState) {
        for (uint32_t i = 0; i < ci.pDynamicState->dynamicStateCount; ++i) {
            if (ClassifyDynamicState(ci.pDynamicState->pDynamicStates[i]) == GplSubset::kUnknown)
                return "unclassifiable dynamic state";
        }
    }
    return nullptr;
}

StateMask ComputePromotedMask(const VkGraphicsPipelineCreateInfo& ci, const DeviceData& device_data) {
    if (device_data.tier != Tier::kDynamize || !RasterizationEnabled(ci)) return 0;

    // States the application already drives dynamically must stay dynamic and untouched by us.
    StateMask originally_static = kPromotable;
    if (ci.pDynamicState) {
        for (uint32_t i = 0; i < ci.pDynamicState->dynamicStateCount; ++i) {
            originally_static &= ~StateBitForDynamicState(ci.pDynamicState->pDynamicStates[i]);
        }
    }

    // Applicability depends only on the presence of the owning struct: cull/front-face
    // live in rasterization (always present here), the depth trio needs pDepthStencilState.
    StateMask applicable = kPreRasterPromotable;
    if (ci.pDepthStencilState != nullptr) {
        applicable |= kFragmentShaderPromotable;
    }

    return originally_static & applicable;
}

void CaptureOriginalValues(const VkGraphicsPipelineCreateInfo& ci, OverrideVals* out) {
    if (!RasterizationEnabled(ci)) return;
    if (ci.pRasterizationState) {
        out->cullMode = ci.pRasterizationState->cullMode;
        out->frontFace = ci.pRasterizationState->frontFace;
    }
    if (ci.pDepthStencilState) {
        out->depthTestEnable = ci.pDepthStencilState->depthTestEnable;
        out->depthWriteEnable = ci.pDepthStencilState->depthWriteEnable;
        out->depthCompareOp = ci.pDepthStencilState->depthCompareOp;
    }
}

void SortUnique(std::vector<VkDynamicState>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Partition the application's dynamic states across libraries and add our promoted states to the
// libraries that own them.
void BuildDynamicStateBuckets(const VkGraphicsPipelineCreateInfo& ci, StateMask promoted, LibBuildCtx* ctx) {
    for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext) {
        GplSubset subsets = TopLevelExtensionSubset(node->sType);
        size_t size = TopLevelExtensionSize(node->sType);
        if (ContainsSubset(subsets, GplSubset::kVertexInput)) ctx->vertex_input_extensions.Prepend(node, size);
        if (ContainsSubset(subsets, GplSubset::kPreRaster)) ctx->pre_raster_extensions.Prepend(node, size);
        if (ContainsSubset(subsets, GplSubset::kFragmentShader)) ctx->fragment_shader_extensions.Prepend(node, size);
        if (ContainsSubset(subsets, GplSubset::kFragmentOutput))
            ctx->fragment_output_extensions.Prepend(node, size, ci.renderPass == VK_NULL_HANDLE && RasterizationEnabled(ci));
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) ctx->link_extensions.Prepend(node, size);
    }
    if (ci.pDynamicState) {
        for (uint32_t i = 0; i < ci.pDynamicState->dynamicStateCount; ++i) {
            VkDynamicState state = ci.pDynamicState->pDynamicStates[i];
            GplSubset subsets = ClassifyDynamicState(state);
            if (ContainsSubset(subsets, GplSubset::kVertexInput)) ctx->vertex_input_dynamic.push_back(state);
            if (ContainsSubset(subsets, GplSubset::kPreRaster)) ctx->pre_raster_dynamic.push_back(state);
            if (ContainsSubset(subsets, GplSubset::kFragmentShader) &&
                (subsets != GplSubset::kFragmentShaderAndOutput || FragmentShaderUsesMultisampling(ci)))
                ctx->fragment_shader_dynamic.push_back(state);
            if (ContainsSubset(subsets, GplSubset::kFragmentOutput)) ctx->fragment_output_dynamic.push_back(state);
        }
    }
    ForEachStateBit(promoted & kPreRasterPromotable,
                    [&](StateBit bit) { ctx->pre_raster_dynamic.push_back(DynamicStateForBit(bit)); });
    ForEachStateBit(promoted & kFragmentShaderPromotable,
                    [&](StateBit bit) { ctx->fragment_shader_dynamic.push_back(DynamicStateForBit(bit)); });

    SortUnique(ctx->vertex_input_dynamic);
    SortUnique(ctx->pre_raster_dynamic);
    SortUnique(ctx->fragment_shader_dynamic);
    SortUnique(ctx->fragment_output_dynamic);
}

// --- key serialization -------------------------------------------------------------------------

template <typename T>
void AppendPod(KeyBuilder& k, const T& value) {
    k.AppendBytes(&value, sizeof(T));
}

void AppendString(KeyBuilder& k, const char* s) {
    if (!s) {
        k.AppendU32(0xffffffffu);
        return;
    }
    uint32_t len = static_cast<uint32_t>(std::strlen(s));
    k.AppendU32(len);
    k.AppendBytes(s, len);
}

void AppendDynamicStates(KeyBuilder& k, const std::vector<VkDynamicState>& states) {
    k.AppendU32(static_cast<uint32_t>(states.size()));
    for (VkDynamicState s : states) {
        k.AppendU32(static_cast<uint32_t>(s));
    }
}

void AppendStage(KeyBuilder& k, const VkPipelineShaderStageCreateInfo& stage, DeviceData& device_data) {
    k.AppendU32(stage.flags);
    k.AppendU32(stage.stage);
    const auto* code = InlineShader(stage);
    k.AppendU32(code ? 1 : 0);
    if (code) {
        k.AppendU32(code->flags);
        k.AppendU64(code->codeSize);
        k.AppendBytes(code->pCode, code->codeSize);
    } else {
        k.AppendU64(device_data.ShaderModuleGeneration(stage.module));
    }
    AppendStageExtensions(k, stage);
    AppendString(k, stage.pName);
    if (stage.pSpecializationInfo) {
        const VkSpecializationInfo& spec = *stage.pSpecializationInfo;
        k.AppendU32(spec.mapEntryCount);
        for (uint32_t i = 0; i < spec.mapEntryCount; ++i) {
            AppendPod(k, spec.pMapEntries[i]);
        }
        k.AppendU32(static_cast<uint32_t>(spec.dataSize));
        k.AppendBytes(spec.pData, spec.dataSize);
    } else {
        k.AppendU32(0xffffffffu);
    }
}

void AppendMultisampleState(KeyBuilder& k, const VkPipelineMultisampleStateCreateInfo* ms, const VkGraphicsPipelineCreateInfo& ci) {
    if (!ms) {
        k.AppendU32(0xffffffffu);
        return;
    }
    k.AppendU32(ms->flags);
    bool dynamic_mask = HasDynamicState(ci, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT);
    // Even with dynamic sample count, this field still sizes a static sample mask (spec).
    bool mask_uses_count = !dynamic_mask && ms->pSampleMask;
    k.AppendU32(HasDynamicState(ci, VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT) && !mask_uses_count ? 0 : ms->rasterizationSamples);
    k.AppendU32(ms->sampleShadingEnable);
    AppendPod(k, ms->minSampleShading);
    if (!dynamic_mask && ms->pSampleMask) {
        uint32_t words = (ms->rasterizationSamples + 31u) / 32u;
        if (words == 0) words = 1;
        for (uint32_t i = 0; i < words; ++i) AppendPod(k, ms->pSampleMask[i]);
    } else {
        k.AppendU32(0xffffffffu);
    }
    k.AppendU32(HasDynamicState(ci, VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT) ? 0 : ms->alphaToCoverageEnable);
    k.AppendU32(HasDynamicState(ci, VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT) ? 0 : ms->alphaToOneEnable);
    AppendExtensionChain(k, ms->pNext, ci);
}

std::string MakeVertexInputKey(const LibBuildCtx& ctx) {
    KeyBuilder k;
    k.AppendU32(0);  // subset tag
    k.AppendU64(LibraryFlagsForSubset(*ctx.ci, GplSubset::kVertexInput));
    const VkPipelineVertexInputStateCreateInfo* vi = ctx.ci->pVertexInputState;
    if (vi && !HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) {
        k.AppendU32(vi->flags);
        k.AppendU32(vi->vertexBindingDescriptionCount);
        for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; ++i) {
            auto binding = vi->pVertexBindingDescriptions[i];
            if (HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)) binding.stride = 0;
            AppendPod(k, binding);
        }
        k.AppendU32(vi->vertexAttributeDescriptionCount);
        for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; ++i) AppendPod(k, vi->pVertexAttributeDescriptions[i]);
        AppendExtensionChain(k, vi->pNext, *ctx.ci);
    } else {
        k.AppendU32(0xffffffffu);
    }
    const VkPipelineInputAssemblyStateCreateInfo* ia = ctx.ci->pInputAssemblyState;
    if (ia) {
        k.AppendU32(ia->flags);
        k.AppendU32(ia->topology);
        k.AppendU32(ia->primitiveRestartEnable);
        AppendExtensionChain(k, ia->pNext, *ctx.ci);
    } else {
        k.AppendU32(0xffffffffu);
    }
    AppendExtensionChain(k, ctx.vertex_input_extensions.head, *ctx.ci, GplSubset::kVertexInput);
    AppendDynamicStates(k, ctx.vertex_input_dynamic);
    return k.Get();
}

std::string MakePreRasterKey(const LibBuildCtx& ctx, DeviceData& device_data) {
    KeyBuilder k;
    k.AppendU32(1);  // subset tag
    k.AppendU64(LibraryFlagsForSubset(*ctx.ci, GplSubset::kPreRaster));
    k.AppendU32(static_cast<uint32_t>(ctx.pre_raster_stages.size()));
    for (const auto& stage : ctx.pre_raster_stages) AppendStage(k, stage, device_data);

    const VkPipelineViewportStateCreateInfo* vp = RasterizationEnabled(*ctx.ci) ? ctx.ci->pViewportState : nullptr;
    if (vp) {
        k.AppendU32(vp->flags);
        bool viewport_count_dynamic = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
        bool scissor_count_dynamic = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
        k.AppendU32(viewport_count_dynamic ? 0 : vp->viewportCount);
        k.AppendU32(scissor_count_dynamic ? 0 : vp->scissorCount);
        if (!viewport_count_dynamic && !HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_VIEWPORT) && vp->pViewports)
            for (uint32_t i = 0; i < vp->viewportCount; ++i) AppendPod(k, vp->pViewports[i]);
        if (!scissor_count_dynamic && !HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_SCISSOR) && vp->pScissors)
            for (uint32_t i = 0; i < vp->scissorCount; ++i) AppendPod(k, vp->pScissors[i]);
        AppendExtensionChain(k, vp->pNext, *ctx.ci);
    } else {
        k.AppendU32(0xffffffffu);
    }

    const VkPipelineTessellationStateCreateInfo* ts = HasTessellation(*ctx.ci) ? ctx.ci->pTessellationState : nullptr;
    if (ts) {
        k.AppendU32(ts->flags);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT) ? 0 : ts->patchControlPoints);
        AppendExtensionChain(k, ts->pNext, *ctx.ci);
    } else {
        k.AppendU32(0xffffffffu);
    }

    // Rasterization is already normalized (promoted cull/front-face canonicalized).
    const VkPipelineRasterizationStateCreateInfo& rs = ctx.raster_normalized;
    k.AppendU32(rs.flags);
    k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT) ? 0 : rs.depthClampEnable);
    k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) ? 0 : rs.rasterizerDiscardEnable);
    k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_POLYGON_MODE_EXT) ? 0 : rs.polygonMode);
    k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_CULL_MODE) ? 0 : rs.cullMode);
    k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_FRONT_FACE) ? 0 : rs.frontFace);
    k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE) ? 0 : rs.depthBiasEnable);
    bool dynamic_bias = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_BIAS);
    AppendPod(k, dynamic_bias ? 0.0f : rs.depthBiasConstantFactor);
    AppendPod(k, dynamic_bias ? 0.0f : rs.depthBiasClamp);
    AppendPod(k, dynamic_bias ? 0.0f : rs.depthBiasSlopeFactor);
    AppendPod(k, HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_LINE_WIDTH) ? 0.0f : rs.lineWidth);
    AppendExtensionChain(k, rs.pNext, *ctx.ci);
    AppendExtensionChain(k, ctx.pre_raster_extensions.head, *ctx.ci, GplSubset::kPreRaster);

    k.AppendU64(device_data.PipelineLayoutGeneration(ctx.ci->layout));
    k.AppendU64(device_data.RenderPassGeneration(ctx.ci->renderPass));
    k.AppendU32(ctx.ci->subpass);
    AppendDynamicStates(k, ctx.pre_raster_dynamic);
    return k.Get();
}

std::string MakeFragmentShaderKey(const LibBuildCtx& ctx, DeviceData& device_data) {
    KeyBuilder k;
    k.AppendU32(2);  // subset tag
    k.AppendU64(LibraryFlagsForSubset(*ctx.ci, GplSubset::kFragmentShader));
    k.AppendU32(static_cast<uint32_t>(ctx.fragment_stages.size()));
    for (const auto& stage : ctx.fragment_stages) AppendStage(k, stage, device_data);

    if (ctx.has_depth) {
        const VkPipelineDepthStencilStateCreateInfo& ds = ctx.depth_normalized;
        k.AppendU32(ds.flags);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) ? 0 : ds.depthTestEnable);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE) ? 0 : ds.depthWriteEnable);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP) ? 0 : ds.depthCompareOp);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE) ? 0 : ds.depthBoundsTestEnable);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE) ? 0 : ds.stencilTestEnable);
        for (auto stencil : {ds.front, ds.back}) {
            if (HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_STENCIL_OP)) {
                stencil.failOp = stencil.passOp = stencil.depthFailOp = VK_STENCIL_OP_KEEP;
                stencil.compareOp = VK_COMPARE_OP_NEVER;
            }
            if (HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) stencil.compareMask = 0;
            if (HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) stencil.writeMask = 0;
            if (HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_STENCIL_REFERENCE)) stencil.reference = 0;
            AppendPod(k, stencil);
        }
        bool dynamic_bounds = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_DEPTH_BOUNDS);
        AppendPod(k, dynamic_bounds ? 0.0f : ds.minDepthBounds);
        AppendPod(k, dynamic_bounds ? 0.0f : ds.maxDepthBounds);
        AppendExtensionChain(k, ds.pNext, *ctx.ci);
    } else {
        k.AppendU32(0xffffffffu);
    }

    AppendMultisampleState(k, FragmentShaderUsesMultisampling(*ctx.ci) ? ctx.ci->pMultisampleState : nullptr, *ctx.ci);
    AppendExtensionChain(k, ctx.fragment_shader_extensions.head, *ctx.ci, GplSubset::kFragmentShader);
    k.AppendU64(device_data.PipelineLayoutGeneration(ctx.ci->layout));
    k.AppendU64(device_data.RenderPassGeneration(ctx.ci->renderPass));
    k.AppendU32(ctx.ci->subpass);
    AppendDynamicStates(k, ctx.fragment_shader_dynamic);
    return k.Get();
}

std::string MakeFragmentOutputKey(const LibBuildCtx& ctx, DeviceData& device_data) {
    KeyBuilder k;
    k.AppendU32(3);  // subset tag
    k.AppendU64(LibraryFlagsForSubset(*ctx.ci, GplSubset::kFragmentOutput));
    const VkPipelineColorBlendStateCreateInfo* cb = FragmentOutputColorBlendState(*ctx.ci, &device_data);
    if (cb) {
        k.AppendU32(cb->flags);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT) ? 0 : cb->logicOpEnable);
        k.AppendU32(HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_LOGIC_OP_EXT) ? 0 : cb->logicOp);
        k.AppendU32(cb->attachmentCount);
        bool dynamic_enable = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
        bool dynamic_equation = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
        bool dynamic_write_mask = HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
        if (!dynamic_enable || !dynamic_equation || !dynamic_write_mask) {
            for (uint32_t i = 0; i < cb->attachmentCount; ++i) {
                const auto& attachment = cb->pAttachments[i];
                if (!dynamic_enable) k.AppendU32(attachment.blendEnable);
                if (!dynamic_equation) {
                    k.AppendU32(attachment.srcColorBlendFactor);
                    k.AppendU32(attachment.dstColorBlendFactor);
                    k.AppendU32(attachment.colorBlendOp);
                    k.AppendU32(attachment.srcAlphaBlendFactor);
                    k.AppendU32(attachment.dstAlphaBlendFactor);
                    k.AppendU32(attachment.alphaBlendOp);
                }
                if (!dynamic_write_mask) k.AppendU32(attachment.colorWriteMask);
            }
        }
        if (!HasDynamicState(*ctx.ci, VK_DYNAMIC_STATE_BLEND_CONSTANTS))
            for (int i = 0; i < 4; ++i) AppendPod(k, cb->blendConstants[i]);
        AppendExtensionChain(k, cb->pNext, *ctx.ci);
    } else {
        k.AppendU32(0xffffffffu);
    }
    AppendMultisampleState(k, ctx.ci->pMultisampleState, *ctx.ci);
    AppendExtensionChain(k, ctx.fragment_output_extensions.head, *ctx.ci, GplSubset::kFragmentOutput);
    k.AppendU64(device_data.RenderPassGeneration(ctx.ci->renderPass));
    k.AppendU32(ctx.ci->subpass);
    AppendDynamicStates(k, ctx.fragment_output_dynamic);
    return k.Get();
}

// --- library builders (match LibraryCache::Builder) ---------------------------------------------

// Shared setup for library creation; subset builders below only select their owned state.
VkPipeline CreateLibraryPipeline(DeviceData& device_data, const LibBuildCtx& ctx, GplSubset subset,
                                 VkGraphicsPipelineCreateInfo lib, const std::vector<VkDynamicState>& dynamic_states,
                                 const LibraryExtensionChain& extensions) {
    VkGraphicsPipelineLibraryCreateInfoEXT gpl{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, extensions.head,
                                               static_cast<VkGraphicsPipelineLibraryFlagsEXT>(subset)};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
                                             static_cast<uint32_t>(dynamic_states.size()), dynamic_states.data()};
    lib.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    lib.pNext = &gpl;
    lib.pDynamicState = dynamic_states.empty() ? nullptr : &dynamic;
    lib.basePipelineIndex = -1;
    VkPipelineCreateFlags2CreateInfo flags2{};
    ApplyPipelineFlags(lib, VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR | LibraryFlagsForSubset(*ctx.ci, subset), *ctx.ci, flags2);

    // Cached libraries outlive application pipeline allocators and use the default allocator,
    // matching LibraryCache::DestroyAll. The final executable uses the application's allocator.
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result =
        device_data.vtable.CreateGraphicsPipelines(device_data.device, ctx.pipeline_cache, 1, &lib, nullptr, &pipeline);
    return result == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

VkPipeline BuildVertexInputLibrary(DeviceData& device_data, const void* context) {
    const auto& ctx = *static_cast<const LibBuildCtx*>(context);
    VkGraphicsPipelineCreateInfo lib{};
    lib.pVertexInputState = ctx.ci->pVertexInputState;
    lib.pInputAssemblyState = ctx.ci->pInputAssemblyState;
    return CreateLibraryPipeline(device_data, ctx, GplSubset::kVertexInput, lib, ctx.vertex_input_dynamic,
                                 ctx.vertex_input_extensions);
}

VkPipeline BuildPreRasterLibrary(DeviceData& device_data, const void* context) {
    const auto& ctx = *static_cast<const LibBuildCtx*>(context);
    VkGraphicsPipelineCreateInfo lib{};
    lib.stageCount = static_cast<uint32_t>(ctx.pre_raster_stages.size());
    lib.pStages = ctx.pre_raster_stages.data();
    lib.pViewportState = RasterizationEnabled(*ctx.ci) ? ctx.ci->pViewportState : nullptr;
    lib.pRasterizationState = &ctx.raster_normalized;
    lib.pTessellationState = HasTessellation(*ctx.ci) ? ctx.ci->pTessellationState : nullptr;
    lib.layout = ctx.ci->layout;
    lib.renderPass = ctx.ci->renderPass;
    lib.subpass = ctx.ci->subpass;
    return CreateLibraryPipeline(device_data, ctx, GplSubset::kPreRaster, lib, ctx.pre_raster_dynamic, ctx.pre_raster_extensions);
}

VkPipeline BuildFragmentShaderLibrary(DeviceData& device_data, const void* context) {
    const auto& ctx = *static_cast<const LibBuildCtx*>(context);
    VkGraphicsPipelineCreateInfo lib{};
    lib.stageCount = static_cast<uint32_t>(ctx.fragment_stages.size());
    lib.pStages = ctx.fragment_stages.data();
    lib.pMultisampleState = FragmentShaderUsesMultisampling(*ctx.ci) ? ctx.ci->pMultisampleState : nullptr;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1.0f;
    lib.pRasterizationState = &raster;
    // A standalone dynamic-rendering FS library cannot infer the eventual attachment formats and
    // must provide disabled depth/stencil state when the monolithic pipeline omitted it (09035).
    VkPipelineDepthStencilStateCreateInfo disabled_depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    lib.pDepthStencilState =
        ctx.has_depth ? &ctx.depth_normalized : (ctx.ci->renderPass == VK_NULL_HANDLE ? &disabled_depth : nullptr);
    lib.layout = ctx.ci->layout;
    lib.renderPass = ctx.ci->renderPass;
    lib.subpass = ctx.ci->subpass;
    return CreateLibraryPipeline(device_data, ctx, GplSubset::kFragmentShader, lib, ctx.fragment_shader_dynamic,
                                 ctx.fragment_shader_extensions);
}

VkPipeline BuildFragmentOutputLibrary(DeviceData& device_data, const void* context) {
    const auto& ctx = *static_cast<const LibBuildCtx*>(context);
    VkGraphicsPipelineCreateInfo lib{};
    lib.pMultisampleState = ctx.ci->pMultisampleState;
    lib.pColorBlendState = FragmentOutputColorBlendState(*ctx.ci, &device_data);
    lib.renderPass = ctx.ci->renderPass;
    lib.subpass = ctx.ci->subpass;
    return CreateLibraryPipeline(device_data, ctx, GplSubset::kFragmentOutput, lib, ctx.fragment_output_dynamic,
                                 ctx.fragment_output_extensions);
}

// Stop conversion when the warmup shows too little sharing of expensive shader libraries.
constexpr uint64_t kGateWarmup = 256;  // converted pipelines before the gate may trip
constexpr uint64_t kGateRatioNum = 4;  // trip when expensive_libs/converted >= 4/5 (little sharing)
constexpr uint64_t kGateRatioDen = 5;

void MaybeTripVariantGate(DeviceData& device_data) {
    if (device_data.gate_downgraded.load(std::memory_order_relaxed)) return;
    uint64_t converted = device_data.converted_pipeline_count.load(std::memory_order_relaxed);
    if (converted < kGateWarmup) return;
    uint64_t expensive = device_data.expensive_library_count.load(std::memory_order_relaxed);
    if (expensive * kGateRatioDen >= converted * kGateRatioNum) {
        bool expected = false;
        if (device_data.gate_downgraded.compare_exchange_strong(expected, true)) {
            LayerMessage(
                "[%s] variant distribution near 1 (%llu expensive libraries for %llu converted pipelines); "
                "downgrading to passthrough.\n",
                kLayerName, static_cast<unsigned long long>(expensive), static_cast<unsigned long long>(converted));
        }
    }
}

// Build the four libraries, link them, and (when applicable) attach the bind-time record. Returns
// VK_NULL_HANDLE if any step fails, so the caller can fall back to native monolithic creation.
VkPipeline ConvertOnePipeline(DeviceData& device_data, const VkGraphicsPipelineCreateInfo& ci, VkPipelineCache pipeline_cache,
                              const VkAllocationCallbacks* allocator, StateMask promoted, const OverrideVals& originals) {
    LibBuildCtx ctx;
    ctx.ci = &ci;
    ctx.pipeline_cache = pipeline_cache;
    const bool rasterization = RasterizationEnabled(ci);

    // EDS3 permits null static state structs when all their fields are dynamic.
    ctx.raster_normalized.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ctx.raster_normalized.lineWidth = 1.0f;
    if (ci.pRasterizationState) ctx.raster_normalized = *ci.pRasterizationState;
    if (promoted & kCullMode) ctx.raster_normalized.cullMode = VK_CULL_MODE_NONE;
    if (promoted & kFrontFace) ctx.raster_normalized.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    ctx.has_depth = rasterization && (ci.pDepthStencilState != nullptr);
    if (ctx.has_depth) {
        ctx.depth_normalized = *ci.pDepthStencilState;
        if (promoted & kDepthTestEnable) ctx.depth_normalized.depthTestEnable = VK_FALSE;
        if (promoted & kDepthWriteEnable) ctx.depth_normalized.depthWriteEnable = VK_FALSE;
        if (promoted & kDepthCompareOp) ctx.depth_normalized.depthCompareOp = VK_COMPARE_OP_NEVER;
    }

    bool uses_mesh_shader = false;
    for (uint32_t i = 0; i < ci.stageCount; ++i) {
        if (ci.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            if (rasterization) ctx.fragment_stages.push_back(ci.pStages[i]);
        } else if (ci.pStages[i].stage & kPreRasterStages) {
            ctx.pre_raster_stages.push_back(ci.pStages[i]);
            uses_mesh_shader |= ci.pStages[i].stage == VK_SHADER_STAGE_MESH_BIT_EXT;
        } else {
            return VK_NULL_HANDLE;  // Not a supported graphics shader stage.
        }
    }

    BuildDynamicStateBuckets(ci, promoted, &ctx);

    bool built = false;
    // Mesh pipelines do not consume vertex input or input assembly state. A standalone vertex-input
    // library would still require those states, so omit that subset entirely (VUID flags-08898).
    VkPipeline vertex_input = VK_NULL_HANDLE;
    if (!uses_mesh_shader) {
        vertex_input = device_data.cache.GetOrBuild(MakeVertexInputKey(ctx), device_data, &ctx, BuildVertexInputLibrary, &built);
        if (vertex_input == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    }

    VkPipeline pre_raster =
        device_data.cache.GetOrBuild(MakePreRasterKey(ctx, device_data), device_data, &ctx, BuildPreRasterLibrary, &built);
    if (pre_raster == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    if (built) device_data.expensive_library_count.fetch_add(1, std::memory_order_relaxed);

    VkPipeline fragment_shader = VK_NULL_HANDLE;
    VkPipeline fragment_output = VK_NULL_HANDLE;
    if (rasterization) {
        fragment_shader = device_data.cache.GetOrBuild(MakeFragmentShaderKey(ctx, device_data), device_data, &ctx,
                                                       BuildFragmentShaderLibrary, &built);
        if (fragment_shader == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        if (built) device_data.expensive_library_count.fetch_add(1, std::memory_order_relaxed);

        fragment_output = device_data.cache.GetOrBuild(MakeFragmentOutputKey(ctx, device_data), device_data, &ctx,
                                                       BuildFragmentOutputLibrary, &built);
        if (fragment_output == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    }

    VkPipeline libraries[4] = {pre_raster};
    uint32_t library_count = 1;
    if (vertex_input) libraries[library_count++] = vertex_input;
    if (fragment_shader) libraries[library_count++] = fragment_shader;
    if (fragment_output) libraries[library_count++] = fragment_output;
    VkPipelineLibraryCreateInfoKHR link{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, nullptr, library_count, libraries};
    link.pNext = ctx.link_extensions.head;

    VkGraphicsPipelineCreateInfo exe{};
    exe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    exe.pNext = &link;
    VkPipelineCreateFlags2CreateInfo flags2{};
    ApplyPipelineFlags(exe, EffectivePipelineFlags(ci), ci, flags2);
    exe.layout = ci.layout;
    exe.renderPass = ci.renderPass;
    exe.subpass = ci.subpass;
    exe.basePipelineIndex = -1;

    VkPipeline executable = VK_NULL_HANDLE;
    VkResult link_result =
        device_data.vtable.CreateGraphicsPipelines(device_data.device, pipeline_cache, 1, &exe, allocator, &executable);
    if (link_result != VK_SUCCESS || executable == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    // Attach the bind-time record only when we actually dynamized something.
    if (promoted != 0) {
        ASSERT(device_data.record_slot != VK_NULL_HANDLE);
        auto* record = new PipelineRecord();
        record->promoted_mask = promoted;
        record->original_static_values = originals;
        VkResult record_result = device_data.vtable.SetPrivateData(
            device_data.device, VK_OBJECT_TYPE_PIPELINE, CastToUint64(executable), device_data.record_slot, CastToUint64(record));
        if (record_result != VK_SUCCESS) {
            device_data.vtable.DestroyPipeline(device_data.device, executable, allocator);
            delete record;
            return VK_NULL_HANDLE;
        }
    }

    device_data.converted_pipeline_count.fetch_add(1, std::memory_order_relaxed);
    MaybeTripVariantGate(device_data);
    return executable;
}

}  // namespace

// Intercepted commands

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                              uint32_t createInfoCount,
                                                              const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                              const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    DeviceData* device_data = GetDeviceData(device);
    ASSERT(device_data != nullptr);

    // Fast path: the layer is inert on this device, or the variant gate downgraded it.
    if (!device_data->ConversionEnabled()) {
        return device_data->vtable.CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
                                                           pPipelines);
    }

    // Derivative dependency indices and compile-required requests must retain native batch handling.
    // Independent entries can be converted while preserving outputs and early-return behavior.
    if (createInfoCount > 1) {
        constexpr VkPipelineCreateFlags2 native_batch_flags =
            VK_PIPELINE_CREATE_2_DERIVATIVE_BIT | VK_PIPELINE_CREATE_2_ALLOW_DERIVATIVES_BIT |
            VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT | VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR;
        for (uint32_t i = 0; i < createInfoCount; ++i) {
            if (EffectivePipelineFlags(pCreateInfos[i]) & native_batch_flags) {
                LayerDebug("[%s] CreateGraphicsPipelines: dependency/control batch passthrough (count=%u)\n", kLayerName,
                           createInfoCount);
                return device_data->vtable.CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
                                                                   pPipelines);
            }
        }
    }

    std::fill(pPipelines, pPipelines + createInfoCount, VK_NULL_HANDLE);
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[i];
        VkPipeline converted = VK_NULL_HANDLE;
        const char* blocker = device_data->ConversionEnabled() ? ConvertBlocker(ci, device_data) : "variant gate passthrough";
        if (!blocker) {
            const auto started = std::chrono::steady_clock::now();
            StateMask promoted = ComputePromotedMask(ci, *device_data);
            OverrideVals originals;
            CaptureOriginalValues(ci, &originals);
            converted = ConvertOnePipeline(*device_data, ci, pipelineCache, pAllocator, promoted, originals);
            if (converted == VK_NULL_HANDLE)
                blocker = "conversion/link failed";
            else
                WriteConversionFeedback(
                    ci,
                    static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()));
        }
        LayerDebug("[%s] CreateGraphicsPipelines pso %u/%u: %s\n", kLayerName, i + 1, createInfoCount,
                   converted != VK_NULL_HANDLE ? "converted" : blocker);

        if (converted != VK_NULL_HANDLE) {
            pPipelines[i] = converted;
            continue;
        }

        // Fallback: native monolithic creation with the original, unmodified create info.
        VkResult native = device_data->vtable.CreateGraphicsPipelines(device, pipelineCache, 1, &ci, pAllocator, &pPipelines[i]);
        if (native != VK_SUCCESS) {
            overall = native;  // report the failure; the handle is left VK_NULL_HANDLE
            if (EffectivePipelineFlags(ci) & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT) break;
        }
    }
    return overall;
}

static VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                                  VkPipeline pipeline) {
    DeviceData* device_data = GetDeviceData(commandBuffer);
    ASSERT(device_data != nullptr);

    // Always perform the real bind first.
    device_data->vtable.CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);

    if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || device_data->record_slot == VK_NULL_HANDLE) {
        return;
    }

    uint64_t raw = 0;
    device_data->vtable.GetPrivateData(device_data->device, VK_OBJECT_TYPE_PIPELINE, CastToUint64(pipeline),
                                       device_data->record_slot, &raw);
    auto* record = CastFromUint64<PipelineRecord*>(raw);
    if (!record) {
        return;  // native/fallback pipeline: nothing to restore
    }

    // Re-apply the original static values we dynamized away.
    const auto& vtable = device_data->vtable;
    const auto& vals = record->original_static_values;
    ForEachStateBit(record->promoted_mask, [&](StateBit bit) {
        switch (bit) {
            case kCullMode:
                vtable.CmdSetCullMode(commandBuffer, vals.cullMode);
                break;
            case kFrontFace:
                vtable.CmdSetFrontFace(commandBuffer, vals.frontFace);
                break;
            case kDepthTestEnable:
                vtable.CmdSetDepthTestEnable(commandBuffer, vals.depthTestEnable);
                break;
            case kDepthWriteEnable:
                vtable.CmdSetDepthWriteEnable(commandBuffer, vals.depthWriteEnable);
                break;
            case kDepthCompareOp:
                vtable.CmdSetDepthCompareOp(commandBuffer, vals.depthCompareOp);
                break;
        }
    });
}

static VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator) {
    DeviceData* device_data = GetDeviceData(device);
    ASSERT(device_data != nullptr);

    if (device_data->record_slot != VK_NULL_HANDLE && pipeline != VK_NULL_HANDLE) {
        uint64_t raw = 0;
        device_data->vtable.GetPrivateData(device, VK_OBJECT_TYPE_PIPELINE, CastToUint64(pipeline), device_data->record_slot, &raw);
        if (auto* record = CastFromUint64<PipelineRecord*>(raw)) {
            delete record;  // the GPL libraries themselves are owned by the cache, not the record
        }
    }
    device_data->vtable.DestroyPipeline(device, pipeline, pAllocator);
}

static VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceData* device_data = GetDeviceData(device);
    ASSERT(device_data != nullptr);
    if (renderPass != VK_NULL_HANDLE) {
        device_data->ForgetRenderPass(renderPass);
    }
    device_data->vtable.DestroyRenderPass(device, renderPass, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceData* data = GetDeviceData(device);
    VkResult result = data->vtable.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) {
        std::vector<bool> uses_color(pCreateInfo->subpassCount, false);
        for (uint32_t i = 0; i < pCreateInfo->subpassCount; ++i)
            for (uint32_t c = 0; c < pCreateInfo->pSubpasses[i].colorAttachmentCount; ++c)
                uses_color[i] = uses_color[i] || pCreateInfo->pSubpasses[i].pColorAttachments[c].attachment != VK_ATTACHMENT_UNUSED;
        data->SetRenderPassColorAttachments(*pRenderPass, std::move(uses_color));
    }
    if (result == VK_SUCCESS && data->multiview_instance_input_workaround) {
        std::vector<uint32_t> masks(pCreateInfo->subpassCount, 0);
        for (auto node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
                const auto& info = *reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(node);
                for (uint32_t i = 0; i < info.subpassCount && i < masks.size(); ++i) masks[i] = info.pViewMasks[i];
            }
        }
        data->SetRenderPassViewMasks(*pRenderPass, std::move(masks));
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceData* data = GetDeviceData(device);
    VkResult result = data->vtable.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) {
        std::vector<bool> uses_color(pCreateInfo->subpassCount, false);
        for (uint32_t i = 0; i < pCreateInfo->subpassCount; ++i)
            for (uint32_t c = 0; c < pCreateInfo->pSubpasses[i].colorAttachmentCount; ++c)
                uses_color[i] = uses_color[i] || pCreateInfo->pSubpasses[i].pColorAttachments[c].attachment != VK_ATTACHMENT_UNUSED;
        data->SetRenderPassColorAttachments(*pRenderPass, std::move(uses_color));
    }
    if (result == VK_SUCCESS && data->multiview_instance_input_workaround) {
        std::vector<uint32_t> masks;
        for (uint32_t i = 0; i < pCreateInfo->subpassCount; ++i) masks.push_back(pCreateInfo->pSubpasses[i].viewMask);
        data->SetRenderPassViewMasks(*pRenderPass, std::move(masks));
    }
    return result;
}

static VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                                      const VkAllocationCallbacks* pAllocator) {
    DeviceData* device_data = GetDeviceData(device);
    ASSERT(device_data != nullptr);
    if (shaderModule != VK_NULL_HANDLE) device_data->ForgetShaderModule(shaderModule);
    device_data->vtable.DestroyShaderModule(device, shaderModule, pAllocator);
}

static VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout,
                                                        const VkAllocationCallbacks* pAllocator) {
    DeviceData* data = GetDeviceData(device);
    if (pipelineLayout != VK_NULL_HANDLE) data->ForgetPipelineLayout(pipelineLayout);
    data->vtable.DestroyPipelineLayout(device, pipelineLayout, pAllocator);
}

// Capability detection and feature/extension injection

namespace {

struct ExtensionPresence {
    bool gpl = false;
    bool pipeline_library = false;
    bool extended_dynamic_state = false;
    bool private_data = false;
};

ExtensionPresence QueryDeviceExtensions(InstanceData& instance_data, VkPhysicalDevice physical_device) {
    ExtensionPresence present;
    uint32_t count = 0;
    if (instance_data.vtable.EnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return present;
    }
    std::vector<VkExtensionProperties> props(count);
    if (instance_data.vtable.EnumerateDeviceExtensionProperties(physical_device, nullptr, &count, props.data()) != VK_SUCCESS) {
        return present;
    }
    for (const auto& p : props) {
        if (strcmp(p.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) == 0)
            present.gpl = true;
        else if (strcmp(p.extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) == 0)
            present.pipeline_library = true;
        else if (strcmp(p.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0)
            present.extended_dynamic_state = true;
        else if (strcmp(p.extensionName, VK_EXT_PRIVATE_DATA_EXTENSION_NAME) == 0)
            present.private_data = true;
    }
    return present;
}

LayerCapabilities DetectCapabilities(InstanceData& instance_data, VkPhysicalDevice physical_device, uint32_t device_api_version,
                                     const ExtensionPresence& present) {
    LayerCapabilities caps;
    if (!instance_data.vtable.GetPhysicalDeviceFeatures2) {
        return caps;  // too old to probe GPL; stay at passthrough
    }

    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_feature{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, nullptr, VK_FALSE};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds_feature{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, nullptr, VK_FALSE};
    VkPhysicalDevicePrivateDataFeatures pd_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, nullptr, VK_FALSE};

    // Only chain a feature struct for an extension the device actually advertises; querying a feature
    // struct for an unsupported extension is not well defined. privateData's struct is core in 1.3.
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&features);
    auto chain = [&](auto& feature) {
        tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&feature);
        tail = reinterpret_cast<VkBaseOutStructure*>(&feature);
    };
    bool pd_queried = (device_api_version >= VK_API_VERSION_1_3) || present.private_data;
    if (present.gpl) chain(gpl_feature);
    if (present.extended_dynamic_state) chain(eds_feature);
    if (pd_queried) chain(pd_feature);
    instance_data.vtable.GetPhysicalDeviceFeatures2(physical_device, &features);

    caps.gpl = present.gpl && present.pipeline_library && gpl_feature.graphicsPipelineLibrary == VK_TRUE;

    // Extended dynamic state is always available in core 1.3; otherwise it needs the extension+feature.
    caps.extended_dynamic_state = (device_api_version >= VK_API_VERSION_1_3) ||
                                  (present.extended_dynamic_state && eds_feature.extendedDynamicState == VK_TRUE);

    // privateData must be enabled even in 1.3; the extension is only needed below 1.3.
    caps.private_data = pd_queried && (pd_feature.privateData == VK_TRUE);

    if (caps.gpl && instance_data.vtable.GetPhysicalDeviceProperties2) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT, nullptr, VK_FALSE, VK_FALSE};
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &gpl_props;
        instance_data.vtable.GetPhysicalDeviceProperties2(physical_device, &props);
        caps.gpl_independent_interp = gpl_props.graphicsPipelineLibraryIndependentInterpolationDecoration == VK_TRUE;
    }
    return caps;
}

// Whether a VkBool32 feature is already present in the application's pNext chain, and its value.
enum class FeaturePresence { kAbsent, kPresentFalse, kPresentTrue };

FeaturePresence ScanFeature(const void* pnext, VkStructureType type, size_t bool_offset) {
    for (auto chain = reinterpret_cast<const VkBaseInStructure*>(pnext); chain; chain = chain->pNext) {
        if (chain->sType == type) {
            VkBool32 value = *reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(chain) + bool_offset);
            return value == VK_TRUE ? FeaturePresence::kPresentTrue : FeaturePresence::kPresentFalse;
        }
    }
    return FeaturePresence::kAbsent;
}

}  // namespace

// Instance / device lifecycle

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    VkLayerInstanceCreateInfo* chain_info = GetChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
    ASSERT(chain_info && chain_info->u.pLayerInfo);

    PFN_vkGetInstanceProcAddr gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create_instance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;  // advance for the next layer

    VkResult result = create_instance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }

    auto* instance_data = new InstanceData();
    instance_data->instance = *pInstance;
    instance_data->api_version = (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion)
                                     ? pCreateInfo->pApplicationInfo->apiVersion
                                     : VK_API_VERSION_1_0;
    instance_data->vtable.Initialize(*pInstance, gipa);
    instance_data_map.insert_or_assign(DispatchKey(*pInstance), instance_data);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    void* key = DispatchKey(instance);
    InstanceData* instance_data = GetInstanceData(instance);
    ASSERT(instance_data != nullptr);
    instance_data->vtable.DestroyInstance(instance, pAllocator);
    instance_data_map.erase(key);
    delete instance_data;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceData* instance_data = GetInstanceData(physicalDevice);
    ASSERT(instance_data != nullptr);

    VkLayerDeviceCreateInfo* chain_info = GetChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
    ASSERT(chain_info && chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance_data->instance, "vkCreateDevice"));
    if (!create_device) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;  // advance for the next layer

    // Probe the physical device's own apiVersion (capabilities are gated on core 1.3 in places).
    uint32_t device_api_version = instance_data->api_version;
    bool multiview_instance_input_workaround = false;
    if (instance_data->vtable.GetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties properties{};
        instance_data->vtable.GetPhysicalDeviceProperties(physicalDevice, &properties);
        device_api_version = std::min(instance_data->api_version, properties.apiVersion);
        // CTS reproduces incorrect GPL vertex fetching for multiview instance inputs on this
        // observed device/driver. Scope the native fallback to this combination, not all instances.
        multiview_instance_input_workaround =
            properties.vendorID == 0x8086 && properties.deviceID == 0x7d51 && properties.driverVersion == 1661338;
    }

    ExtensionPresence present = QueryDeviceExtensions(*instance_data, physicalDevice);
    LayerCapabilities caps = DetectCapabilities(*instance_data, physicalDevice, device_api_version, present);

    // Reconcile capabilities with what the application already requested; we never flip a feature the
    // application explicitly disabled, we only fill in ones it omitted.
    FeaturePresence app_gpl =
        ScanFeature(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
                    offsetof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT, graphicsPipelineLibrary));
    FeaturePresence app_eds = ScanFeature(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
                                          offsetof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState));
    FeaturePresence app_pd = ScanFeature(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES,
                                         offsetof(VkPhysicalDevicePrivateDataFeatures, privateData));
    if (app_pd == FeaturePresence::kAbsent) {
        // The feature may instead be requested via the aggregated Vulkan 1.3 features struct.
        app_pd = ScanFeature(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                             offsetof(VkPhysicalDeviceVulkan13Features, privateData));
    }

    bool can_gpl = caps.gpl && app_gpl != FeaturePresence::kPresentFalse;
    bool below_1_3 = device_api_version < VK_API_VERSION_1_3;
    bool eds_available = caps.extended_dynamic_state && (!below_1_3 || app_eds != FeaturePresence::kPresentFalse);
    bool pd_available = caps.private_data && app_pd != FeaturePresence::kPresentFalse;

    Tier tier = Tier::kPassthrough;
    if (can_gpl) {
        tier = (eds_available && pd_available) ? Tier::kDynamize : Tier::kPureGpl;
    }

    // With device-generated commands the application can bind
    // pipelines without going through vkCmdBindPipeline, so our bind-time state restore would never
    // run. Still convert for the GPL sharing benefit, but do not dynamize.
    if (tier == Tier::kDynamize) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            const char* name = pCreateInfo->ppEnabledExtensionNames[i];
            if (strcmp(name, "VK_NV_device_generated_commands") == 0 || strcmp(name, "VK_EXT_device_generated_commands") == 0) {
                LayerMessage("[%s] device-generated-commands detected; disabling dynamization (pure GPL).\n", kLayerName);
                tier = Tier::kPureGpl;
                break;
            }
        }
    }

    // Assemble the (possibly) augmented extension list and the feature structs to prepend.
    std::vector<const char*> extensions;
    if (pCreateInfo->enabledExtensionCount) {
        extensions.assign(pCreateInfo->ppEnabledExtensionNames,
                          pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    }
    auto already_enabled = [&](const char* name) {
        for (const char* e : extensions)
            if (strcmp(e, name) == 0) return true;
        return false;
    };
    auto add_extension = [&](const char* name) {
        if (!already_enabled(name)) extensions.push_back(name);
    };

    if (can_gpl) {
        add_extension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        add_extension(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        if (tier == Tier::kDynamize && below_1_3) {
            if (app_eds == FeaturePresence::kAbsent) add_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
            if (app_pd == FeaturePresence::kAbsent) add_extension(VK_EXT_PRIVATE_DATA_EXTENSION_NAME);
        }
    }

    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT inject_gpl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, nullptr, VK_TRUE};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT inject_eds{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, nullptr, VK_TRUE};
    VkPhysicalDevicePrivateDataFeatures inject_pd{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, nullptr, VK_TRUE};

    // Prepend only the structs the application omitted, chaining into its existing (const) pNext.
    const void* injected_head = pCreateInfo->pNext;
    auto prepend = [&](auto& feature) {
        feature.pNext = const_cast<void*>(injected_head);
        injected_head = &feature;
    };
    if (can_gpl && app_gpl == FeaturePresence::kAbsent) prepend(inject_gpl);
    if (tier == Tier::kDynamize && below_1_3 && app_eds == FeaturePresence::kAbsent) prepend(inject_eds);
    if (tier == Tier::kDynamize && app_pd == FeaturePresence::kAbsent) prepend(inject_pd);

    VkDeviceCreateInfo modified = *pCreateInfo;
    modified.pNext = injected_head;
    modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modified.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    // Lower layers consume this shared link while creating their device dispatch tables. The
    // Khronos loader also carries an opaque device pointer in the output slot. Preserve its bytes
    // so output-only loaders do not require evaluating an uninitialized VkDevice value.
    VkLayerDeviceLink* next_layer = chain_info->u.pLayerInfo;
    unsigned char initial_device[sizeof(VkDevice)];
    std::memcpy(initial_device, pDevice, sizeof(initial_device));
    VkResult result = create_device(physicalDevice, &modified, pAllocator, pDevice);
    if (result != VK_SUCCESS && tier != Tier::kPassthrough) {
        // Our augmentation may have upset a finicky driver: retry untouched and run inert.
        LayerMessage("[%s] augmented vkCreateDevice failed (%d); retrying with the original create info and staying inert.\n",
                     kLayerName, result);
        chain_info->u.pLayerInfo = next_layer;
        std::memcpy(pDevice, initial_device, sizeof(initial_device));
        result = create_device(physicalDevice, pCreateInfo, pAllocator, pDevice);
        tier = Tier::kPassthrough;
    }
    if (result != VK_SUCCESS) {
        return result;
    }

    auto* device_data = new DeviceData();
    device_data->device = *pDevice;
    device_data->caps = caps;
    device_data->tier = tier;
    device_data->multiview_instance_input_workaround = multiview_instance_input_workaround;
    device_data->vtable.Initialize(*pDevice, gdpa);

    if (tier == Tier::kDynamize) {
        VkPrivateDataSlotCreateInfo slot_info{VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO, nullptr, 0};
        VkResult slot_result =
            device_data->vtable.CreatePrivateDataSlot(*pDevice, &slot_info, pAllocator, &device_data->record_slot);
        if (slot_result != VK_SUCCESS) {
            // Cannot carry records, so we cannot dynamize; fall back to pure GPL conversion.
            device_data->record_slot = VK_NULL_HANDLE;
            device_data->tier = Tier::kPureGpl;
        }
    }

    LayerMessage("[%s] device %p tier=%s (gpl=%d eds=%d privateData=%d independentInterp=%d)\n", kLayerName,
                 static_cast<void*>(*pDevice),
                 device_data->tier == Tier::kDynamize  ? "dynamize"
                 : device_data->tier == Tier::kPureGpl ? "pure-gpl"
                                                       : "passthrough",
                 caps.gpl, caps.extended_dynamic_state, caps.private_data, caps.gpl_independent_interp);

    device_data_map.insert_or_assign(DispatchKey(*pDevice), device_data);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = DispatchKey(device);
    DeviceData* device_data = GetDeviceData(device);
    ASSERT(device_data != nullptr);

    PFN_vkDestroyDevice destroy_device = device_data->vtable.DestroyDevice;

    if (device_data->tier != Tier::kPassthrough) {
        LayerMessage("[%s] device %p summary: converted=%llu expensive_libraries=%llu downgraded=%d\n", kLayerName,
                     static_cast<void*>(device), static_cast<unsigned long long>(device_data->converted_pipeline_count.load()),
                     static_cast<unsigned long long>(device_data->expensive_library_count.load()),
                     device_data->gate_downgraded.load() ? 1 : 0);
    }

    device_data->cache.DestroyAll(*device_data);
    if (device_data->record_slot != VK_NULL_HANDLE) {
        device_data->vtable.DestroyPrivateDataSlot(device, device_data->record_slot, pAllocator);
    }

    device_data_map.erase(key);
    delete device_data;

    destroy_device(device, pAllocator);
}

// Enumeration and GetProcAddr plumbing

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return EnumerateProperties(1, &kGlobalLayer, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount,
                                                                           VkExtensionProperties* pProperties) {
    if (pLayerName && strcmp(pLayerName, kLayerName) == 0) {
        return EnumerateProperties<VkExtensionProperties>(0, nullptr, pPropertyCount, pProperties);
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pPropertyCount,
                                                                     VkLayerProperties* pProperties) {
    return EnumerateProperties(1, &kGlobalLayer, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName,
                                                                         uint32_t* pPropertyCount,
                                                                         VkExtensionProperties* pProperties) {
    if (pLayerName && strcmp(pLayerName, kLayerName) == 0) {
        // The layer exposes no device extensions of its own; it uses GPL transparently.
        return EnumerateProperties<VkExtensionProperties>(0, nullptr, pPropertyCount, pProperties);
    }
    InstanceData* instance_data = GetInstanceData(physicalDevice);
    ASSERT(instance_data != nullptr);
    return instance_data->vtable.EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

static PFN_vkVoidFunction FindIntercept(const char* name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (PFN_vkVoidFunction intercept = FindIntercept(pName)) {
        return intercept;
    }
    InstanceData* instance_data = instance ? GetInstanceData(instance) : nullptr;
    if (instance_data && instance_data->vtable.GetInstanceProcAddr) {
        return instance_data->vtable.GetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName) {
    DeviceData* device_data = GetDeviceData(device);
    ASSERT(device_data != nullptr);

    // vkDestroyDevice must always be our wrapper so we can release device data even when inert.
    if (strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);

    if (device_data->tier != Tier::kPassthrough) {
        if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(CreateGraphicsPipelines);
        }
        if (strcmp(pName, "vkDestroyPipeline") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyPipeline);
        if (strcmp(pName, "vkCmdBindPipeline") == 0) return reinterpret_cast<PFN_vkVoidFunction>(CmdBindPipeline);
        if (strcmp(pName, "vkDestroyRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyRenderPass);
        if (strcmp(pName, "vkCreateRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass);
        if ((strcmp(pName, "vkCreateRenderPass2") == 0 || strcmp(pName, "vkCreateRenderPass2KHR") == 0) &&
            device_data->vtable.CreateRenderPass2)
            return reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2);
        if (strcmp(pName, "vkDestroyShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyShaderModule);
        if (strcmp(pName, "vkDestroyPipelineLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyPipelineLayout);
    }
    return device_data->vtable.GetDeviceProcAddr(device, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    // The layer intercepts no physical-device commands; forward everything.
    InstanceData* instance_data = instance ? GetInstanceData(instance) : nullptr;
    if (instance_data && instance_data->vtable.GetInstanceProcAddr) {
        return instance_data->vtable.GetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

static PFN_vkVoidFunction FindIntercept(const char* name) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction func;
    };
    static const Entry kEntries[] = {
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetInstanceProcAddr)},
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance)},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice)},
        {"vkEnumerateInstanceLayerProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceLayerProperties)},
        {"vkEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceExtensionProperties)},
        {"vkEnumerateDeviceLayerProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceLayerProperties)},
        {"vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties)},
        {"vkCreateGraphicsPipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateGraphicsPipelines)},
        {"vkDestroyPipeline", reinterpret_cast<PFN_vkVoidFunction>(DestroyPipeline)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(CmdBindPipeline)},
        {"vkDestroyRenderPass", reinterpret_cast<PFN_vkVoidFunction>(DestroyRenderPass)},
        {"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass)},
        {"vkCreateRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2)},
        {"vkCreateRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2)},
        {"vkDestroyShaderModule", reinterpret_cast<PFN_vkVoidFunction>(DestroyShaderModule)},
        {"vkDestroyPipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(DestroyPipelineLayout)},
    };
    for (const auto& entry : kEntries) {
        if (strcmp(entry.name, name) == 0) return entry.func;
    }
    return nullptr;
}

}  // namespace graphics_pipeline_library

// Exported loader entry points

extern "C" VEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return graphics_pipeline_library::GetInstanceProcAddr(instance, pName);
}

extern "C" VEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return graphics_pipeline_library::GetDeviceProcAddr(device, pName);
}

extern "C" VEL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                                                        VkLayerProperties* pProperties) {
    return graphics_pipeline_library::EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

extern "C" VEL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                                                                            uint32_t* pPropertyCount,
                                                                                            VkExtensionProperties* pProperties) {
    return graphics_pipeline_library::EnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

extern "C" VEL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                                                      uint32_t* pPropertyCount,
                                                                                      VkLayerProperties* pProperties) {
    return graphics_pipeline_library::EnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
}

extern "C" VEL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                          const char* pLayerName,
                                                                                          uint32_t* pPropertyCount,
                                                                                          VkExtensionProperties* pProperties) {
    return graphics_pipeline_library::EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

extern "C" VEL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                                                 const char* pName) {
    return graphics_pipeline_library::GetPhysicalDeviceProcAddr(instance, pName);
}

extern "C" VEL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    ASSERT(pVersionStruct != nullptr);
    ASSERT(pVersionStruct->sType == LAYER_NEGOTIATE_INTERFACE_STRUCT);
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetInstanceProcAddr = graphics_pipeline_library::GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = graphics_pipeline_library::GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = graphics_pipeline_library::GetPhysicalDeviceProcAddr;
    }
    return VK_SUCCESS;
}
