// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_consumers.h"
#include "omm_as_provenance.h"
#include "omm_query_rewrite.h"
#include "omm_shader_compose.h"
#include <vulkan/utility/vk_safe_struct.hpp>
#include <vulkan/utility/vk_struct_helper.hpp>
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <unordered_map>

namespace vel::omm {
namespace {
struct Native {
    VkDevice device;
    PFN_vkGetDeviceProcAddr next;
    VkAllocationCallbacks allocator{};
    bool allocated = false;
    const VkAllocationCallbacks* allocation() const { return allocated ? &allocator : nullptr; }
    template <typename T>
    T get(const char* name) const {
        return reinterpret_cast<T>(next(device, name));
    }
};
struct SetDefinition {
    std::vector<uint64_t> key;
};
template <typename T>
struct Entry {
    std::shared_ptr<T> object;
    uint64_t creations = 1;
};
struct Shader {
    std::vector<uint32_t> words;
    DescriptorContextRef placeholder_context;
};
struct Variant {
    Native native;
    VkPipeline pipeline = VK_NULL_HANDLE;
    DescriptorContextRef context;
    std::shared_ptr<const ConsumerLayout> layout;
    ~Variant() {
        if (pipeline) native.get<PFN_vkDestroyPipeline>("vkDestroyPipeline")(native.device, pipeline, native.allocation());
    }
};
bool has_query(const uint32_t* words, size_t size) noexcept {
    for (size_t offset = 5; offset < size;) {
        const auto count = words[offset] >> 16;
        if (!count || count > size - offset) return false;
        if (spv::Op(words[offset] & 65535u) == spv::OpRayQueryInitializeKHR) return true;
        offset += count;
    }
    return false;
}
bool has_query(const std::vector<uint32_t>& words) noexcept { return has_query(words.data(), words.size()); }
std::vector<uint32_t> sanitize_declarations(const uint32_t* code, size_t size) {
    std::vector<uint32_t> words(code, code + size / 4), result;
    if (words.size() < 5 || words[0] != spv::MagicNumber) return words;
    result.assign(words.begin(), words.begin() + 5);
    for (size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16;
        if (!count || count > words.size() - offset) return words;
        const auto op = spv::Op(words[offset] & 65535u);
        const bool remove = (op == spv::OpCapability && count == 2 && words[offset + 1] == 5381u) ||
                            (op == spv::OpExtension && count >= 2 &&
                             std::string(reinterpret_cast<const char*>(words.data() + offset + 1),
                                         strnlen(reinterpret_cast<const char*>(words.data() + offset + 1), (count - 1) * 4)) ==
                                 "SPV_EXT_opacity_micromap");
        if (!remove) result.insert(result.end(), words.begin() + offset, words.begin() + offset + count);
        offset += count;
    }
    return result;
}
uint32_t named(const std::vector<uint32_t>& words, const char* prefix) {
    for (size_t offset = 5; offset < words.size(); offset += words[offset] >> 16)
        if (spv::Op(words[offset] & 65535u) == spv::OpName && (words[offset] >> 16) >= 3 &&
            !std::strncmp(reinterpret_cast<const char*>(words.data() + offset + 2), prefix, std::strlen(prefix)))
            return words[offset + 1];
    return 0;
}
template <typename Handle>
uint64_t handle_bits(Handle handle) {
    static_assert(sizeof(Handle) <= 8);
    uint64_t bits = 0;
    std::memcpy(&bits, &handle, sizeof(handle));
    return bits;
}
}  // namespace
struct ConsumerLayout {
    Native native;
    VkPipelineLayout handle = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<SetDefinition>> sets;
    std::vector<VkPushConstantRange> pushes;
    ~ConsumerLayout() {
        if (handle) native.get<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(native.device, handle, native.allocation());
    }
};
struct ConsumerPipeline {
    vku::safe_VkComputePipelineCreateInfo info;
    std::shared_ptr<ConsumerLayout> layout;
    std::vector<uint32_t> source;
    DescriptorContextRef base_context;
    std::map<VkDeviceAddress, std::weak_ptr<Variant>> variants;
};
struct ConsumerBackend {
    Native native;
    DescriptorBackend* descriptors;
    std::vector<uint32_t> library;
    QueryLibraryOptions exports;
    std::mutex mutex;
    std::unordered_map<VkDescriptorSetLayout, Entry<SetDefinition>> set_layouts;
    std::map<std::vector<uint64_t>, std::weak_ptr<SetDefinition>> definitions;
    uint64_t unique_key = 0;
    std::unordered_map<VkPipelineLayout, Entry<ConsumerLayout>> layouts;
    std::unordered_map<VkShaderModule, Entry<Shader>> shaders;
    std::unordered_map<VkPipeline, Entry<ConsumerPipeline>> pipelines;
};
namespace {
bool compatible(const ConsumerLayout& a, const ConsumerLayout& b, size_t set) {
    if (a.pushes.size() != b.pushes.size() || a.sets.size() <= set || b.sets.size() <= set) return false;
    for (size_t i = 0; i < a.pushes.size(); ++i)
        if (a.pushes[i].offset != b.pushes[i].offset || a.pushes[i].size != b.pushes[i].size ||
            a.pushes[i].stageFlags != b.pushes[i].stageFlags)
            return false;
    for (size_t i = 0; i <= set; ++i)
        if (a.sets[i] != b.sets[i]) return false;
    return true;
}
std::vector<uint32_t> source_for(ConsumerBackend& backend, const VkPipelineShaderStageCreateInfo& stage) {
    if (stage.module) {
        const auto found = backend.shaders.find(stage.module);
        if (found != backend.shaders.end()) return found->second.object->words;
    }
    if (const auto* module = vku::FindStructInPNextChain<VkShaderModuleCreateInfo>(stage.pNext))
        return {module->pCode, module->pCode + module->codeSize / 4};
    return {};
}
void copy_feedback(const void* from_chain, const void* to_chain) noexcept {
    const auto* to = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfo>(to_chain);
    const auto* from = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfo>(from_chain);
    if (!to || !from) return;
    if (to->pPipelineCreationFeedback && from->pPipelineCreationFeedback)
        *to->pPipelineCreationFeedback = *from->pPipelineCreationFeedback;
    if (to->pPipelineStageCreationFeedbacks && from->pPipelineStageCreationFeedbacks)
        std::copy_n(from->pPipelineStageCreationFeedbacks,
                    std::min(to->pipelineStageCreationFeedbackCount, from->pipelineStageCreationFeedbackCount),
                    to->pPipelineStageCreationFeedbacks);
}
VkResult transform_shader(ConsumerBackend& backend, const std::vector<uint32_t>& source, const DescriptorContextRef& context,
                          std::vector<uint32_t>* output) {
    auto exports = backend.exports;
    exports.context_address = context.address;
    ComposedQueryShader composed;
    std::string diagnostic;
    auto result = compose_query_library(source, backend.library, exports, &composed, &diagnostic);
    if (result != ShaderCompositionStatus::Success) {
        std::fprintf(stderr, "OMM shader composition failed: %s\n", diagnostic.c_str());
        return result == ShaderCompositionStatus::OutOfMemory ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
    }
    AsProvenanceOptions provenance_options;
    provenance_options.descriptor_fetch_function = composed.descriptor_fetch_function;
    provenance_options.integer_resolver_function = composed.integer_resolver_function;
    AsProvenanceModule provenance;
    const auto provenance_result = propagate_as_provenance(composed.words, provenance_options, &provenance, &diagnostic);
    if (provenance_result != AsProvenanceStatus::Success) {
        std::fprintf(stderr, "OMM AS provenance failed: %s\n", diagnostic.c_str());
        return provenance_result == AsProvenanceStatus::OutOfMemory ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
    }
    QueryConsumerOptions options;
    options.classify_function = composed.classifier_function;
    options.initialization_metadata = std::move(provenance.initialization_metadata);
    QueryStateModule rewritten;
    const auto rewrite_result = rewrite_query_consumers(provenance.words, options, &rewritten, &diagnostic);
    if (rewrite_result != QueryRewriteStatus::Success) {
        std::fprintf(stderr, "OMM query consumer failed: %s\n", diagnostic.c_str());
        return rewrite_result == QueryRewriteStatus::OutOfMemory ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
    }
    *output = std::move(rewritten.words);
    return VK_SUCCESS;
}
VkResult create_variant(ConsumerBackend& backend, ConsumerPipeline& pipeline, const DescriptorContextRef& context,
                        VkPipelineCache cache, const VkAllocationCallbacks* allocator, VkPipeline* out,
                        const VkComputePipelineCreateInfo* public_info = nullptr) {
    std::vector<uint32_t> transformed;
    const auto transformed_result = transform_shader(backend, pipeline.source, context, &transformed);
    if (transformed_result != VK_SUCCESS) return transformed_result;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = transformed.size() * 4;
    module_info.pCode = transformed.data();
    struct ModuleLifetime {
        Native native;
        VkShaderModule handle{};
        ~ModuleLifetime() {
            if (handle) native.get<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(native.device, handle, native.allocation());
        }
    } module{backend.native};
    auto status = backend.native.get<PFN_vkCreateShaderModule>("vkCreateShaderModule")(backend.native.device, &module_info,
                                                                                       backend.native.allocation(), &module.handle);
    if (status != VK_SUCCESS) return status;
    // All private variants use their retained native layout. Derivative handles,
    // application pipeline caches and feedback output pointers need not survive
    // a later bind: variants are independent internal compilations.
    vku::safe_VkComputePipelineCreateInfo owned(pipeline.info.ptr());
    owned.layout = pipeline.layout->handle;
    owned.stage.module = module.handle;
    vku::RemoveFromPnext(owned.stage, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    if (!public_info) {
        constexpr auto remove = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        owned.flags &= ~remove;
        if (auto* flags = const_cast<VkPipelineCreateFlags2CreateInfo*>(
                vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfo>(owned.pNext)))
            flags->flags &= ~VkPipelineCreateFlags2(remove);
        owned.basePipelineHandle = VK_NULL_HANDLE;
        owned.basePipelineIndex = -1;
        vku::RemoveFromPnext(owned, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
    }
    status = backend.native.get<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(backend.native.device, cache, 1,
                                                                                          owned.ptr(), allocator, out);
    if (public_info) copy_feedback(owned.pNext, public_info->pNext);
    return status;
}
}  // namespace
VkResult consumer_backend_create(const ConsumerBackendCreateInfo& info, ConsumerBackend** out) {
    if (!out || !info.library_code || info.library_code_size < 20 || info.library_code_size % 4)
        return VK_ERROR_INITIALIZATION_FAILED;
    try {
        auto backend = std::make_unique<ConsumerBackend>();
        backend->native = {info.device, info.get_device_proc_addr};
        if (info.allocator) {
            backend->native.allocator = *info.allocator;
            backend->native.allocated = true;
        }
        backend->descriptors = info.descriptors;
        backend->library.assign(info.library_code, info.library_code + info.library_code_size / 4);
        backend->exports.classifier_function = named(backend->library, "ommClassify(");
        backend->exports.descriptor_fetch_function = named(backend->library, "ommFetchDescriptor(");
        if (!backend->exports.classifier_function || !backend->exports.descriptor_fetch_function)
            return VK_ERROR_INITIALIZATION_FAILED;
        *out = backend.release();
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void consumer_backend_destroy(ConsumerBackend* backend) { delete backend; }
VkResult consumer_track_set_layout(ConsumerBackend* backend, VkDescriptorSetLayout handle,
                                   const VkDescriptorSetLayoutCreateInfo* info) {
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        auto existing = backend->set_layouts.find(handle);
        if (existing != backend->set_layouts.end()) {
            ++existing->second.creations;
            return VK_SUCCESS;
        }
        std::vector<uint64_t> key{info->flags, info->bindingCount};
        const auto* flags = vku::FindStructInPNextChain<VkDescriptorSetLayoutBindingFlagsCreateInfo>(info->pNext);
        const auto* mutable_types = vku::FindStructInPNextChain<VkMutableDescriptorTypeCreateInfoEXT>(info->pNext);
        bool unknown = false;
        for (auto* next = reinterpret_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
            if (next->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO &&
                next->sType != VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT)
                unknown = true;
        std::vector<uint32_t> order(info->bindingCount);
        for (uint32_t i = 0; i < info->bindingCount; ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](auto a, auto b) { return info->pBindings[a].binding < info->pBindings[b].binding; });
        for (auto i : order) {
            const auto& binding = info->pBindings[i];
            key.insert(key.end(), {binding.binding, uint32_t(binding.descriptorType), binding.descriptorCount, binding.stageFlags,
                                   flags && i < flags->bindingCount ? flags->pBindingFlags[i] : 0});
            const bool sampler = binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                 binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            key.push_back(sampler && binding.pImmutableSamplers ? 1 : 0);
            if (sampler && binding.pImmutableSamplers)
                for (uint32_t j = 0; j < binding.descriptorCount; ++j) key.push_back(handle_bits(binding.pImmutableSamplers[j]));
            std::vector<uint32_t> types;
            if (binding.descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_EXT && mutable_types &&
                i < mutable_types->mutableDescriptorTypeListCount) {
                const auto& list = mutable_types->pMutableDescriptorTypeLists[i];
                for (uint32_t j = 0; j < list.descriptorTypeCount; ++j) types.push_back(list.pDescriptorTypes[j]);
                std::sort(types.begin(), types.end());
            }
            key.push_back(types.size());
            key.insert(key.end(), types.begin(), types.end());
        }
        if (unknown) key = {UINT64_MAX, ++backend->unique_key};
        for (auto i = backend->definitions.begin(); i != backend->definitions.end();) {
            if (i->second.expired())
                i = backend->definitions.erase(i);
            else
                ++i;
        }
        auto object = backend->definitions[key].lock();
        if (!object) {
            object = std::make_shared<SetDefinition>();
            object->key = key;
            backend->definitions[key] = object;
        }
        backend->set_layouts.emplace(handle, Entry<SetDefinition>{object});
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void consumer_untrack_set_layout(ConsumerBackend* backend, VkDescriptorSetLayout handle) {
    std::lock_guard<std::mutex> lock(backend->mutex);
    const auto found = backend->set_layouts.find(handle);
    if (found != backend->set_layouts.end() && --found->second.creations == 0) backend->set_layouts.erase(found);
}
VkResult consumer_track_layout(ConsumerBackend* backend, VkPipelineLayout handle, const VkPipelineLayoutCreateInfo* info) {
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        const auto found = backend->layouts.find(handle);
        if (found != backend->layouts.end()) {
            ++found->second.creations;
            return VK_SUCCESS;
        }
        if (info->flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) return VK_ERROR_FEATURE_NOT_PRESENT;
        auto layout = std::make_shared<ConsumerLayout>();
        layout->native = backend->native;
        layout->pushes.assign(info->pPushConstantRanges, info->pPushConstantRanges + info->pushConstantRangeCount);
        std::sort(layout->pushes.begin(), layout->pushes.end(), [](const auto& a, const auto& b) {
            return std::tie(a.offset, a.size, a.stageFlags) < std::tie(b.offset, b.size, b.stageFlags);
        });
        for (uint32_t i = 0; i < info->setLayoutCount; ++i) {
            const auto set = backend->set_layouts.find(info->pSetLayouts[i]);
            if (set == backend->set_layouts.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
            layout->sets.push_back(set->second.object);
        }
        auto status = backend->native.get<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
            backend->native.device, info, backend->native.allocation(), &layout->handle);
        if (status != VK_SUCCESS) return status;
        backend->layouts.emplace(handle, Entry<ConsumerLayout>{layout});
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void consumer_untrack_layout(ConsumerBackend* backend, VkPipelineLayout handle) {
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto found = backend->layouts.find(handle);
    if (found != backend->layouts.end() && --found->second.creations == 0) backend->layouts.erase(found);
}
VkResult consumer_create_shader(ConsumerBackend* backend, const VkShaderModuleCreateInfo* info,
                                const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    try {
        auto shader = std::make_shared<Shader>();
        shader->words.assign(info->pCode, info->pCode + info->codeSize / 4);
        if (shader->words.size() < 5 || shader->words[0] != spv::MagicNumber) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (size_t offset = 5; offset < shader->words.size();) {
            const auto count = shader->words[offset] >> 16;
            if (!count || count > shader->words.size() - offset) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (spv::Op(shader->words[offset] & 65535u) == spv::OpEntryPoint && count >= 3 &&
                shader->words[offset + 1] >= spv::ExecutionModelRayGenerationKHR &&
                shader->words[offset + 1] <= spv::ExecutionModelCallableKHR)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            offset += count;
        }
        auto sanitized = sanitize_declarations(info->pCode, info->codeSize);
        if (has_query(shader->words)) {
            // Native module validation also checks constant ray flags. Removing
            // just the EXT capability while retaining Force2State is invalid.
            // Provide a complete, valid private module now; pipelines still
            // retain/rewrite the ORIGINAL source with their actual bound sets.
            auto result = descriptor_build_context(backend->descriptors, {}, &shader->placeholder_context);
            if (result != VK_SUCCESS) return result;
            result = transform_shader(*backend, shader->words, shader->placeholder_context, &sanitized);
            if (result != VK_SUCCESS) return result;
        }
        auto native_info = *info;
        native_info.codeSize = sanitized.size() * 4;
        native_info.pCode = sanitized.data();
        auto result = backend->native.get<PFN_vkCreateShaderModule>("vkCreateShaderModule")(backend->native.device, &native_info,
                                                                                            allocator, out);
        if (result != VK_SUCCESS) return result;
        try {
            std::lock_guard<std::mutex> lock(backend->mutex);
            const auto found = backend->shaders.find(*out);
            if (found != backend->shaders.end())
                ++found->second.creations;
            else
                backend->shaders.emplace(*out, Entry<Shader>{shader});
        } catch (...) {
            backend->native.get<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(backend->native.device, *out, allocator);
            *out = VK_NULL_HANDLE;
            throw;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void consumer_destroy_shader(ConsumerBackend* backend, VkShaderModule shader, const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard<std::mutex> lock(backend->mutex);
        auto found = backend->shaders.find(shader);
        if (found != backend->shaders.end() && --found->second.creations == 0) backend->shaders.erase(found);
    }
    backend->native.get<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(backend->native.device, shader, allocator);
}
bool consumer_shader_uses_query(ConsumerBackend* backend, const VkPipelineShaderStageCreateInfo& stage) noexcept {
    std::lock_guard<std::mutex> lock(backend->mutex);
    // This predicate runs directly inside a public Vulkan hook. Inspect the
    // retained or inline source in place: copying its words could throw across
    // that C ABI while deciding whether an ordinary graphics pipeline passes.
    if (stage.module) {
        const auto found = backend->shaders.find(stage.module);
        if (found != backend->shaders.end()) return has_query(found->second.object->words);
    }
    if (const auto* source = vku::FindStructInPNextChain<VkShaderModuleCreateInfo>(stage.pNext))
        return !source->pCode || source->codeSize < 20 || source->codeSize % 4 || has_query(source->pCode, source->codeSize / 4);
    return true;  // An identifier/binary-only source is not known to be ordinary.
}
VkResult consumer_create_compute(ConsumerBackend* backend, VkPipelineCache cache, uint32_t count,
                                 const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                 VkPipeline* out) {
    for (uint32_t i = 0; i < count; ++i) out[i] = VK_NULL_HANDLE;
    VkResult overall = VK_SUCCESS;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        for (uint32_t i = 0; i < count; ++i) {
            auto info = infos[i];
            vku::safe_VkComputePipelineCreateInfo independent;
            bool independent_compilation = false;
            const auto* effective_flags = vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfo>(info.pNext);
            if (((effective_flags ? effective_flags->flags : info.flags) & VK_PIPELINE_CREATE_DERIVATIVE_BIT) &&
                !info.basePipelineHandle && info.basePipelineIndex >= 0) {
                if (uint32_t(info.basePipelineIndex) >= i) return VK_ERROR_FEATURE_NOT_PRESENT;
                info.basePipelineHandle = out[info.basePipelineIndex];
                info.basePipelineIndex = -1;
                if (!info.basePipelineHandle) {
                    // A valid batch may fail its parent yet succeed its child.
                    // Attempt the child's functional pipeline independently;
                    // never pass an invalid NULL derivative parent downstream.
                    // Preserve effective public flags and feedback except for
                    // this unavailable compilation-reuse hint, without writing
                    // through the application's pNext chain.
                    independent.initialize(&info);
                    independent.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
                    if (auto* flags = const_cast<VkPipelineCreateFlags2CreateInfo*>(
                            vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfo>(independent.pNext)))
                        flags->flags &= ~VkPipelineCreateFlags2(VK_PIPELINE_CREATE_DERIVATIVE_BIT);
                    info = *independent.ptr();
                    independent_compilation = true;
                }
            }
            auto source = source_for(*backend, info.stage);
            VkResult result;
            std::shared_ptr<ConsumerPipeline> pipeline;
            if (!source.empty() && !has_query(source))
                result = backend->native.get<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
                    backend->native.device, cache, 1, &info, allocator, &out[i]);
            else {
                const auto layout = backend->layouts.find(infos[i].layout);
                if (source.empty() || layout == backend->layouts.end() ||
                    vku::FindStructInPNextChain<VkPipelineBinaryInfoKHR>(infos[i].pNext))
                    result = VK_ERROR_FEATURE_NOT_PRESENT;
                else {
                    pipeline = std::make_shared<ConsumerPipeline>();
                    pipeline->info.initialize(&info);
                    pipeline->layout = layout->second.object;
                    pipeline->source = std::move(source);
                    DescriptorContextRef empty;
                    result = descriptor_build_context(backend->descriptors, {}, &empty);
                    if (result == VK_SUCCESS) {
                        pipeline->base_context = empty;
                        result = create_variant(*backend, *pipeline, empty, cache, allocator, &out[i], &info);
                    }
                }
            }
            if (independent_compilation) copy_feedback(info.pNext, infos[i].pNext);
            if (result == VK_SUCCESS && pipeline) {
                try {
                    const auto existing = backend->pipelines.find(out[i]);
                    if (existing != backend->pipelines.end())
                        ++existing->second.creations;
                    else
                        backend->pipelines.emplace(out[i], Entry<ConsumerPipeline>{pipeline});
                } catch (...) {
                    backend->native.get<PFN_vkDestroyPipeline>("vkDestroyPipeline")(backend->native.device, out[i], allocator);
                    out[i] = VK_NULL_HANDLE;
                    throw;
                }
            }
            if (result != VK_SUCCESS) {
                if (overall == VK_SUCCESS) overall = result;
                const auto* flags = vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfo>(info.pNext);
                if ((flags ? flags->flags : info.flags) & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT) break;
            }
        }
        return overall;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void consumer_destroy_pipeline(ConsumerBackend* backend, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard<std::mutex> lock(backend->mutex);
        auto found = backend->pipelines.find(pipeline);
        if (found != backend->pipelines.end() && --found->second.creations == 0) backend->pipelines.erase(found);
    }
    backend->native.get<PFN_vkDestroyPipeline>("vkDestroyPipeline")(backend->native.device, pipeline, allocator);
}
VkResult consumer_bind_sets(ConsumerBackend* backend, ConsumerBindingState& state, VkPipelineLayout handle, uint32_t first,
                            uint32_t count, const VkDescriptorSet* sets) {
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        const auto found = backend->layouts.find(handle);
        if (found == backend->layouts.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
        const auto layout = found->second.object;
        const size_t end = size_t(first) + count;
        if (end > layout->sets.size()) return VK_ERROR_FEATURE_NOT_PRESENT;
        const size_t size = std::max(end, state.sets.size());
        // Keep the two logical arrays in lockstep on either allocation failure.
        // Once both capacities exist, these trivial/default shared_ptr resizes
        // and every following binding update are allocation-free.
        state.sets.reserve(size);
        state.set_layouts.reserve(size);
        state.sets.resize(size, VK_NULL_HANDLE);
        state.set_layouts.resize(size);
        for (size_t set = first; set < end; ++set) {
            for (size_t lower = 0; lower < set; ++lower)
                if (state.set_layouts[lower] && !compatible(*state.set_layouts[lower], *layout, lower)) {
                    state.sets[lower] = VK_NULL_HANDLE;
                    state.set_layouts[lower] = layout;
                }
            if (state.set_layouts[set] && !compatible(*state.set_layouts[set], *layout, set))
                for (size_t higher = set + 1; higher < state.sets.size(); ++higher) {
                    state.sets[higher] = VK_NULL_HANDLE;
                    state.set_layouts[higher] = layout;
                }
            state.sets[set] = sets[set - first];
            state.set_layouts[set] = layout;
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkResult consumer_pipeline(ConsumerBackend* backend, ConsumerBindingState& state, VkPipeline logical, VkPipeline* out) {
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        if (state.sets.size() != state.set_layouts.size()) return VK_ERROR_INITIALIZATION_FAILED;
        state.logical_pipeline = logical;
        const auto found = backend->pipelines.find(logical);
        if (found == backend->pipelines.end()) {
            *out = logical;
            return VK_SUCCESS;
        }
        auto pipeline = found->second.object;
        // Unused declarations/other entry points may legally have no bound set.
        // Bindings incompatible with this pipeline remain undefined, just as in
        // native Vulkan; they must not become a stale but readable shadow set.
        auto slots = state.sets;
        for (size_t set = 0; set < slots.size(); ++set)
            if (!state.set_layouts[set] || !compatible(*state.set_layouts[set], *pipeline->layout, set))
                slots[set] = VK_NULL_HANDLE;
        DescriptorContextRef context;
        auto result = descriptor_build_context(backend->descriptors, slots, &context);
        if (result != VK_SUCCESS) return result;
        for (auto i = pipeline->variants.begin(); i != pipeline->variants.end();) {
            if (i->second.expired())
                i = pipeline->variants.erase(i);
            else
                ++i;
        }
        auto& cached = pipeline->variants[context.address];
        auto variant = cached.lock();
        if (!variant) {
            auto created = std::make_shared<Variant>();
            created->native = backend->native;
            created->context = context;
            created->layout = pipeline->layout;
            result = create_variant(*backend, *pipeline, context, VK_NULL_HANDLE, backend->native.allocation(), &created->pipeline);
            if (result != VK_SUCCESS) return result;
            variant = std::move(created);
            cached = variant;
        }
        if (std::find(state.retained.begin(), state.retained.end(), variant) == state.retained.end())
            state.retained.push_back(variant);
        *out = variant->pipeline;
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
}  // namespace vel::omm
