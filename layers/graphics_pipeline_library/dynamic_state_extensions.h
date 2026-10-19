/* Copyright 2026 Xiaomi. SPDX-License-Identifier: Apache-2.0 */
// Private implementation helpers; included inside graphics_pipeline_library's anonymous namespace.

bool HasDynamicState(const VkGraphicsPipelineCreateInfo& ci, VkDynamicState state) {
    if (!ci.pDynamicState) return false;
    for (uint32_t i = 0; i < ci.pDynamicState->dynamicStateCount; ++i)
        if (ci.pDynamicState->pDynamicStates[i] == state) return true;
    return false;
}

const VkPipelineCreateFlags2CreateInfo* PipelineFlags2(const VkGraphicsPipelineCreateInfo& ci) {
    for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
            return reinterpret_cast<const VkPipelineCreateFlags2CreateInfo*>(node);
    return nullptr;
}

VkPipelineCreateFlags2 EffectivePipelineFlags(const VkGraphicsPipelineCreateInfo& ci) {
    const auto* flags2 = PipelineFlags2(ci);
    return flags2 ? flags2->flags : ci.flags;
}

constexpr VkPipelineCreateFlags2 kCommonLibraryFlags =
    VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT | VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT |
    VK_PIPELINE_CREATE_2_NO_PROTECTED_ACCESS_BIT | VK_PIPELINE_CREATE_2_PROTECTED_ACCESS_ONLY_BIT;
constexpr VkPipelineCreateFlags2 kFragmentAttachmentFlags =
    VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
    VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
constexpr VkPipelineCreateFlags2 kFragmentOutputFlags = VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
                                                        VK_PIPELINE_CREATE_2_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
                                                        VK_PIPELINE_CREATE_2_ENABLE_LEGACY_DITHERING_BIT_EXT;
constexpr VkPipelineCreateFlags2 kConvertiblePipelineFlags = kCommonLibraryFlags | kFragmentAttachmentFlags | kFragmentOutputFlags |
                                                             VK_PIPELINE_CREATE_2_VIEW_INDEX_FROM_DEVICE_INDEX_BIT |
                                                             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT;

VkPipelineCreateFlags2 LibraryFlagsForSubset(const VkGraphicsPipelineCreateInfo& ci, GplSubset subset) {
    VkPipelineCreateFlags2 mask = kCommonLibraryFlags;
    if (subset == GplSubset::kPreRaster || subset == GplSubset::kFragmentShader)
        mask |= VK_PIPELINE_CREATE_2_VIEW_INDEX_FROM_DEVICE_INDEX_BIT;
    if (subset == GplSubset::kFragmentShader) mask |= kFragmentAttachmentFlags;
    if (subset == GplSubset::kFragmentOutput) mask |= kFragmentOutputFlags;
    return EffectivePipelineFlags(ci) & mask;
}

void ApplyPipelineFlags(VkGraphicsPipelineCreateInfo& out, VkPipelineCreateFlags2 flags,
                        const VkGraphicsPipelineCreateInfo& original, VkPipelineCreateFlags2CreateInfo& storage) {
    if (PipelineFlags2(original)) {
        storage = {VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, out.pNext, flags};
        out.pNext = &storage;
        out.flags = 0;  // flags2 takes precedence; never introduce this carrier on older applications.
    } else {
        out.flags = static_cast<VkPipelineCreateFlags>(flags);
    }
}

bool RasterizationEnabled(const VkGraphicsPipelineCreateInfo& ci) {
    return !ci.pRasterizationState || !ci.pRasterizationState->rasterizerDiscardEnable ||
           HasDynamicState(ci, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE);
}

bool HasTessellation(const VkGraphicsPipelineCreateInfo& ci) {
    for (uint32_t i = 0; i < ci.stageCount; ++i)
        if (ci.pStages[i].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) return true;
    return false;
}

bool UsesMeshShader(const VkGraphicsPipelineCreateInfo& ci) {
    for (uint32_t i = 0; i < ci.stageCount; ++i)
        if (ci.pStages[i].stage == VK_SHADER_STAGE_MESH_BIT_EXT) return true;
    return false;
}

const VkShaderModuleCreateInfo* InlineShader(const VkPipelineShaderStageCreateInfo& stage) {
    if (stage.module != VK_NULL_HANDLE) return nullptr;
    for (auto node = static_cast<const VkBaseInStructure*>(stage.pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            return reinterpret_cast<const VkShaderModuleCreateInfo*>(node);
    return nullptr;
}

bool SupportedStageChain(const VkPipelineShaderStageCreateInfo& stage) {
    for (auto node = static_cast<const VkBaseInStructure*>(stage.pNext); node; node = node->pNext) {
        switch (node->sType) {
            case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
            case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO:
            case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO:
                break;
            default:
                return false;
        }
    }
    return stage.module != VK_NULL_HANDLE || InlineShader(stage) != nullptr;
}

void AppendRobustness(KeyBuilder& key, const VkPipelineRobustnessCreateInfo& s) {
    key.AppendU32(s.storageBuffers);
    key.AppendU32(s.uniformBuffers);
    key.AppendU32(s.vertexInputs);
    key.AppendU32(s.images);
}

void AppendStageExtensions(KeyBuilder& key, const VkPipelineShaderStageCreateInfo& stage) {
    for (auto node = static_cast<const VkBaseInStructure*>(stage.pNext); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;  // code is keyed separately
        key.AppendU32(node->sType);
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO)
            AppendRobustness(key, *reinterpret_cast<const VkPipelineRobustnessCreateInfo*>(node));
        else if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO)
            key.AppendU32(reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(node)->requiredSubgroupSize);
    }
    key.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
}

void WriteConversionFeedback(const VkGraphicsPipelineCreateInfo& ci, uint64_t nanoseconds) {
    for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) continue;
        const auto& info = *reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo*>(node);
        *info.pPipelineCreationFeedback = {VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT, nanoseconds};
        for (uint32_t i = 0; i < info.pipelineStageCreationFeedbackCount; ++i) info.pPipelineStageCreationFeedbacks[i] = {0, 0};
    }
}

bool FragmentShaderUsesMultisampling(const VkGraphicsPipelineCreateInfo& ci) {
    return ci.renderPass != VK_NULL_HANDLE || (ci.pMultisampleState && ci.pMultisampleState->sampleShadingEnable);
}

const VkPipelineColorBlendStateCreateInfo* FragmentOutputColorBlendState(const VkGraphicsPipelineCreateInfo& ci,
                                                                         DeviceData* device = nullptr) {
    if (ci.renderPass == VK_NULL_HANDLE) {
        uint32_t color_count = 0;
        for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
                color_count = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(node)->colorAttachmentCount;
        // A depth/stencil-only pipeline does not consume color blend state. Do not read ignored
        // attachment arrays or carry an irrelevant attachmentCount into the standalone FO library.
        if (color_count == 0) return nullptr;
    } else if (device && !device->RenderPassUsesColorAttachment(ci.renderPass, ci.subpass)) {
        return nullptr;
    }
    return ci.pColorBlendState;
}

size_t StateExtensionSize(VkStructureType type) {
    switch (type) {
#define GPL_STATE_EXTENSION(tag, type) \
    case tag:                          \
        return sizeof(type)
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO,
                            VkPipelineTessellationDomainOriginStateCreateInfo);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
                            VkPipelineRasterizationDepthClipStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
                            VkPipelineRasterizationConservativeStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT,
                            VkPipelineRasterizationStateStreamCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
                            VkPipelineRasterizationProvokingVertexStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT,
                            VkPipelineRasterizationLineStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                            VkPipelineViewportDepthClipControlCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT,
                            VkPipelineColorBlendAdvancedStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT, VkPipelineColorWriteCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
                            VkPipelineSampleLocationsStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                            VkPipelineVertexInputDivisorStateCreateInfoEXT);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_W_SCALING_STATE_CREATE_INFO_NV,
                            VkPipelineViewportWScalingStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV,
                            VkPipelineViewportSwizzleStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SHADING_RATE_IMAGE_STATE_CREATE_INFO_NV,
                            VkPipelineViewportShadingRateImageStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_COARSE_SAMPLE_ORDER_STATE_CREATE_INFO_NV,
                            VkPipelineViewportCoarseSampleOrderStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_EXCLUSIVE_SCISSOR_STATE_CREATE_INFO_NV,
                            VkPipelineViewportExclusiveScissorStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_TO_COLOR_STATE_CREATE_INFO_NV,
                            VkPipelineCoverageToColorStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_MODULATION_STATE_CREATE_INFO_NV,
                            VkPipelineCoverageModulationStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_REDUCTION_STATE_CREATE_INFO_NV,
                            VkPipelineCoverageReductionStateCreateInfoNV);
        GPL_STATE_EXTENSION(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT,
                            VkPipelineViewportDepthClampControlCreateInfoEXT);
#undef GPL_STATE_EXTENSION
        default:
            return 0;
    }
}

GplSubset TopLevelExtensionSubset(VkStructureType type) {
    switch (type) {
        case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO:
            return GplSubset::kAll;
        case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO:
            return GplSubset::kFragmentShader;
        case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO:
        case VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD:
            return GplSubset::kFragmentOutput;
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:
            return GplSubset::kShadersAndFragmentOutput;
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
            return GplSubset::kPreRasterAndFragmentShader;
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV:
        case VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV:
            return GplSubset::kFragmentShader;
        case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
            return GplSubset::kPreRaster;
        default:
            return GplSubset::kUnknown;
    }
}

size_t TopLevelExtensionSize(VkStructureType type) {
    switch (type) {
        case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO:
            return sizeof(VkPipelineCreateFlags2CreateInfo);
        case VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO:
            return sizeof(VkPipelineCreationFeedbackCreateInfo);
        case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO:
            return sizeof(VkPipelineRobustnessCreateInfo);
        case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO:
            return sizeof(VkRenderingInputAttachmentIndexInfo);
        case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO:
            return sizeof(VkRenderingAttachmentLocationInfo);
        case VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD:
            return sizeof(VkAttachmentSampleCountInfoAMD);
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:
            return sizeof(VkPipelineRenderingCreateInfo);
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
            return sizeof(VkPipelineFragmentShadingRateStateCreateInfoKHR);
        case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV:
            return sizeof(VkPipelineFragmentShadingRateEnumStateCreateInfoNV);
        case VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV:
            return sizeof(VkPipelineRepresentativeFragmentTestStateCreateInfoNV);
        case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
            return sizeof(VkPipelineDiscardRectangleStateCreateInfoEXT);
        default:
            return 0;
    }
}

bool SupportedExtensionChain(const void* chain, bool top_level, const VkGraphicsPipelineCreateInfo& ci) {
    for (auto node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
        if (!(top_level ? TopLevelExtensionSize(node->sType) : StateExtensionSize(node->sType))) return false;
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT) {
            const auto& s = *reinterpret_cast<const VkPipelineSampleLocationsStateCreateInfoEXT*>(node);
            bool enabled = s.sampleLocationsEnable || HasDynamicState(ci, VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT);
            if (enabled && !HasDynamicState(ci, VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT) && s.sampleLocationsInfo.pNext) return false;
        }
    }
    return true;
}

// Owned headers let a top-level extension be present in multiple library chains without mutating
// the application's pNext pointers. Payload arrays stay borrowed only for this synchronous call.
struct LibraryExtensionChain {
    std::vector<std::vector<uint64_t>> nodes;
    std::vector<std::vector<uint32_t>> identity_maps;
    VkBaseOutStructure* head = nullptr;
    void Prepend(const VkBaseInStructure* source, size_t size, bool normalize_locations = false) {
        nodes.emplace_back((size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        std::memcpy(nodes.back().data(), source, size);
        auto node = reinterpret_cast<VkBaseOutStructure*>(nodes.back().data());
        node->pNext = head;
        head = node;
        if (normalize_locations && node->sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO) {
            auto* locations = reinterpret_cast<VkRenderingAttachmentLocationInfo*>(node);
            if (!locations->pColorAttachmentLocations && locations->colorAttachmentCount) {
                // NULL is a legal identity map. Materialize it for drivers that do not correctly
                // handle the optional pointer when this state is supplied to a standalone library.
                identity_maps.emplace_back(locations->colorAttachmentCount);
                auto& values = identity_maps.back();
                for (uint32_t i = 0; i < values.size(); ++i) values[i] = i;
                locations->pColorAttachmentLocations = values.data();
            }
        }
    }
};

template <typename T>
void AppendExtensionArray(KeyBuilder& key, uint32_t count, const T* data) {
    key.AppendU32(count);
    if (count) key.AppendBytes(data, static_cast<size_t>(count) * sizeof(T));
}

void AppendExtensionChain(KeyBuilder& key, const void* chain, const VkGraphicsPipelineCreateInfo& ci,
                          GplSubset subset = GplSubset::kUnknown) {
    auto dynamic = [&](VkDynamicState state) { return HasDynamicState(ci, state); };
    for (auto node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
        key.AppendU32(node->sType);
        switch (node->sType) {
            case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO:
                AppendRobustness(key, *reinterpret_cast<const VkPipelineRobustnessCreateInfo*>(node));
                break;
            case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO: {
                if (ci.renderPass != VK_NULL_HANDLE) break;
                const auto& s = *reinterpret_cast<const VkRenderingAttachmentLocationInfo*>(node);
                key.AppendU32(s.colorAttachmentCount);
                for (uint32_t i = 0; i < s.colorAttachmentCount; ++i)
                    key.AppendU32(s.pColorAttachmentLocations ? s.pColorAttachmentLocations[i] : i);
                break;
            }
            case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO: {
                if (ci.renderPass != VK_NULL_HANDLE) break;
                const auto& s = *reinterpret_cast<const VkRenderingInputAttachmentIndexInfo*>(node);
                key.AppendU32(s.colorAttachmentCount);
                key.AppendU32(s.pColorAttachmentInputIndices ? 1 : 0);
                if (s.pColorAttachmentInputIndices)
                    AppendExtensionArray(key, s.colorAttachmentCount, s.pColorAttachmentInputIndices);
                for (const uint32_t* index : {s.pDepthInputAttachmentIndex, s.pStencilInputAttachmentIndex}) {
                    key.AppendU32(index ? 1 : 0);
                    if (index) key.AppendU32(*index);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD: {
                if (ci.renderPass != VK_NULL_HANDLE) break;
                const auto& s = *reinterpret_cast<const VkAttachmentSampleCountInfoAMD*>(node);
                AppendExtensionArray(key, s.colorAttachmentCount, s.pColorAttachmentSamples);
                key.AppendU32(s.depthStencilAttachmentSamples);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: {
                if (ci.renderPass != VK_NULL_HANDLE) break;  // entirely ignored with a traditional render pass
                const auto& s = *reinterpret_cast<const VkPipelineRenderingCreateInfo*>(node);
                if (subset != GplSubset::kFragmentOutput) key.AppendU32(s.viewMask);
                // The implicit local-read input mapping is sized by colorAttachmentCount. Keep FS
                // libraries separate across counts even when formats themselves are ignored.
                if (subset == GplSubset::kFragmentShader) key.AppendU32(s.colorAttachmentCount);
                if (subset != GplSubset::kPreRaster && subset != GplSubset::kFragmentShader) {
                    AppendExtensionArray(key, s.colorAttachmentCount, s.pColorAttachmentFormats);
                    key.AppendU32(s.depthAttachmentFormat);
                    key.AppendU32(s.stencilAttachmentFormat);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineColorWriteCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT))
                    AppendExtensionArray(key, s.attachmentCount, s.pColorWriteEnables);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT))
                    AppendExtensionArray(key, s.vertexBindingDivisorCount, s.pVertexBindingDivisors);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineSampleLocationsStateCreateInfoEXT*>(node);
                key.AppendU32(dynamic(VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT) ? 0 : s.sampleLocationsEnable);
                if ((s.sampleLocationsEnable || dynamic(VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT)) &&
                    !dynamic(VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT)) {
                    const auto& info = s.sampleLocationsInfo;
                    key.AppendU32(info.sampleLocationsPerPixel);
                    key.AppendU32(info.sampleLocationGridSize.width);
                    key.AppendU32(info.sampleLocationGridSize.height);
                    AppendExtensionArray(key, info.sampleLocationsCount, info.pSampleLocations);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_W_SCALING_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportWScalingStateCreateInfoNV*>(node);
                key.AppendU32(dynamic(VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_ENABLE_NV) ? 0 : s.viewportWScalingEnable);
                if ((s.viewportWScalingEnable || dynamic(VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_ENABLE_NV)) &&
                    !dynamic(VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_NV))
                    AppendExtensionArray(key, s.viewportCount, s.pViewportWScalings);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportSwizzleStateCreateInfoNV*>(node);
                key.AppendU32(s.flags);
                if (!dynamic(VK_DYNAMIC_STATE_VIEWPORT_SWIZZLE_NV)) AppendExtensionArray(key, s.viewportCount, s.pViewportSwizzles);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SHADING_RATE_IMAGE_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportShadingRateImageStateCreateInfoNV*>(node);
                key.AppendU32(dynamic(VK_DYNAMIC_STATE_SHADING_RATE_IMAGE_ENABLE_NV) ? 0 : s.shadingRateImageEnable);
                if ((s.shadingRateImageEnable || dynamic(VK_DYNAMIC_STATE_SHADING_RATE_IMAGE_ENABLE_NV)) &&
                    !dynamic(VK_DYNAMIC_STATE_VIEWPORT_SHADING_RATE_PALETTE_NV)) {
                    key.AppendU32(s.viewportCount);
                    for (uint32_t i = 0; i < s.viewportCount; ++i) {
                        const auto& palette = s.pShadingRatePalettes[i];
                        AppendExtensionArray(key, palette.shadingRatePaletteEntryCount, palette.pShadingRatePaletteEntries);
                    }
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_COARSE_SAMPLE_ORDER_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportCoarseSampleOrderStateCreateInfoNV*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_VIEWPORT_COARSE_SAMPLE_ORDER_NV)) {
                    key.AppendU32(s.sampleOrderType);
                    uint32_t count = s.sampleOrderType == VK_COARSE_SAMPLE_ORDER_TYPE_CUSTOM_NV ? s.customSampleOrderCount : 0;
                    key.AppendU32(count);
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto& order = s.pCustomSampleOrders[i];
                        key.AppendU32(order.shadingRate);
                        key.AppendU32(order.sampleCount);
                        AppendExtensionArray(key, order.sampleLocationCount, order.pSampleLocations);
                    }
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_EXCLUSIVE_SCISSOR_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportExclusiveScissorStateCreateInfoNV*>(node);
                key.AppendU32(s.exclusiveScissorCount);
                if (!dynamic(VK_DYNAMIC_STATE_EXCLUSIVE_SCISSOR_NV))
                    AppendExtensionArray(key, s.exclusiveScissorCount, s.pExclusiveScissors);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_MODULATION_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineCoverageModulationStateCreateInfoNV*>(node);
                key.AppendU32(s.flags);
                key.AppendU32(dynamic(VK_DYNAMIC_STATE_COVERAGE_MODULATION_MODE_NV) ? 0 : s.coverageModulationMode);
                key.AppendU32(dynamic(VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_ENABLE_NV) ? 0 : s.coverageModulationTableEnable);
                if ((s.coverageModulationTableEnable || dynamic(VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_ENABLE_NV)) &&
                    !dynamic(VK_DYNAMIC_STATE_COVERAGE_MODULATION_TABLE_NV))
                    AppendExtensionArray(key, s.coverageModulationTableCount, s.pCoverageModulationTable);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportDepthClampControlCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_DEPTH_CLAMP_RANGE_EXT)) {
                    key.AppendU32(s.depthClampMode);
                    if (s.depthClampMode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT)
                        key.AppendBytes(s.pDepthClampRange, sizeof(VkDepthClampRangeEXT));
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineDiscardRectangleStateCreateInfoEXT*>(node);
                key.AppendU32(s.flags);
                key.AppendU32(s.discardRectangleCount);
                key.AppendU32(dynamic(VK_DYNAMIC_STATE_DISCARD_RECTANGLE_MODE_EXT) ? 0 : s.discardRectangleMode);
                if (!dynamic(VK_DYNAMIC_STATE_DISCARD_RECTANGLE_EXT))
                    AppendExtensionArray(key, s.discardRectangleCount, s.pDiscardRectangles);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO: {
                const auto& s = *reinterpret_cast<const VkPipelineTessellationDomainOriginStateCreateInfo*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_TESSELLATION_DOMAIN_ORIGIN_EXT)) key.AppendU32(s.domainOrigin);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(node);
                key.AppendU32(s.flags);
                if (!dynamic(VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT)) key.AppendU32(s.depthClipEnable);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT*>(node);
                key.AppendU32(s.flags);
                if (!dynamic(VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT)) key.AppendU32(s.conservativeRasterizationMode);
                if (!dynamic(VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT))
                    key.AppendBytes(&s.extraPrimitiveOverestimationSize, sizeof(s.extraPrimitiveOverestimationSize));
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT*>(node);
                key.AppendU32(s.flags);
                if (!dynamic(VK_DYNAMIC_STATE_RASTERIZATION_STREAM_EXT)) key.AppendU32(s.rasterizationStream);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT)) key.AppendU32(s.provokingVertexMode);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineRasterizationLineStateCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT)) key.AppendU32(s.lineRasterizationMode);
                if (!dynamic(VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT)) key.AppendU32(s.stippledLineEnable);
                if ((s.stippledLineEnable || dynamic(VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT)) &&
                    !dynamic(VK_DYNAMIC_STATE_LINE_STIPPLE_EXT)) {
                    key.AppendU32(s.lineStippleFactor);
                    key.AppendU32(s.lineStipplePattern);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT)) key.AppendU32(s.negativeOneToOne);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT: {
                const auto& s = *reinterpret_cast<const VkPipelineColorBlendAdvancedStateCreateInfoEXT*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT)) {
                    key.AppendU32(s.srcPremultiplied);
                    key.AppendU32(s.dstPremultiplied);
                    key.AppendU32(s.blendOverlap);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_TO_COLOR_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineCoverageToColorStateCreateInfoNV*>(node);
                key.AppendU32(s.flags);
                if (!dynamic(VK_DYNAMIC_STATE_COVERAGE_TO_COLOR_ENABLE_NV)) key.AppendU32(s.coverageToColorEnable);
                if (!dynamic(VK_DYNAMIC_STATE_COVERAGE_TO_COLOR_LOCATION_NV)) key.AppendU32(s.coverageToColorLocation);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_REDUCTION_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineCoverageReductionStateCreateInfoNV*>(node);
                key.AppendU32(s.flags);
                if (!dynamic(VK_DYNAMIC_STATE_COVERAGE_REDUCTION_MODE_NV)) key.AppendU32(s.coverageReductionMode);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR: {
                const auto& s = *reinterpret_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR)) {
                    key.AppendU32(s.fragmentSize.width);
                    key.AppendU32(s.fragmentSize.height);
                    for (auto op : s.combinerOps) key.AppendU32(op);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineFragmentShadingRateEnumStateCreateInfoNV*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR)) {
                    key.AppendU32(s.shadingRateType);
                    key.AppendU32(s.shadingRate);
                    for (auto op : s.combinerOps) key.AppendU32(op);
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV: {
                const auto& s = *reinterpret_cast<const VkPipelineRepresentativeFragmentTestStateCreateInfoNV*>(node);
                if (!dynamic(VK_DYNAMIC_STATE_REPRESENTATIVE_FRAGMENT_TEST_ENABLE_NV))
                    key.AppendU32(s.representativeFragmentTestEnable);
                break;
            }
            default:
                // Eligibility checks reject unknown structures before building any key.
                ASSERT(false);
                break;
        }
    }
    key.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
}
