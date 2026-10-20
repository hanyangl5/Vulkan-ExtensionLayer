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

// Transparent GPL pipeline-state-object acceleration layer.
//
// The layer intercepts vkCreateGraphicsPipelines and, when the device supports
// VK_EXT_graphics_pipeline_library, rebuilds each monolithic pipeline out of the
// four GPL sub-libraries, sharing expensive shader compilation across state variants.
// See docs/graphics_pipeline_library_layer.md for supported state and fallback behavior.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace graphics_pipeline_library {

// Promotable states
//
// Only these five states are ever dynamized. Their attribution to a GPL sub-library is fixed:
// CULL_MODE / FRONT_FACE belong to the pre-rasterization library, the three depth states belong
// to the fragment-shader library. Declaring a dynamic-state enum on the wrong library is a
// validation error, so the mapping lives in one place.

enum StateBit : uint32_t {
    kCullMode = 1u << 0,
    kFrontFace = 1u << 1,
    kDepthTestEnable = 1u << 2,
    kDepthWriteEnable = 1u << 3,
    kDepthCompareOp = 1u << 4,
};

using StateMask = uint32_t;

static constexpr StateMask kPromotable = kCullMode | kFrontFace | kDepthTestEnable | kDepthWriteEnable | kDepthCompareOp;
static constexpr StateMask kPreRasterPromotable = kCullMode | kFrontFace;
static constexpr StateMask kFragmentShaderPromotable = kDepthTestEnable | kDepthWriteEnable | kDepthCompareOp;

template <typename Fn>
inline void ForEachStateBit(StateMask mask, Fn&& fn) {
    for (uint32_t bit = kCullMode; bit <= kDepthCompareOp; bit <<= 1) {
        if (mask & bit) {
            fn(static_cast<StateBit>(bit));
        }
    }
}

// Original static values, captured at create time and restored at bind time.
struct OverrideVals {
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkBool32 depthTestEnable = VK_FALSE;
    VkBool32 depthWriteEnable = VK_FALSE;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_NEVER;
};

// GPL sub-library membership of dynamic states
//
// To split an application pipeline into GPL libraries we must declare each of the application's own
// dynamic states on the library that owns it. A state we cannot classify means we cannot safely
// split, so the conversion bails to native creation for that pipeline.

enum class GplSubset : uint32_t {
    kUnknown = 0,
    kVertexInput = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
    kPreRaster = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    kFragmentShader = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
    kFragmentOutput = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    kVertexInputAndPreRaster = 1 | 2,
    kPreRasterAndFragmentShader = 2 | 4,
    kFragmentShaderAndOutput = 4 | 8,
    kShadersAndFragmentOutput = 2 | 4 | 8,
    kAll = 1 | 2 | 4 | 8,
};

inline bool ContainsSubset(GplSubset mask, GplSubset subset) {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(subset)) != 0;
}

// Per-pipeline record, carried on the pipeline via a private-data slot

struct PipelineRecord {
    StateMask promoted_mask = 0;
    OverrideVals original_static_values;
};

// Capabilities and enablement tier

struct LayerCapabilities {
    bool gpl = false;                     // VK_EXT_graphics_pipeline_library + feature
    bool gpl_independent_interp = false;  // graphicsPipelineLibraryIndependentInterpolationDecoration (property)
    bool extended_dynamic_state = false;  // core 1.3 or VK_EXT_extended_dynamic_state + feature
    bool private_data = false;            // core 1.3 or VK_EXT_private_data + feature
};

enum class Tier {
    kPassthrough,  // no GPL: the layer is a no-op
    kPureGpl,      // GPL only: convert but do not dynamize
    kDynamize,     // GPL + the rest: convert and dynamize the five states
};

// Dispatch tables

struct LayerDispatchInstance {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;

    void Initialize(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, uint32_t api_version);
};

struct LayerDispatchDevice {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;

    // Private data: core 1.3 or VK_EXT_private_data.
    PFN_vkCreatePrivateDataSlot CreatePrivateDataSlot = nullptr;
    PFN_vkDestroyPrivateDataSlot DestroyPrivateDataSlot = nullptr;
    PFN_vkSetPrivateData SetPrivateData = nullptr;
    PFN_vkGetPrivateData GetPrivateData = nullptr;

    // Extended dynamic state: core 1.3 or VK_EXT_extended_dynamic_state.
    PFN_vkCmdSetCullMode CmdSetCullMode = nullptr;
    PFN_vkCmdSetFrontFace CmdSetFrontFace = nullptr;
    PFN_vkCmdSetDepthTestEnable CmdSetDepthTestEnable = nullptr;
    PFN_vkCmdSetDepthWriteEnable CmdSetDepthWriteEnable = nullptr;
    PFN_vkCmdSetDepthCompareOp CmdSetDepthCompareOp = nullptr;

    void Initialize(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, uint32_t api_version);
};

// Library cache key
//
// The key is an exact byte serialization of every field that distinguishes a library, so that no
// hash collision can ever cause two semantically different libraries to be shared. The cache
// version tag is mixed in first; bump it whenever the conversion logic changes.

static constexpr uint32_t kCacheVersionTag = 10;

class KeyBuilder {
  public:
    KeyBuilder() { AppendU32(kCacheVersionTag); }

    void AppendBytes(const void* data, size_t size) {
        if (size != 0) {
            buffer_.append(static_cast<const char*>(data), size);
        }
    }
    void AppendU32(uint32_t v) { AppendBytes(&v, sizeof(v)); }
    void AppendU64(uint64_t v) { AppendBytes(&v, sizeof(v)); }

    const std::string& Get() const { return buffer_; }

  private:
    std::string buffer_;
};

// Single-flight library cache
//
// Concurrent create calls that need the same library block on a condition variable rather than
// building it twice. A build failure wakes the waiters so they fall back instead of hanging. The
// cache owns successful library handles until device destruction; failed builds can be retried.

struct DeviceData;

class LibraryCache {
  public:
    // Builder returns a library handle, or VK_NULL_HANDLE on failure.
    using Builder = VkPipeline (*)(DeviceData& device_data, const void* context);

    // Returns the cached or freshly built library, or VK_NULL_HANDLE if the build failed.
    // was_built_out distinguishes a fresh build from a cache hit for conversion statistics.
    VkPipeline GetOrBuild(const std::string& key, DeviceData& device_data, const void* context, Builder builder,
                          bool* was_built_out);

    void DestroyAll(DeviceData& device_data);

  private:
    enum class State { kBuilding, kReady };
    struct Entry {
        State state = State::kBuilding;
        VkPipeline handle = VK_NULL_HANDLE;
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Entry> entries_;
};

// Exact content interning. Unknown definitions receive a unique lifetime identity instead.
class ObjectIdentityCache {
  public:
    uint64_t Get(uint64_t handle);
    uint64_t Intern(std::string content);
    void Register(uint64_t handle, std::string content, bool known);
    void Forget(uint64_t handle);

  private:
    uint64_t InternLocked(std::string content);
    std::mutex mutex_;
    std::unordered_map<uint64_t, uint64_t> live_;
    std::unordered_map<std::string, uint64_t> contents_;
    uint64_t next_identity_ = 1;
};

// Per-object data

struct InstanceData {
    LayerDispatchInstance vtable;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_0;
};

struct DeviceData {
    LayerDispatchDevice vtable;
    VkDevice device = VK_NULL_HANDLE;
    LayerCapabilities caps;
    Tier tier = Tier::kPassthrough;

    // Record storage for dynamization. Without private data, pure-GPL conversion remains available.
    VkPrivateDataSlot record_slot = VK_NULL_HANDLE;

    LibraryCache cache;

    // Render-pass metadata is live-object state; content identities survive object destruction.
    std::mutex render_pass_mutex;
    ObjectIdentityCache render_pass_identities;
    std::unordered_map<VkRenderPass, std::vector<uint32_t>> render_pass_view_masks;
    std::unordered_map<VkRenderPass, std::vector<bool>> render_pass_color_attachments;

    ObjectIdentityCache shader_module_identities;
    ObjectIdentityCache sampler_identities;
    ObjectIdentityCache descriptor_set_layout_identities;
    ObjectIdentityCache pipeline_layout_identities;

    // Lifetime statistics only; pipeline counts and library sharing do not disable conversion.
    std::atomic<uint64_t> converted_pipeline_count{0};
    std::atomic<uint64_t> expensive_library_count{0};
    bool multiview_instance_input_workaround = false;
    bool combine_line_pre_raster = false;
    bool dynamic_rendering_local_read = false;

    uint64_t RenderPassIdentity(VkRenderPass render_pass);
    void ForgetRenderPass(VkRenderPass render_pass);
    void SetRenderPassViewMasks(VkRenderPass render_pass, std::vector<uint32_t> masks);
    uint32_t RenderPassViewMask(VkRenderPass render_pass, uint32_t subpass);
    void SetRenderPassColorAttachments(VkRenderPass render_pass, std::vector<bool> uses_color);
    bool RenderPassUsesColorAttachment(VkRenderPass render_pass, uint32_t subpass);
    uint64_t ShaderModuleIdentity(VkShaderModule shader_module);
    void ForgetShaderModule(VkShaderModule shader_module);
    uint64_t PipelineLayoutIdentity(VkPipelineLayout layout);
    void ForgetPipelineLayout(VkPipelineLayout layout);
    bool ConversionEnabled() const { return tier != Tier::kPassthrough; }
};

// Shared helpers

// The loader writes a dispatch-table pointer as the first word of every dispatchable handle; layers
// use it as the registry key so a command buffer resolves to its device, etc.
inline void* DispatchKey(const void* dispatchable_object) { return *reinterpret_cast<void* const*>(dispatchable_object); }

}  // namespace graphics_pipeline_library
