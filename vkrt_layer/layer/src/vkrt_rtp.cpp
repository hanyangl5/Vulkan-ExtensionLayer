/* SPDX-License-Identifier: MIT */
#ifndef NOMINMAX
#define NOMINMAX
#endif
extern "C" {
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"
#include "vkrt_deferred.h"
#include "vkrt_rtp.h"
}
#include "typed_runtime.h"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
using Words = std::vector<uint32_t>;
using namespace vel::rt;
constexpr VkShaderStageFlags rt_bits = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                       VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
struct Failure {
    VkResult result;
};
void Need(bool b) {
    if (!b) throw Failure{VK_ERROR_FEATURE_NOT_PRESENT};
}
void Check(VkResult r) {
    if (r != VK_SUCCESS) throw Failure{r};
}
template <class F>
VkResult Guard(F f) noexcept {
    try {
        return f();
    } catch (const Failure &e) {
        return e.result;
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::exception &e) {
        VKRT_LOG_ERR("typed RTP: %s", e.what());
        return VK_ERROR_FEATURE_NOT_PRESENT;
    } catch (...) {
        return VK_ERROR_UNKNOWN;
    }
}
struct Allocator {
    bool custom = false;
    VkAllocationCallbacks callbacks{};
    explicit Allocator(const VkAllocationCallbacks *a = nullptr) {
        if (a) {
            custom = true;
            callbacks = *a;
        }
    }
    void *allocate(size_t n, size_t alignment, VkSystemAllocationScope scope) const {
        void *p = custom ? callbacks.pfnAllocation(callbacks.pUserData, n, alignment, scope) : std::malloc(n);
        if (!p) throw std::bad_alloc();
        return p;
    }
    void free(void *p) const {
        if (custom)
            callbacks.pfnFree(callbacks.pUserData, p);
        else
            std::free(p);
    }
};
template <class T, class... A>
T *Token(const VkAllocationCallbacks *allocator, A &&...args) {
    Allocator a(allocator);
    void *p = a.allocate(sizeof(T), alignof(T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    try {
        return new (p) T(a, std::forward<A>(args)...);
    } catch (...) {
        a.free(p);
        throw;
    }
}
template <class T>
void FreeToken(T *p) {
    if (!p) return;
    Allocator a = p->allocator;
    p->~T();
    a.free(p);
}
template <class H, class T>
H Handle(T *p) {
    return (H)(uintptr_t)p;
}
struct Layout {
    vkrt_device_data *dev;
    VkPipelineLayout native{};
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> pushes;
    uint32_t push_bytes = 0;
    ~Layout() {
        if (native) dev->vtable.DestroyPipelineLayout(dev->device, native, nullptr);
    }
};
struct ShaderToken {
    Allocator allocator;
    Words words;
    ShaderToken(Allocator a, Words w) : allocator(a), words(std::move(w)) {}
};
struct LayoutToken {
    Allocator allocator;
    std::shared_ptr<Layout> value;
    VkPipelineLayout public_layout{};
    LayoutToken(Allocator a, std::shared_ptr<Layout> v) : allocator(a), value(std::move(v)) {}
};
struct Variant {
    TypedRuntimeProgram program;
    uint64_t budget = 0;
};
struct Pipeline {
    vkrt_device_data *dev;
    std::shared_ptr<Layout> layout;
    std::vector<TypedContinuationProgram> metadata;
    std::vector<VkShaderStageFlagBits> kinds;
    std::vector<std::string> entries;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    uint32_t depth = 0;
    uint64_t identity = 0;
    bool dynamic_stack = false;
    std::shared_ptr<Variant> initial;
    std::set<uint32_t> used_sets;
    std::map<uint64_t, std::shared_ptr<Variant>> variants;
    std::mutex lock;
};
struct PipelineToken {
    Allocator allocator;
    std::shared_ptr<Pipeline> value;
    PipelineToken(Allocator a, std::shared_ptr<Pipeline> v) : allocator(a), value(std::move(v)) {}
};
struct Binding {
    std::shared_ptr<Layout> owner;
    VkPipelineLayout native{};
    uint32_t first = 0;
    std::vector<VkDescriptorSet> sets;
    std::vector<uint32_t> offsets;
    std::vector<bool> live;
};
struct Packet {
    vkrt_device_data *dev;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *mapping = nullptr;
    VkDeviceAddress address = 0;
    std::vector<VkPipeline> pipelines;
    std::shared_ptr<Pipeline> pipeline;
    ~Packet() {
        for (auto p : pipelines) dev->vtable.DestroyPipeline(dev->device, p, nullptr);
        if (mapping) dev->vtable.UnmapMemory(dev->device, memory);
        if (buffer) dev->vtable.DestroyBuffer(dev->device, buffer, nullptr);
        if (memory) dev->vtable.FreeMemory(dev->device, memory, nullptr);
    }
};
}  // namespace

struct vkrt_rtp_state {
    Allocator allocator;
    vkrt_device_data *dev;
    std::mutex lock;
    std::unordered_map<uint64_t, ShaderToken *> shaders;
    std::unordered_map<uint64_t, LayoutToken *> layouts;
    std::unordered_map<uint64_t, PipelineToken *> pipelines;
    std::atomic<uint64_t> next_identity{1};
    uint32_t max_push = 0, max_dispatch = 0;
    vkrt_rtp_state(Allocator a, vkrt_device_data *d) : allocator(a), dev(d) {}
};
struct vkrt_rtp_command {
    std::shared_ptr<Pipeline> bound;
    std::vector<Binding> rt_bindings, compute_bindings;
    std::vector<std::shared_ptr<Layout>> retained_layouts;
    std::map<uint32_t, std::vector<uint8_t>> pushes;
    std::map<uint32_t, std::shared_ptr<Layout>> push_layouts;
    std::vector<std::unique_ptr<Packet>> packets;
    uint64_t stack = 0;
};

namespace {
vkrt_device_data *Device(VkDevice d) {
    auto p = VKRT_FIND(vkrt_device_data, d);
    Need(p != nullptr);
    return p;
}
vkrt_rtp_command &State(vkrt_cmd_buffer_data *cb) {
    if (!cb->rtp) cb->rtp = new vkrt_rtp_command;
    return *cb->rtp;
}
std::shared_ptr<Layout> FindLayout(vkrt_device_data *d, VkPipelineLayout h) {
    if (!d->rtp) return {};
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->layouts.find(VKRT_HKEY(h));
    return i == d->rtp->layouts.end() ? nullptr : i->second->value;
}
std::shared_ptr<Pipeline> FindPipeline(vkrt_device_data *d, VkPipeline h) {
    if (!d->rtp) return {};
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->pipelines.find(VKRT_HKEY(h));
    return i == d->rtp->pipelines.end() ? nullptr : i->second->value;
}
bool RawShader(vkrt_device_data *d, VkShaderModule h) {
    if (!d->rtp) return false;
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    return d->rtp->shaders.count(VKRT_HKEY(h)) != 0;
}
Words ShaderWords(vkrt_device_data *d, VkShaderModule h) {
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->shaders.find(VKRT_HKEY(h));
    Need(i != d->rtp->shaders.end());
    return i->second->words;
}
bool Compatible(const Layout &a, const Layout &b, uint32_t last) {
    if (a.pushes.size() != b.pushes.size() || a.sets.size() <= last || b.sets.size() <= last) return false;
    for (size_t i = 0; i < a.pushes.size(); ++i)
        if (a.pushes[i].stageFlags != b.pushes[i].stageFlags || a.pushes[i].offset != b.pushes[i].offset ||
            a.pushes[i].size != b.pushes[i].size)
            return false;
    return std::equal(a.sets.begin(), a.sets.begin() + last + 1, b.sets.begin());
}
bool PushCompatible(const Layout &a, const Layout &b) {
    if (a.pushes.size() != b.pushes.size()) return false;
    for (size_t i = 0; i < a.pushes.size(); ++i)
        if (a.pushes[i].stageFlags != b.pushes[i].stageFlags || a.pushes[i].offset != b.pushes[i].offset ||
            a.pushes[i].size != b.pushes[i].size)
            return false;
    return true;
}
std::shared_ptr<Variant> Build(Pipeline &p, uint64_t budget) {
    std::lock_guard<std::mutex> lock(p.lock);
    auto i = p.variants.find(budget);
    if (i != p.variants.end()) return i->second;
    Need(budget && budget <= uint64_t(UINT32_MAX) * 4 && !(budget & 3));
    TypedContinuationCompileOptions config;
    config.transport = TypedFrameTransport::PrivateArray;
    config.private_frame_words = uint32_t(budget / 4);
    std::vector<TypedContinuationProgram> stages;
    for (size_t n = 0; n < p.metadata.size(); ++n)
        stages.push_back(CompileTypedPipelineStage(p.metadata[n].source, p.entries[n], p.kinds[n], nullptr,
                                                   "vel_rt_stage_" + std::to_string(n), config));
    auto v = std::make_shared<Variant>();
    v->budget = budget;
    v->program = BuildTypedRuntimeProgram(stages, p.groups, {p.depth, p.identity});
    p.variants.emplace(budget, v);
    return v;
}
void Bind(vkrt_device_data *d, VkCommandBuffer c, const Binding &b) {
    d->vtable.CmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, b.native, b.first, uint32_t(b.sets.size()), b.sets.data(),
                                    uint32_t(b.offsets.size()), b.offsets.data());
}
void Allocate(Packet &p, uint64_t bytes) {
    auto d = p.dev;
    Need(bytes <= SIZE_MAX - 7);
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = bytes + 7;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    ci.sharingMode = d->queue_family_count > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    if (d->queue_family_count > 1) {
        ci.queueFamilyIndexCount = d->queue_family_count;
        ci.pQueueFamilyIndices = d->queue_families;
    }
    VkBuffer buffer{};
    Check(d->vtable.CreateBuffer(d->device, &ci, nullptr, &buffer));
    p.buffer = buffer;
    VkMemoryRequirements req;
    d->vtable.GetBufferMemoryRequirements(d->device, p.buffer, &req);
    VkPhysicalDeviceMemoryProperties props;
    d->instance_data->vtable.GetPhysicalDeviceMemoryProperties(d->physical_device, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    Need(type != UINT32_MAX);
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &flags;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    Check(d->vtable.AllocateMemory(d->device, &ai, nullptr, &memory));
    p.memory = memory;
    Check(d->vtable.BindBufferMemory(d->device, p.buffer, p.memory, 0));
    void *mapping = nullptr;
    Check(d->vtable.MapMemory(d->device, p.memory, 0, bytes + 7, 0, &mapping));
    p.mapping = mapping;
    VkBufferDeviceAddressInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bi.buffer = p.buffer;
    uint64_t raw = d->vtable.GetBufferDeviceAddress(d->device, &bi);
    Need(raw && raw <= UINT64_MAX - 7);
    uint64_t padding = (0 - raw) & 7;
    p.address = raw + padding;
    p.mapping = static_cast<uint8_t *>(mapping) + padding;
    Need(p.address <= UINT64_MAX - bytes);
    std::memset(p.mapping, 0, size_t(bytes));
}
template <class T>
void Write(Packet &p, uint64_t offset, const T &v) {
    std::memcpy((uint8_t *)p.mapping + offset, &v, sizeof(v));
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci,
                                                    const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    *out = VK_NULL_HANDLE;
    return Guard([&] {
        auto d = Device(device);
        if (!d->rtp) return d->vtable.CreatePipelineLayout(device, ci, a, out);
        auto v = std::make_shared<Layout>();
        v->dev = d;
        if (ci->setLayoutCount) v->sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
        if (ci->pushConstantRangeCount)
            v->pushes.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
        std::sort(v->pushes.begin(), v->pushes.end(), [](auto x, auto y) {
            return std::tie(x.stageFlags, x.offset, x.size) < std::tie(y.stageFlags, y.offset, y.size);
        });
        std::vector<VkPushConstantRange> native;
        for (auto r : v->pushes) {
            Need(r.offset <= d->rtp->max_push && r.size <= d->rtp->max_push - r.offset);
            v->push_bytes = std::max(v->push_bytes, r.offset + r.size);
            if (r.stageFlags != VK_SHADER_STAGE_ALL) r.stageFlags &= ~rt_bits;
            if (r.stageFlags) native.push_back(r);
        }
        auto local = *ci;
        local.pushConstantRangeCount = uint32_t(native.size());
        local.pPushConstantRanges = native.data();
        // Keep a genuine native public handle for every unhooked ordinary
        // consumer (templates, push descriptors, maintenance6, and others).
        // The independently allocated compatible copy outlives the public
        // object's callbacks when recorded CBs retain it.
        VkPipelineLayout public_layout{};
        Check(d->vtable.CreatePipelineLayout(device, &local, a, &public_layout));
        struct PublicGuard {
            vkrt_device_data *dev;
            VkPipelineLayout handle;
            const VkAllocationCallbacks *allocator;
            ~PublicGuard() {
                if (handle) dev->vtable.DestroyPipelineLayout(dev->device, handle, allocator);
            }
        } public_guard{d, public_layout, a};
        VkPipelineLayout native_layout{};
        Check(d->vtable.CreatePipelineLayout(device, &local, nullptr, &native_layout));
        v->native = native_layout;
        auto token = Token<LayoutToken>(a, v);
        token->public_layout = public_layout;
        try {
            std::lock_guard<std::mutex> lock(d->rtp->lock);
            d->rtp->layouts.emplace(VKRT_HKEY(public_layout), token);
        } catch (...) {
            FreeToken(token);
            throw;
        }
        public_guard.handle = VK_NULL_HANDLE;
        *out = public_layout;
        return VK_SUCCESS;
    });
}
VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice device, VkPipelineLayout h, const VkAllocationCallbacks *a) {
    Guard([&] {
        auto d = Device(device);
        LayoutToken *token = nullptr;
        if (d->rtp) {
            std::lock_guard<std::mutex> lock(d->rtp->lock);
            auto i = d->rtp->layouts.find(VKRT_HKEY(h));
            if (i != d->rtp->layouts.end()) {
                token = i->second;
                d->rtp->layouts.erase(i);
            }
        }
        if (token) {
            d->vtable.DestroyPipelineLayout(device, token->public_layout,
                                            token->allocator.custom ? &token->allocator.callbacks : nullptr);
            FreeToken(token);
        } else if (h)
            d->vtable.DestroyPipelineLayout(device, h, a);
        return VK_SUCCESS;
    });
}
VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                      VkPipeline *out) {
    return Guard([&] {
        auto d = Device(device);
        std::fill(out, out + count, VK_NULL_HANDLE);
        std::vector<VkComputePipelineCreateInfo> copy(infos, infos + count);
        std::vector<std::shared_ptr<Layout>> refs;
        for (auto &c : copy) {
            Need(!RawShader(d, c.stage.module) && !FindPipeline(d, c.basePipelineHandle));
            auto ref = FindLayout(d, c.layout);
            if (ref) {
                c.layout = ref->native;
                refs.push_back(ref);
            }
        }
        return d->vtable.CreateComputePipelines(device, cache, count, copy.data(), a, out);
    });
}
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                       VkPipeline *out) {
    return Guard([&] {
        auto d = Device(device);
        std::fill(out, out + count, VK_NULL_HANDLE);
        std::vector<VkGraphicsPipelineCreateInfo> copy(infos, infos + count);
        std::vector<std::shared_ptr<Layout>> refs;
        for (auto &c : copy) {
            Need(!FindPipeline(d, c.basePipelineHandle));
            for (uint32_t s = 0; s < c.stageCount; ++s) Need(!RawShader(d, c.pStages[s].module));
            auto ref = FindLayout(d, c.layout);
            if (ref) {
                c.layout = ref->native;
                refs.push_back(ref);
            }
        }
        return d->vtable.CreateGraphicsPipelines(device, cache, count, copy.data(), a, out);
    });
}
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferred, VkPipelineCache,
                                                            uint32_t count, const VkRayTracingPipelineCreateInfoKHR *infos,
                                                            const VkAllocationCallbacks *a, VkPipeline *out) {
    std::fill(out, out + count, VK_NULL_HANDLE);
    return Guard([&] {
        auto d = Device(device);
        Need(d->rtp && vkrt_deferred_valid(d, deferred));
        VkResult overall = VK_SUCCESS;
        for (uint32_t n = 0; n < count; ++n) {
            VkResult one = Guard([&] {
                const auto &ci = infos[n];
                Need(!ci.pNext && !ci.flags && !ci.pLibraryInfo && !ci.pLibraryInterface);
                auto p = std::make_shared<Pipeline>();
                p->dev = d;
                p->layout = FindLayout(d, ci.layout);
                Need(p->layout != nullptr);
                p->depth = ci.maxPipelineRayRecursionDepth;
                p->identity = d->rtp->next_identity.fetch_add(1);
                Need(p->identity != 0);
                if (ci.pDynamicState) {
                    Need(!ci.pDynamicState->pNext);
                    for (uint32_t k = 0; k < ci.pDynamicState->dynamicStateCount; ++k) {
                        Need(ci.pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR);
                        p->dynamic_stack = true;
                    }
                }
                p->groups.assign(ci.pGroups, ci.pGroups + ci.groupCount);
                for (auto g : p->groups) Need(!g.pNext && !g.pShaderGroupCaptureReplayHandle);
                for (uint32_t s = 0; s < ci.stageCount; ++s) {
                    const auto &stage = ci.pStages[s];
                    Need(!stage.pNext && !stage.flags && stage.pName);
                    auto source = ShaderWords(d, stage.module);
                    p->kinds.push_back(stage.stage);
                    p->entries.emplace_back(stage.pName);
                    p->metadata.push_back(CompileTypedPipelineStage(source, stage.pName, stage.stage, stage.pSpecializationInfo,
                                                                    "vel_rt_stage_" + std::to_string(s)));
                }
                uint64_t bytes = PlanTypedRuntimeArenaBytes(p->metadata, p->groups, p->depth);
                p->initial = Build(*p, bytes);
                const auto &native = p->initial->program.spirv;
                for (size_t word = 5; word < native.size(); word += native[word] >> 16)
                    if ((native[word] & 65535) == spv::OpDecorate && native[word + 2] == spv::DecorationDescriptorSet)
                        p->used_sets.insert(native[word + 3]);
                auto token = Token<PipelineToken>(a, p);
                try {
                    std::lock_guard<std::mutex> lock(d->rtp->lock);
                    d->rtp->pipelines.emplace(VKRT_HKEY(Handle<VkPipeline>(token)), token);
                } catch (...) {
                    FreeToken(token);
                    throw;
                }
                out[n] = Handle<VkPipeline>(token);
                return VK_SUCCESS;
            });
            if (one != VK_SUCCESS && overall == VK_SUCCESS) overall = one;
        }
        if (overall != VK_SUCCESS) return overall;
        return deferred ? VK_OPERATION_NOT_DEFERRED_KHR : VK_SUCCESS;
    });
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline h, const VkAllocationCallbacks *a) {
    Guard([&] {
        auto d = Device(device);
        PipelineToken *token = nullptr;
        if (d->rtp) {
            std::lock_guard<std::mutex> lock(d->rtp->lock);
            auto i = d->rtp->pipelines.find(VKRT_HKEY(h));
            if (i != d->rtp->pipelines.end()) {
                token = i->second;
                d->rtp->pipelines.erase(i);
            }
        }
        if (token)
            FreeToken(token);
        else if (h)
            d->vtable.DestroyPipeline(device, h, a);
        return VK_SUCCESS;
    });
}
VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingShaderGroupHandlesKHR(VkDevice device, VkPipeline h, uint32_t first, uint32_t count,
                                                                  size_t bytes, void *data) {
    return Guard([&] {
        auto p = FindPipeline(Device(device), h);
        Need(p != nullptr);
        auto v = p->initial;
        Need(uint64_t(first) + count <= v->program.group_handles.size() && uint64_t(count) * 32 <= bytes);
        for (uint32_t i = 0; i < count; ++i) std::memcpy((uint8_t *)data + i * 32, v->program.group_handles[first + i].data(), 32);
        return VK_SUCCESS;
    });
}
VKAPI_ATTR VkDeviceSize VKAPI_CALL GetRayTracingShaderGroupStackSizeKHR(VkDevice device, VkPipeline h, uint32_t group,
                                                                        VkShaderGroupShaderKHR shader) {
    uint64_t bytes = 0;
    Guard([&] {
        auto p = FindPipeline(Device(device), h);
        Need(p && group < p->groups.size());
        auto g = p->groups[group];
        uint32_t s = VK_SHADER_UNUSED_KHR;
        if (shader == VK_SHADER_GROUP_SHADER_GENERAL_KHR)
            s = g.generalShader;
        else if (shader == VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR)
            s = g.closestHitShader;
        else if (shader == VK_SHADER_GROUP_SHADER_ANY_HIT_KHR)
            s = g.anyHitShader;
        else if (shader == VK_SHADER_GROUP_SHADER_INTERSECTION_KHR)
            s = g.intersectionShader;
        if (s != VK_SHADER_UNUSED_KHR) {
            auto &sizes = p->initial->program.stage_stack_bytes;
            Need(s < sizes.size());
            bytes = sizes[s];
        }
        return VK_SUCCESS;
    });
    return bytes;
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                                                 uint32_t first, uint32_t count, const VkDescriptorSet *sets,
                                                 uint32_t dynamic_count, const uint32_t *offsets) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb) return;
    VkResult r = Guard([&] {
        auto d = cb->device_data;
        auto ref = FindLayout(d, layout);
        VkPipelineLayout native = ref ? ref->native : layout;
        if (point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR || point == VK_PIPELINE_BIND_POINT_COMPUTE) {
            Binding b;
            b.owner = ref;
            b.native = native;
            b.first = first;
            if (count) b.sets.assign(sets, sets + count);
            b.live.resize(count, true);
            if (dynamic_count) b.offsets.assign(offsets, offsets + dynamic_count);
            auto &state = State(cb);
            auto &history = point == VK_PIPELINE_BIND_POINT_COMPUTE ? state.compute_bindings : state.rt_bindings;
            for (auto &old : history)
                for (size_t slot = 0; slot < old.live.size(); ++slot)
                    if (uint64_t(old.first) + slot >= first && uint64_t(old.first) + slot < uint64_t(first) + count)
                        old.live[slot] = false;
            history.push_back(std::move(b));
        }
        if (point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
            d->vtable.CmdBindDescriptorSets(command, point, native, first, count, sets, dynamic_count, offsets);
        return VK_SUCCESS;
    });
    if (r != VK_SUCCESS) cb->record_result = r;
}
VKAPI_ATTR void VKAPI_CALL CmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer command, uint32_t size) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb) return;
    VkResult r = Guard([&] {
        State(cb).stack = (uint64_t(size) + 3) & ~UINT64_C(3);
        return VK_SUCCESS;
    });
    if (r != VK_SUCCESS) cb->record_result = r;
}
VKAPI_ATTR void VKAPI_CALL CmdTraceRaysKHR(VkCommandBuffer command, const VkStridedDeviceAddressRegionKHR *rg,
                                           const VkStridedDeviceAddressRegionKHR *miss, const VkStridedDeviceAddressRegionKHR *hit,
                                           const VkStridedDeviceAddressRegionKHR *call, uint32_t width, uint32_t height,
                                           uint32_t depth) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    VkResult r = Guard([&] {
        auto d = cb->device_data;
        auto &state = State(cb);
        auto p = state.bound;
        Need(p && d->rtp && d->rtp->max_dispatch);
        if (!width || !height || !depth) return VK_SUCCESS;
        uint64_t total = uint64_t(width) * height;
        Need(total <= UINT64_MAX / depth);
        total *= depth;
        uint64_t default_bytes = PlanTypedRuntimeArenaBytes(p->metadata, p->groups, p->depth);
        auto v = Build(*p, p->dynamic_stack && state.stack ? state.stack : default_bytes);
        uint64_t chunks = (total - 1) / d->rtp->max_dispatch + 1;
        Need(chunks <= SIZE_MAX / VEL_RT_RUNTIME_CONTEXT_BYTES);
        uint64_t table = chunks * VEL_RT_RUNTIME_CONTEXT_BYTES;
        uint64_t push_size = (p->layout->push_bytes + 7u) & ~uint64_t(7);
        Need(p->metadata.size() <= ((UINT64_MAX - table) / (push_size + 8)));
        uint64_t bytes = table + p->metadata.size() * (push_size + 8);
        Need(bytes <= SIZE_MAX);
        auto packet = std::make_unique<Packet>();
        packet->dev = d;
        packet->pipeline = p;
        Allocate(*packet, bytes);
        uint64_t payload = table + p->metadata.size() * 8;
        for (size_t s = 0; s < p->metadata.size(); ++s) {
            if (p->metadata[s].uses_app_push_constants) {
                auto source = state.push_layouts.find(p->kinds[s]);
                Need(source != state.push_layouts.end() && PushCompatible(*source->second, *p->layout));
            }
            Write(*packet, table + s * 8, packet->address + payload + s * push_size);
            auto i = state.pushes.find(p->kinds[s]);
            if (i != state.pushes.end())
                std::memcpy((uint8_t *)packet->mapping + payload + s * push_size, i->second.data(),
                            std::min(size_t(push_size), i->second.size()));
        }
        VkShaderModuleCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = v->program.spirv.size() * 4;
        sm.pCode = v->program.spirv.data();
        VkShaderModule shader{};
        Check(d->vtable.CreateShaderModule(d->device, &sm, nullptr, &shader));
        struct ModuleGuard {
            vkrt_device_data *d;
            VkShaderModule s;
            ~ModuleGuard() { d->vtable.DestroyShaderModule(d->device, s, nullptr); }
        } module{d, shader};
        for (uint64_t chunk = 0, base = 0; chunk < chunks; ++chunk) {
            uint64_t count = std::min(uint64_t(d->rtp->max_dispatch), total - base), offset = chunk * VEL_RT_RUNTIME_CONTEXT_BYTES;
            Write(*packet, offset + VEL_RT_RUN_MAGIC, uint32_t(VEL_RT_RUNTIME_MAGIC));
            Write(*packet, offset + VEL_RT_RUN_VERSION, uint32_t(VEL_RT_RUNTIME_VERSION));
            Write(*packet, offset + VEL_RT_RUN_ARENA_BYTES, v->budget);
            Write(*packet, offset + VEL_RT_RUN_INVOCATION_COUNT, count);
            Write(*packet, offset + VEL_RT_RUN_WIDTH, width);
            Write(*packet, offset + VEL_RT_RUN_HEIGHT, height);
            Write(*packet, offset + VEL_RT_RUN_DEPTH, depth);
            Write(*packet, offset + VEL_RT_RUN_LAUNCH_BASE, base);
            Write(*packet, offset + VEL_RT_RUN_AS_REGISTRY, vkrt_as_registry_address(d));
            Write(*packet, offset + VEL_RT_RUN_PUSH_TABLE, packet->address + table);
            Write(*packet, offset + VEL_RT_RUN_PIPELINE_ID, p->identity);
            const VkStridedDeviceAddressRegionKHR *regions[] = {rg, miss, hit, call};
            const uint32_t starts[] = {80, 104, 128, 152};
            for (unsigned i = 0; i < 4; ++i) {
                Need(regions[i] != nullptr);
                Write(*packet, offset + starts[i], regions[i]->deviceAddress);
                Write(*packet, offset + starts[i] + 8, i == 0 ? regions[i]->size : regions[i]->stride);
                Write(*packet, offset + starts[i] + 16, regions[i]->size);
            }
            uint64_t root = packet->address + offset;
            uint32_t words[] = {uint32_t(root), uint32_t(root >> 32)};
            VkSpecializationMapEntry entries[] = {{0, 0, 4}, {1, 4, 4}};
            VkSpecializationInfo spec{2, entries, sizeof(words), words};
            VkComputePipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = shader;
            ci.stage.pName = "main";
            ci.stage.pSpecializationInfo = &spec;
            ci.layout = p->layout->native;
            VkPipeline native{};
            Check(d->vtable.CreateComputePipelinesInternal(d->device, VK_NULL_HANDLE, 1, &ci, nullptr, &native));
            try {
                packet->pipelines.push_back(native);
            } catch (...) {
                d->vtable.DestroyPipeline(d->device, native, nullptr);
                throw;
            }
            base += count;
        }
        // Preserve packet ownership before recording commands which reference it.
        Packet *owned = packet.get();
        state.packets.push_back(std::move(packet));
        for (const auto &b : state.rt_bindings) {
            uint32_t last = UINT32_MAX;
            for (uint32_t s = 0; s < b.live.size(); ++s)
                if (b.live[s] && p->used_sets.count(b.first + s)) last = s;
            if (last == UINT32_MAX) continue;
            if (b.owner) Need(Compatible(*b.owner, *p->layout, b.first + last));
            Bind(d, command, b);
        }
        uint64_t remaining = total;
        for (auto native : owned->pipelines) {
            uint32_t count = uint32_t(std::min(remaining, uint64_t(d->rtp->max_dispatch)));
            d->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, native);
            d->vtable.CmdDispatch(command, count, 1, 1);
            remaining -= count;
        }
        if (cb->bound_compute_pipeline)
            d->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, cb->bound_compute_pipeline);
        for (const auto &b : state.compute_bindings) Bind(d, command, b);
        for (auto push = cb->push_head; push; push = push->next)
            d->vtable.CmdPushConstants(command, push->layout, push->stages, push->offset, push->size, push->bytes);
        return VK_SUCCESS;
    });
    if (r != VK_SUCCESS) cb->record_result = r;
}
}  // namespace

extern "C" VkResult vkrt_rtp_init(vkrt_device_data *dev, const VkAllocationCallbacks *a) {
    return Guard([&] {
        if (dev->rtp) return VK_SUCCESS;
        Allocator allocator(a);
        void *memory = allocator.allocate(sizeof(vkrt_rtp_state), alignof(vkrt_rtp_state), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
        vkrt_rtp_state *s = nullptr;
        try {
            s = new (memory) vkrt_rtp_state(allocator, dev);
        } catch (...) {
            allocator.free(memory);
            throw;
        }
        VkPhysicalDeviceProperties p;
        dev->instance_data->vtable.GetPhysicalDeviceProperties(dev->physical_device, &p);
        s->max_push = p.limits.maxPushConstantsSize;
        s->max_dispatch = p.limits.maxComputeWorkGroupCount[0];
        dev->rtp = s;
        return VK_SUCCESS;
    });
}
extern "C" void vkrt_rtp_finish(vkrt_device_data *dev) {
    if (!dev->rtp) return;
    auto s = dev->rtp;
    for (auto p : s->pipelines) FreeToken(p.second);
    for (auto p : s->shaders) FreeToken(p.second);
    for (auto p : s->layouts) {
        auto token = p.second;
        dev->vtable.DestroyPipelineLayout(dev->device, token->public_layout,
                                          token->allocator.custom ? &token->allocator.callbacks : nullptr);
        FreeToken(token);
    }
    Allocator a = s->allocator;
    s->~vkrt_rtp_state();
    a.free(s);
    dev->rtp = nullptr;
}
extern "C" VkResult vkrt_rtp_create_shader(vkrt_device_data *dev, const VkShaderModuleCreateInfo *ci,
                                           const VkAllocationCallbacks *a, VkShaderModule *out, bool *handled) {
    *handled = false;
    return Guard([&] {
        if (!dev->rtp) return VK_SUCCESS;
        Need(ci && ci->pCode && ci->codeSize >= 20 && !(ci->codeSize & 3));
        bool rt = false;
        for (size_t p = 5; p < ci->codeSize / 4;) {
            uint32_t n = ci->pCode[p] >> 16;
            Need(n && n <= ci->codeSize / 4 - p);
            if ((ci->pCode[p] & 65535) == spv::OpEntryPoint) {
                uint32_t model = ci->pCode[p + 1];
                rt |= model >= spv::ExecutionModelRayGenerationKHR && model <= spv::ExecutionModelCallableKHR;
            }
            p += n;
        }
        if (!rt) return VK_SUCCESS;
        *handled = true;
        *out = VK_NULL_HANDLE;
        Need(!ci->pNext && !ci->flags);
        auto token = Token<ShaderToken>(a, Words(ci->pCode, ci->pCode + ci->codeSize / 4));
        try {
            std::lock_guard<std::mutex> lock(dev->rtp->lock);
            dev->rtp->shaders.emplace(VKRT_HKEY(Handle<VkShaderModule>(token)), token);
        } catch (...) {
            FreeToken(token);
            throw;
        }
        *out = Handle<VkShaderModule>(token);
        return VK_SUCCESS;
    });
}
extern "C" bool vkrt_rtp_destroy_shader(vkrt_device_data *dev, VkShaderModule h) {
    if (!dev->rtp) return false;
    ShaderToken *token = nullptr;
    VkResult status = Guard([&] {
        std::lock_guard<std::mutex> lock(dev->rtp->lock);
        auto i = dev->rtp->shaders.find(VKRT_HKEY(h));
        if (i != dev->rtp->shaders.end()) {
            token = i->second;
            dev->rtp->shaders.erase(i);
        }
        return VK_SUCCESS;
    });
    if (token) FreeToken(token);
    return token != nullptr || status != VK_SUCCESS;
}
extern "C" void vkrt_rtp_reset_command(vkrt_cmd_buffer_data *cb) {
    delete cb->rtp;
    cb->rtp = nullptr;
}
extern "C" VkPipelineLayout vkrt_rtp_native_layout(vkrt_device_data *dev, VkPipelineLayout h) {
    VkPipelineLayout result = h;
    Guard([&] {
        auto p = FindLayout(dev, h);
        if (p) result = p->native;
        return VK_SUCCESS;
    });
    return result;
}
extern "C" bool vkrt_rtp_bind_pipeline(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipeline h) {
    if (point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) return false;
    VkResult r = Guard([&] {
        State(cb).bound = FindPipeline(cb->device_data, h);
        Need(State(cb).bound != nullptr);
        return VK_SUCCESS;
    });
    if (r != VK_SUCCESS) cb->record_result = r;
    return true;
}
extern "C" VkShaderStageFlags vkrt_rtp_push(vkrt_cmd_buffer_data *cb, VkPipelineLayout h, VkShaderStageFlags stages,
                                            uint32_t offset, uint32_t size, const void *values) {
    if (!cb->device_data->rtp) return stages;
    VkResult r = Guard([&] {
        auto p = FindLayout(cb->device_data, h);
        if (p) State(cb).retained_layouts.push_back(p);
        if (stages & rt_bits) {
            Need(p && offset <= p->push_bytes && size <= p->push_bytes - offset);
            auto &state = State(cb);
            for (uint32_t bit = 1; bit; bit <<= 1)
                if ((stages & rt_bits) & bit) {
                    auto &bytes = state.pushes[bit];
                    bytes.resize(cb->device_data->rtp->max_push);
                    std::memcpy(bytes.data() + offset, values, size);
                    state.push_layouts[bit] = p;
                }
        }
        return VK_SUCCESS;
    });
    if (r != VK_SUCCESS) cb->record_result = r;
    return stages == VK_SHADER_STAGE_ALL ? VK_SHADER_STAGE_ALL : stages & ~rt_bits;
}
extern "C" PFN_vkVoidFunction vkrt_rtp_proc(const char *name) {
    if (!name) return nullptr;
#define ENTRY(fn) \
    if (!std::strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)fn
    ENTRY(CreatePipelineLayout);
    ENTRY(DestroyPipelineLayout);
    ENTRY(CreateComputePipelines);
    ENTRY(CreateGraphicsPipelines);
    ENTRY(CreateRayTracingPipelinesKHR);
    ENTRY(DestroyPipeline);
    ENTRY(GetRayTracingShaderGroupHandlesKHR);
    ENTRY(GetRayTracingShaderGroupStackSizeKHR);
    ENTRY(CmdBindDescriptorSets);
    ENTRY(CmdSetRayTracingPipelineStackSizeKHR);
    ENTRY(CmdTraceRaysKHR);
#undef ENTRY
    return nullptr;
}
