// Copyright (c) 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Private content serializers; included inside the implementation's anonymous namespace.

bool ShaderModuleChainSupported(const VkShaderModuleCreateInfo& ci) {
    for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext)
        if (node->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT) return false;
    return true;
}

bool ShaderContentKey(const VkShaderModuleCreateInfo& ci, std::string& content) {
    if (!ShaderModuleChainSupported(ci)) return false;
    KeyBuilder k;
    k.AppendU32(ci.flags);
    k.AppendU64(ci.codeSize);
    k.AppendBytes(ci.pCode, ci.codeSize);
    content = k.Get();
    return true;
}

bool SamplerContentKey(const VkSamplerCreateInfo& ci, std::string& content) {
    if (ci.pNext) return false;
    KeyBuilder k;
    k.AppendU32(ci.flags);
    k.AppendU32(ci.magFilter);
    k.AppendU32(ci.minFilter);
    k.AppendU32(ci.mipmapMode);
    k.AppendU32(ci.addressModeU);
    k.AppendU32(ci.addressModeV);
    k.AppendU32(ci.addressModeW);
    k.AppendBytes(&ci.mipLodBias, sizeof(float));
    k.AppendU32(ci.anisotropyEnable);
    k.AppendBytes(&ci.maxAnisotropy, sizeof(float));
    k.AppendU32(ci.compareEnable);
    k.AppendU32(ci.compareOp);
    k.AppendBytes(&ci.minLod, sizeof(float));
    k.AppendBytes(&ci.maxLod, sizeof(float));
    k.AppendU32(ci.borderColor);
    k.AppendU32(ci.unnormalizedCoordinates);
    content = k.Get();
    return true;
}

bool DescriptorSetLayoutContentKey(const VkDescriptorSetLayoutCreateInfo& ci, DeviceData& data, std::string& content) {
    const VkDescriptorSetLayoutBindingFlagsCreateInfo* flags = nullptr;
    const VkMutableDescriptorTypeCreateInfoEXT* mutable_types = nullptr;
    for (auto node = static_cast<const VkBaseInStructure*>(ci.pNext); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            flags = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(node);
        else if (node->sType == VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT)
            mutable_types = reinterpret_cast<const VkMutableDescriptorTypeCreateInfoEXT*>(node);
        else
            return false;
    }
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < ci.bindingCount; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (ci.pBindings[a].binding != ci.pBindings[b].binding) return ci.pBindings[a].binding < ci.pBindings[b].binding;
        return ci.pBindings[a].stageFlags < ci.pBindings[b].stageFlags;
    });
    KeyBuilder k;
    k.AppendU32(ci.flags);
    k.AppendU32(ci.bindingCount);
    for (uint32_t i : order) {
        const auto& b = ci.pBindings[i];
        k.AppendU32(b.binding);
        k.AppendU32(b.descriptorType);
        k.AppendU32(b.descriptorCount);
        k.AppendU32(b.stageFlags);
        k.AppendU32(flags && flags->bindingCount ? flags->pBindingFlags[i] : 0);
        const bool immutable =
            b.descriptorCount && b.pImmutableSamplers &&
            (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        k.AppendU32(immutable);
        if (immutable)
            for (uint32_t n = 0; n < b.descriptorCount; ++n)
                k.AppendU64(data.sampler_identities.Get(CastToUint64(b.pImmutableSamplers[n])));
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_EXT) {
            std::vector<VkDescriptorType> types;
            if (mutable_types && i < mutable_types->mutableDescriptorTypeListCount) {
                const auto& list = mutable_types->pMutableDescriptorTypeLists[i];
                if (list.descriptorTypeCount) types.assign(list.pDescriptorTypes, list.pDescriptorTypes + list.descriptorTypeCount);
            }
            std::sort(types.begin(), types.end());
            k.AppendU32(static_cast<uint32_t>(types.size()));
            for (auto type : types) k.AppendU32(type);
        }
    }
    content = k.Get();
    return true;
}

bool PipelineLayoutContentKey(const VkPipelineLayoutCreateInfo& ci, DeviceData& data, std::string& content) {
    if (ci.pNext) return false;
    KeyBuilder k;
    k.AppendU32(ci.flags);
    k.AppendU32(ci.setLayoutCount);
    for (uint32_t i = 0; i < ci.setLayoutCount; ++i)
        k.AppendU64(data.descriptor_set_layout_identities.Get(CastToUint64(ci.pSetLayouts[i])));
    std::vector<VkPushConstantRange> ranges;
    if (ci.pushConstantRangeCount) ranges.assign(ci.pPushConstantRanges, ci.pPushConstantRanges + ci.pushConstantRangeCount);
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        if (a.size != b.size) return a.size < b.size;
        return a.stageFlags < b.stageFlags;
    });
    k.AppendU32(static_cast<uint32_t>(ranges.size()));
    for (const auto& range : ranges) {
        k.AppendU32(range.stageFlags);
        k.AppendU32(range.offset);
        k.AppendU32(range.size);
    }
    content = k.Get();
    return true;
}

bool AppendRenderReference(KeyBuilder& k, const VkAttachmentReference& ref) {
    k.AppendU32(ref.attachment);
    if (ref.attachment != VK_ATTACHMENT_UNUSED) k.AppendU32(ref.layout);
    return true;
}

bool AppendRenderReference(KeyBuilder& k, const VkAttachmentReference2& ref) {
    k.AppendU32(ref.attachment);
    if (ref.attachment == VK_ATTACHMENT_UNUSED) return true;
    k.AppendU32(ref.layout);
    k.AppendU32(ref.aspectMask);
    for (auto node = static_cast<const VkBaseInStructure*>(ref.pNext); node; node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT) return false;
        k.AppendU32(node->sType);
        k.AppendU32(reinterpret_cast<const VkAttachmentReferenceStencilLayout*>(node)->stencilLayout);
    }
    k.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
    return true;
}

template <typename Ref>
bool AppendOptionalRenderReference(KeyBuilder& k, const Ref* ref) {
    k.AppendU32(ref != nullptr);
    return !ref || AppendRenderReference(k, *ref);
}

template <typename Attachment>
void AppendRenderAttachment(KeyBuilder& k, const Attachment& a) {
    k.AppendU32(a.flags);
    k.AppendU32(a.format);
    k.AppendU32(a.samples);
    k.AppendU32(a.loadOp);
    k.AppendU32(a.storeOp);
    k.AppendU32(a.stencilLoadOp);
    k.AppendU32(a.stencilStoreOp);
    k.AppendU32(a.initialLayout);
    k.AppendU32(a.finalLayout);
}

template <typename Subpass>
bool AppendRenderSubpass(KeyBuilder& k, const Subpass& s) {
    k.AppendU32(s.flags);
    k.AppendU32(s.pipelineBindPoint);
    k.AppendU32(s.inputAttachmentCount);
    for (uint32_t i = 0; i < s.inputAttachmentCount; ++i)
        if (!AppendRenderReference(k, s.pInputAttachments[i])) return false;
    k.AppendU32(s.colorAttachmentCount);
    for (uint32_t i = 0; i < s.colorAttachmentCount; ++i)
        if (!AppendRenderReference(k, s.pColorAttachments[i])) return false;
    k.AppendU32(s.pResolveAttachments != nullptr);
    if (s.pResolveAttachments)
        for (uint32_t i = 0; i < s.colorAttachmentCount; ++i)
            if (!AppendRenderReference(k, s.pResolveAttachments[i])) return false;
    if (!AppendOptionalRenderReference(k, s.pDepthStencilAttachment)) return false;
    k.AppendU32(s.preserveAttachmentCount);
    for (uint32_t i = 0; i < s.preserveAttachmentCount; ++i) k.AppendU32(s.pPreserveAttachments[i]);
    return true;
}

template <typename Dependency>
void AppendRenderDependency(KeyBuilder& k, const Dependency& d) {
    k.AppendU32(d.srcSubpass);
    k.AppendU32(d.dstSubpass);
    k.AppendU32(d.srcStageMask);
    k.AppendU32(d.dstStageMask);
    k.AppendU32(d.srcAccessMask);
    k.AppendU32(d.dstAccessMask);
    k.AppendU32(d.dependencyFlags);
}

bool AppendRenderPassChain(KeyBuilder& k, const void* chain) {
    for (auto node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
        k.AppendU32(node->sType);
        if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
            const auto& s = *reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(node);
            k.AppendU32(s.subpassCount);
            for (uint32_t i = 0; i < s.subpassCount; ++i) k.AppendU32(s.pViewMasks[i]);
            k.AppendU32(s.dependencyCount);
            for (uint32_t i = 0; i < s.dependencyCount; ++i) k.AppendU32(s.pViewOffsets[i]);
            k.AppendU32(s.correlationMaskCount);
            for (uint32_t i = 0; i < s.correlationMaskCount; ++i) k.AppendU32(s.pCorrelationMasks[i]);
        } else if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO) {
            const auto& s = *reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(node);
            k.AppendU32(s.aspectReferenceCount);
            for (uint32_t i = 0; i < s.aspectReferenceCount; ++i) {
                k.AppendU32(s.pAspectReferences[i].subpass);
                k.AppendU32(s.pAspectReferences[i].inputAttachmentIndex);
                k.AppendU32(s.pAspectReferences[i].aspectMask);
            }
        } else if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT) {
            if (!AppendRenderReference(
                    k, reinterpret_cast<const VkRenderPassFragmentDensityMapCreateInfoEXT*>(node)->fragmentDensityMapAttachment))
                return false;
        } else {
            return false;
        }
    }
    k.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
    return true;
}

bool RenderPassContentKey(const VkRenderPassCreateInfo& ci, std::string& content) {
    KeyBuilder k;
    k.AppendU32(1);  // Conservative exact definitions; do not conflate legacy and render-pass-2 extensions.
    k.AppendU32(ci.flags);
    if (!AppendRenderPassChain(k, ci.pNext)) return false;
    k.AppendU32(ci.attachmentCount);
    for (uint32_t i = 0; i < ci.attachmentCount; ++i) AppendRenderAttachment(k, ci.pAttachments[i]);
    k.AppendU32(ci.subpassCount);
    for (uint32_t i = 0; i < ci.subpassCount; ++i)
        if (!AppendRenderSubpass(k, ci.pSubpasses[i])) return false;
    k.AppendU32(ci.dependencyCount);
    for (uint32_t i = 0; i < ci.dependencyCount; ++i) AppendRenderDependency(k, ci.pDependencies[i]);
    content = k.Get();
    return true;
}

bool RenderPassContentKey(const VkRenderPassCreateInfo2& ci, std::string& content) {
    KeyBuilder k;
    k.AppendU32(2);
    k.AppendU32(ci.flags);
    if (!AppendRenderPassChain(k, ci.pNext)) return false;
    k.AppendU32(ci.attachmentCount);
    for (uint32_t i = 0; i < ci.attachmentCount; ++i) {
        const auto& a = ci.pAttachments[i];
        AppendRenderAttachment(k, a);
        for (auto node = static_cast<const VkBaseInStructure*>(a.pNext); node; node = node->pNext) {
            if (node->sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT) return false;
            const auto& s = *reinterpret_cast<const VkAttachmentDescriptionStencilLayout*>(node);
            k.AppendU32(node->sType);
            k.AppendU32(s.stencilInitialLayout);
            k.AppendU32(s.stencilFinalLayout);
        }
        k.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
    }
    k.AppendU32(ci.subpassCount);
    for (uint32_t i = 0; i < ci.subpassCount; ++i) {
        const auto& s = ci.pSubpasses[i];
        if (!AppendRenderSubpass(k, s)) return false;
        k.AppendU32(s.viewMask);
        for (auto node = static_cast<const VkBaseInStructure*>(s.pNext); node; node = node->pNext) {
            k.AppendU32(node->sType);
            if (node->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE) {
                const auto& ds = *reinterpret_cast<const VkSubpassDescriptionDepthStencilResolve*>(node);
                k.AppendU32(ds.depthResolveMode);
                k.AppendU32(ds.stencilResolveMode);
                const auto* resolve = (ds.depthResolveMode != VK_RESOLVE_MODE_NONE || ds.stencilResolveMode != VK_RESOLVE_MODE_NONE)
                                          ? ds.pDepthStencilResolveAttachment
                                          : nullptr;
                if (!AppendOptionalRenderReference(k, resolve)) return false;
            } else if (node->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) {
                const auto& rate = *reinterpret_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(node);
                if (!AppendOptionalRenderReference(k, rate.pFragmentShadingRateAttachment)) return false;
                k.AppendU32(rate.shadingRateAttachmentTexelSize.width);
                k.AppendU32(rate.shadingRateAttachmentTexelSize.height);
            } else {
                return false;
            }
        }
        k.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
    }
    k.AppendU32(ci.dependencyCount);
    for (uint32_t i = 0; i < ci.dependencyCount; ++i) {
        const auto& d = ci.pDependencies[i];
        AppendRenderDependency(k, d);
        k.AppendU32(d.viewOffset);
        for (auto node = static_cast<const VkBaseInStructure*>(d.pNext); node; node = node->pNext) {
            if (node->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 || node->pNext) return false;
            const auto& barrier = *reinterpret_cast<const VkMemoryBarrier2*>(node);
            k.AppendU32(node->sType);
            k.AppendU64(barrier.srcStageMask);
            k.AppendU64(barrier.srcAccessMask);
            k.AppendU64(barrier.dstStageMask);
            k.AppendU64(barrier.dstAccessMask);
        }
        k.AppendU32(VK_STRUCTURE_TYPE_MAX_ENUM);
    }
    k.AppendU32(ci.correlatedViewMaskCount);
    for (uint32_t i = 0; i < ci.correlatedViewMaskCount; ++i) k.AppendU32(ci.pCorrelatedViewMasks[i]);
    content = k.Get();
    return true;
}
