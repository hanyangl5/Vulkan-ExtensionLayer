/* SPDX-License-Identifier: MIT */
#ifndef NOMINMAX
#define NOMINMAX
#endif
extern "C" {
#include "vkrt_layer.h"
#include "vkrt_as_registry.h"
#include "vkrt_deferred.h"
#include "vkrt_rtp.h"
#include "vkrt_debug_objects.h"
}
#include "typed_runtime.h"
#include "rtp_launch_plan.h"
#include "vkrt_replay_capture_generated.h"
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
T *ScopedToken(const VkAllocationCallbacks *allocator, VkSystemAllocationScope scope, A &&...args) {
    Allocator a(allocator);
    void *p = a.allocate(sizeof(T), alignof(T), scope);
    try {
        return new (p) T(a, std::forward<A>(args)...);
    } catch (...) {
        a.free(p);
        throw;
    }
}
template <class T, class... A>
T *Token(const VkAllocationCallbacks *allocator, A &&...args) {
    return ScopedToken<T>(allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, std::forward<A>(args)...);
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
struct DescriptorBinding {
    uint32_t binding, count;
    VkDescriptorType type;
    VkShaderStageFlags stages;
    VkDescriptorBindingFlags flags = 0;
    std::vector<VkSampler> samplers;
    std::vector<VkDescriptorType> mutable_types;
    bool operator==(const DescriptorBinding &b) const {
        return binding == b.binding && count == b.count && type == b.type && stages == b.stages && flags == b.flags &&
               samplers == b.samplers && mutable_types == b.mutable_types;
    }
};
struct DescriptorLayout {
    VkDescriptorSetLayoutCreateFlags flags;
    std::vector<DescriptorBinding> bindings;
    bool known = true;
    bool operator==(const DescriptorLayout &b) const { return known && b.known && flags == b.flags && bindings == b.bindings; }
};
struct DescriptorLayoutRecord {
    std::shared_ptr<const DescriptorLayout> definition;
    uint64_t references = 1;
};
struct Layout {
    vkrt_device_data *dev;
    VkPipelineLayout native{}, rt_native{};
    std::vector<std::shared_ptr<const DescriptorLayout>> sets;
    std::vector<VkPushConstantRange> pushes;
    bool independent_sets = false;
    uint32_t push_bytes = 0;
    ~Layout() {
        if (rt_native) dev->vtable.DestroyPipelineLayout(dev->device, rt_native, nullptr);
        if (native) dev->vtable.DestroyPipelineLayout(dev->device, native, nullptr);
    }
};
struct ShaderToken {
    enum class Kind { Source, Native };
    Allocator allocator;
    Kind kind;
    Words words;
    VkShaderModule native{};
    ShaderToken(Allocator a, Words w) : allocator(a), kind(Kind::Source), words(std::move(w)) {}
    ShaderToken(Allocator a, VkShaderModule module) : allocator(a), kind(Kind::Native), native(module) {}
};
void FreeShader(vkrt_device_data *dev, ShaderToken *token, Allocator allocator) {
    if (token->kind == ShaderToken::Kind::Native)
        dev->vtable.DestroyShaderModule(dev->device, token->native, allocator.custom ? &allocator.callbacks : nullptr);
    token->~ShaderToken();
    allocator.free(token);
}
struct LayoutToken {
    Allocator allocator;
    Allocator public_allocator;
    std::shared_ptr<Layout> value;
    VkPipelineLayout public_layout{};
    LayoutToken *next = nullptr;
    LayoutToken(Allocator owner, Allocator object, std::shared_ptr<Layout> v)
        : allocator(owner), public_allocator(object), value(std::move(v)) {}
};
struct LayoutRecord {
    std::shared_ptr<Layout> canonical;
    LayoutToken *tokens = nullptr;
};
struct Variant {
    vkrt_device_data *dev = nullptr;
    TypedRuntimeProgram program;
    VkPipeline native{};
    std::set<uint32_t> used_sets;
    ~Variant() {
        if (native) dev->vtable.DestroyPipeline(dev->device, native, nullptr);
    }
};
struct Pipeline {
    vkrt_device_data *dev;
    std::shared_ptr<Layout> layout;
    std::vector<TypedContinuationProgram> metadata;
    std::vector<VkShaderStageFlagBits> kinds;
    std::vector<std::string> entries;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    uint32_t depth = 0;
    bool has_interface = false;
    uint32_t max_payload = 0, max_attribute = 0;
    std::vector<uint64_t> library_stack_bytes;
    VkPipelineCreateFlags flags = 0;
    uint64_t identity = 0;
    bool dynamic_stack = false;
    std::shared_ptr<Variant> initial;
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
struct JournalDeleter {
    void operator()(vkrt_replay_journal *journal) const { vkrt_replay_journal_destroy(journal); }
};
struct Packet {
    vkrt_device_data *dev;
    vkrt_private_slice context{}, arena{};
    void *mapping = nullptr;
    VkDeviceAddress address = 0;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Variant> variant;
    ~Packet() {
        vkrt_private_pool_release(dev, &arena);
        vkrt_private_pool_release(dev, &context);
    }
};
}  // namespace

struct vkrt_rtp_state {
    Allocator allocator;
    vkrt_device_data *dev;
    std::mutex lock;
    std::unordered_map<uint64_t, ShaderToken *> shaders;
    std::unordered_map<uint64_t, LayoutRecord> layouts;
    std::unordered_map<uint64_t, PipelineToken *> pipelines;
    std::unordered_map<uint64_t, DescriptorLayoutRecord> descriptor_layouts;
    std::atomic<uint64_t> next_identity{1};
    uint32_t max_push = 0, max_dispatch = 0;
    vkrt_rtp_state(Allocator a, vkrt_device_data *d) : allocator(a), dev(d) {}
};
struct vkrt_rtp_command {
    std::shared_ptr<Pipeline> bound;
    std::vector<Binding> rt_bindings, compute_bindings;
    // Buffer bindings are global and rebinding them invalidates offsets at
    // every bind point. Restore the complete native descriptor history in
    // order, including graphics state needed by a later draw.
    std::unique_ptr<vkrt_replay_journal, JournalDeleter> descriptors;
    bool restoring_descriptors = false;
    std::vector<std::shared_ptr<Layout>> retained_layouts;
    std::unordered_map<uint64_t, std::shared_ptr<Layout>> replay_layouts;
    std::map<uint32_t, std::vector<uint8_t>> pushes;
    std::map<uint32_t, std::shared_ptr<Layout>> push_layouts;
    std::vector<std::unique_ptr<Packet>> packets;
    uint64_t stack = 0;
    bool stack_set = false;
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
template <class F>
F Native(vkrt_device_data *dev, const char *name, const char *alias = nullptr) {
    Need(dev->vtable.GetDeviceProcAddr != nullptr);
    auto proc = dev->vtable.GetDeviceProcAddr(dev->device, name);
    if (!proc && alias) proc = dev->vtable.GetDeviceProcAddr(dev->device, alias);
    Need(proc != nullptr);
    return reinterpret_cast<F>(proc);
}
template <class F>
void RecordDescriptor(vkrt_cmd_buffer_data *cb, F record) {
    if (!cb->device_data->rtp || !cb->replaying) return;
    auto &state = State(cb);
    if (state.restoring_descriptors) return;
    if (!state.descriptors) {
        const auto &allocator = cb->device_data->rtp->allocator;
        state.descriptors.reset(
            vkrt_replay_journal_create(allocator.custom ? &allocator.callbacks : nullptr, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE));
        if (!state.descriptors) throw std::bad_alloc();
    }
    // Inputs already name this recording's retained layout identities. Reuse
    // the generic schema for all pointer-valued descriptor data and pNexts.
    vkrt_replay_context context{};
    context.journal = state.descriptors.get();
    Check(record(&context));
}
void RecordSets(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count,
                const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets) {
    RecordDescriptor(cb, [&](vkrt_replay_context *context) {
        auto capture =
            reinterpret_cast<PFN_vkrt_capture_CmdBindDescriptorSets>(vkrt_replay_capture_proc("vkCmdBindDescriptorSets"));
        Need(capture != nullptr);
        return capture(context, point, layout, first, count, sets, dynamic_count, offsets);
    });
}
std::shared_ptr<Layout> FindLayout(vkrt_device_data *d, VkPipelineLayout h) {
    if (!d->rtp) return {};
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->layouts.find(VKRT_HKEY(h));
    return i == d->rtp->layouts.end() ? nullptr : i->second.canonical;
}
std::shared_ptr<Layout> FindCommandLayout(vkrt_cmd_buffer_data *cb, VkPipelineLayout h) {
    if (!cb->replaying) return FindLayout(cb->device_data, h);
    if (!cb->rtp) return {};
    auto i = cb->rtp->replay_layouts.find(VKRT_HKEY(h));
    return i == cb->rtp->replay_layouts.end() ? nullptr : i->second;
}
std::shared_ptr<Pipeline> FindPipeline(vkrt_device_data *d, VkPipeline h) {
    if (!d->rtp) return {};
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->pipelines.find(VKRT_HKEY(h));
    return i == d->rtp->pipelines.end() ? nullptr : i->second->value;
}
VkShaderModule RegisterShader(vkrt_device_data *dev, ShaderToken *token) {
    const auto module = Handle<VkShaderModule>(token);
    try {
        std::lock_guard<std::mutex> lock(dev->rtp->lock);
        dev->rtp->shaders.emplace(VKRT_HKEY(module), token);
    } catch (...) {
        FreeToken(token);
        throw;
    }
    return module;
}
bool RawShader(vkrt_device_data *d, VkShaderModule h) {
    if (!d->rtp) return false;
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->shaders.find(VKRT_HKEY(h));
    return i != d->rtp->shaders.end() && i->second->kind == ShaderToken::Kind::Source;
}
Words ShaderWords(vkrt_device_data *d, VkShaderModule h) {
    std::lock_guard<std::mutex> lock(d->rtp->lock);
    auto i = d->rtp->shaders.find(VKRT_HKEY(h));
    Need(i != d->rtp->shaders.end() && i->second->kind == ShaderToken::Kind::Source);
    return i->second->words;
}
bool Compatible(const Layout &a, const Layout &b, uint32_t last) {
    if (a.pushes.size() != b.pushes.size() || a.sets.size() <= last || b.sets.size() <= last ||
        a.independent_sets != b.independent_sets)
        return false;
    for (size_t i = 0; i < a.pushes.size(); ++i)
        if (a.pushes[i].stageFlags != b.pushes[i].stageFlags || a.pushes[i].offset != b.pushes[i].offset ||
            a.pushes[i].size != b.pushes[i].size)
            return false;
    for (uint32_t i = 0; i <= last; ++i)
        if (a.sets[i] != b.sets[i] && (!a.sets[i] || !b.sets[i] || !(*a.sets[i] == *b.sets[i]))) return false;
    return true;
}
bool SameLayoutDefinition(const Layout &a, const Layout &b) {
    if (a.sets.size() != b.sets.size() || a.pushes.size() != b.pushes.size() || a.independent_sets != b.independent_sets)
        return false;
    for (size_t i = 0; i < a.pushes.size(); ++i)
        if (a.pushes[i].stageFlags != b.pushes[i].stageFlags || a.pushes[i].offset != b.pushes[i].offset ||
            a.pushes[i].size != b.pushes[i].size)
            return false;
    for (size_t i = 0; i < a.sets.size(); ++i)
        if (a.sets[i] != b.sets[i] && (!a.sets[i] || !b.sets[i] || !(*a.sets[i] == *b.sets[i]))) return false;
    return true;
}
bool PushCompatible(const Layout &a, const Layout &b) {
    if (a.pushes.size() != b.pushes.size()) return false;
    for (size_t i = 0; i < a.pushes.size(); ++i)
        if (a.pushes[i].stageFlags != b.pushes[i].stageFlags || a.pushes[i].offset != b.pushes[i].offset ||
            a.pushes[i].size != b.pushes[i].size)
            return false;
    return true;
}
std::shared_ptr<Variant> Build(Pipeline &p, VkPipelineCache cache = VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(p.lock);
    if (p.initial) return p.initial;
    TypedContinuationCompileOptions config;
    config.transport = TypedFrameTransport::PhysicalAddress;
    std::vector<TypedContinuationProgram> stages;
    for (size_t n = 0; n < p.metadata.size(); ++n)
        stages.push_back(CompileTypedPipelineStage(p.metadata[n].source, p.entries[n], p.kinds[n], nullptr,
                                                   "vel_rt_stage_" + std::to_string(n), config));
    auto v = std::make_shared<Variant>();
    v->dev = p.dev;
    const bool optimize = (p.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) == 0;
    v->program = BuildTypedRuntimeProgram(stages, p.groups, {p.depth, p.identity, true, optimize, true});
    const auto &words = v->program.spirv;
    for (size_t word = 5; word < words.size(); word += words[word] >> 16)
        if ((words[word] & 65535) == spv::OpDecorate && words[word + 2] == spv::DecorationDescriptorSet)
            v->used_sets.insert(words[word + 3]);
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = v->program.spirv.size() * 4;
    sm.pCode = v->program.spirv.data();
    VkShaderModule shader{};
    Check(p.dev->vtable.CreateShaderModule(p.dev->device, &sm, nullptr, &shader));
    struct ModuleGuard {
        vkrt_device_data *dev;
        VkShaderModule shader;
        ~ModuleGuard() { dev->vtable.DestroyShaderModule(dev->device, shader, nullptr); }
    } module{p.dev, shader};
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.flags = p.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = shader;
    ci.stage.pName = "main";
    ci.layout = p.layout->rt_native;
    VkPipeline native{};
    VkResult result = p.dev->vtable.CreateComputePipelinesInternal(p.dev->device, cache, 1, &ci, nullptr, &native);
    if (result != VK_SUCCESS) {
        if (native) p.dev->vtable.DestroyPipeline(p.dev->device, native, nullptr);
        Check(result);
    }
    v->native = native;
    Need(v->native != VK_NULL_HANDLE);
    p.initial = v;
    VKRT_LOG_DBG("typed RTP BDA native variant created identity=%llu", (unsigned long long)p.identity);
    return v;
}
void Bind(vkrt_device_data *d, VkCommandBuffer c, const Binding &b, bool rt = false) {
    Need(!rt || b.owner);
    VkPipelineLayout layout = rt ? b.owner->rt_native : b.native;
    d->vtable.CmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, b.first, uint32_t(b.sets.size()), b.sets.data(),
                                    uint32_t(b.offsets.size()), b.offsets.data());
}
void Allocate(Packet &p, RtpLaunchPlan &plan) {
    Check(vkrt_private_pool_allocate_ex(p.dev, plan.context_bytes, VKRT_PRIVATE_HOST_COHERENT, 8, 1, &p.context));
    Need(p.context.mapping && p.context.size >= plan.context_bytes && p.context.address && !(p.context.address & 7) &&
         plan.context_bytes <= UINT64_MAX - p.context.address);
    p.address = p.context.address;
    p.mapping = p.context.mapping;
    std::memset(p.mapping, 0, size_t(plan.context_bytes));
    for (;;) {
        const auto result =
            vkrt_private_pool_allocate_ex(p.dev, plan.arena_bytes, VKRT_PRIVATE_DEVICE_LOCAL_PREFERRED, 16, 1, &p.arena);
        if (result == VK_SUCCESS) break;
        if ((result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) || !ReduceRtpLaunchTile(plan))
            Check(result);
    }
    Need(p.arena.address && !(p.arena.address & 15) && p.arena.size >= plan.arena_bytes &&
         plan.arena_bytes <= UINT64_MAX - p.arena.address);
}
template <class T>
void Write(Packet &p, uint64_t offset, const T &v) {
    Need(offset <= p.context.size && sizeof(v) <= p.context.size - offset);
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
        v->independent_sets = (ci->flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0;
        {
            std::lock_guard<std::mutex> lock(d->rtp->lock);
            for (uint32_t i = 0; i < ci->setLayoutCount; ++i) {
                if (!ci->pSetLayouts[i]) {
                    v->sets.push_back(nullptr);
                    continue;
                }
                auto definition = d->rtp->descriptor_layouts.find(VKRT_HKEY(ci->pSetLayouts[i]));
                Need(definition != d->rtp->descriptor_layouts.end());
                v->sets.push_back(definition->second.definition);
            }
        }
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
        // Serialize native public-handle creation/publication with retirement.
        // v was declared before this lock, so its private native owners unwind
        // only after unlocking if any later operation fails.
        std::unique_lock<std::mutex> mutation(d->rtp->lock);
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
        auto alias = d->rtp->layouts.find(VKRT_HKEY(public_layout));
        // Stripping RT ranges can make different public definitions identical
        // to the ICD. A numeric native alias cannot identify both definitions.
        // Until all public consumers support virtual layout handles, reject
        // that ambiguity rather than silently replacing recorded semantics.
        Need(alias == d->rtp->layouts.end() || SameLayoutDefinition(*alias->second.canonical, *v));
        VkPipelineLayout native_layout{};
        Check(d->vtable.CreatePipelineLayout(device, &local, nullptr, &native_layout));
        v->native = native_layout;
        // RT shader push constants are read through immutable BDA snapshots.
        // This separate layout uses the native compute range for a recorded
        // context pointer, independent of every application push range. Create
        // it now, while the caller's descriptor-set layout handles are valid.
        VkPushConstantRange context_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        auto rt_layout = *ci;
        rt_layout.pushConstantRangeCount = 1;
        rt_layout.pPushConstantRanges = &context_range;
        VkPipelineLayout native_rt{};
        Check(d->vtable.CreatePipelineLayout(device, &rt_layout, nullptr, &native_rt));
        v->rt_native = native_rt;
        // Alias bookkeeping can outlive any particular object's callbacks.
        // Native public objects still use their caller's OBJECT allocator.
        const VkAllocationCallbacks *owner = d->rtp->allocator.custom ? &d->rtp->allocator.callbacks : nullptr;
        if (!owner && d->instance_data && d->instance_data->has_allocator) owner = &d->instance_data->allocator;
        auto token = ScopedToken<LayoutToken>(owner, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, Allocator(a), v);
        token->public_layout = public_layout;
        try {
            auto result = d->rtp->layouts.emplace(VKRT_HKEY(public_layout), LayoutRecord{v, token});
            if (!result.second) {
                token->next = result.first->second.tokens;
                result.first->second.tokens = token;
            }
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
        std::shared_ptr<Layout> retired_canonical;
        if (d->rtp) {
            std::lock_guard<std::mutex> lock(d->rtp->lock);
            auto i = d->rtp->layouts.find(VKRT_HKEY(h));
            if (i != d->rtp->layouts.end()) {
                token = i->second.tokens;
                i->second.tokens = token->next;
                if (!i->second.tokens) {
                    retired_canonical = std::move(i->second.canonical);
                    d->rtp->layouts.erase(i);
                }
            }
            // Compatibility is semantic, not callback-structure identity. Use
            // this call's allocator; a previously retired alias's context may
            // already be gone. Bookkeeping itself has DEVICE lifetime.
            if (h) d->vtable.DestroyPipelineLayout(device, h, a);
        } else if (h)
            d->vtable.DestroyPipelineLayout(device, h, a);
        // Private Layout destruction must not run under the metadata lock.
        FreeToken(token);
        return VK_SUCCESS;
    });
}
struct ShaderRewrite {
    VkShaderModuleCreateInfo info;
    const uint32_t *original;
    explicit ShaderRewrite(const VkShaderModuleCreateInfo &source) : info(source), original(source.pCode) {}
    ~ShaderRewrite() {
        if (info.pCode != original) std::free(const_cast<uint32_t *>(info.pCode));
    }
};
struct NativeStageModules {
    vkrt_device_data *dev;
    const VkAllocationCallbacks *allocator;
    std::vector<VkShaderModule> modules;
    ~NativeStageModules() {
        for (auto module : modules) dev->vtable.DestroyShaderModule(dev->device, module, allocator);
    }
    void AdmitInline(const VkPipelineShaderStageCreateInfo &stage) {
        if (stage.module || !dev->virtual_rt) return;
        vk_foreach_struct_const(item, stage.pNext) {
            if (item->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            auto source = *reinterpret_cast<const VkShaderModuleCreateInfo *>(item);
            // This pNext continues the stage chain, not a module creation chain.
            // Inspect a local copy; the native inline stage stays unchanged.
            source.pNext = nullptr;
            ShaderRewrite rewritten(source);
            bool query = false, pipeline = false;
            const auto result = vkrt_rewrite_shader_module(dev, &source, &rewritten.info, &query, &pipeline);
            if (query || pipeline) {
                VKRT_LOG_ERR("inline shader modules using software ray-query or ray-tracing source are not supported");
                throw Failure{VK_ERROR_FEATURE_NOT_PRESENT};
            }
            Check(result);
            return;
        }
    }
    VkPipelineShaderStageCreateInfo Translate(const VkPipelineShaderStageCreateInfo &stage) {
        AdmitInline(stage);
        VkShaderModule native{};
        if (vkrt_rtp_native_shader(dev, stage.module, &native)) {
            auto translated = stage;
            translated.module = native;
            return translated;
        }
        if (!RawShader(dev, stage.module)) return stage;
        Need(stage.pName != nullptr);
        const auto selected = SelectNativeShaderEntry(ShaderWords(dev, stage.module), stage.pName, stage.stage);
        VkShaderModuleCreateInfo source{};
        source.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        source.pCode = selected.data();
        source.codeSize = selected.size() * sizeof(uint32_t);
        ShaderRewrite rewritten(source);
        bool query = false, pipeline = false;
        Check(vkrt_rewrite_shader_module(dev, &source, &rewritten.info, &query, &pipeline));
        Need(!pipeline);
        VkShaderModule module{};
        const auto result = dev->vtable.CreateShaderModule(dev->device, &rewritten.info, allocator, &module);
        if (module) {
            try {
                modules.push_back(module);
            } catch (...) {
                dev->vtable.DestroyShaderModule(dev->device, module, allocator);
                throw;
            }
        }
        Check(result);
        Need(module != VK_NULL_HANDLE);
        auto translated = stage;
        translated.module = module;
        return translated;
    }
};
VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                      VkPipeline *out) {
    return Guard([&] {
        auto d = Device(device);
        std::fill(out, out + count, VK_NULL_HANDLE);
        std::vector<VkComputePipelineCreateInfo> copy(infos, infos + count);
        std::vector<std::shared_ptr<Layout>> refs;
        NativeStageModules modules{d, a, {}};
        for (auto &c : copy) {
            Need(!FindPipeline(d, c.basePipelineHandle));
            c.stage = modules.Translate(c.stage);
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
        NativeStageModules modules{d, a, {}};
        std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(count);
        for (uint32_t n = 0; n < count; ++n) {
            auto &c = copy[n];
            Need(!FindPipeline(d, c.basePipelineHandle));
            if (c.stageCount) stages[n].assign(c.pStages, c.pStages + c.stageCount);
            for (auto &stage : stages[n]) stage = modules.Translate(stage);
            c.pStages = stages[n].data();
            auto ref = FindLayout(d, c.layout);
            if (ref) {
                c.layout = ref->native;
                refs.push_back(ref);
            }
        }
        return d->vtable.CreateGraphicsPipelines(device, cache, count, copy.data(), a, out);
    });
}
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferred, VkPipelineCache cache,
                                                            uint32_t count, const VkRayTracingPipelineCreateInfoKHR *infos,
                                                            const VkAllocationCallbacks *a, VkPipeline *out) {
    std::fill(out, out + count, VK_NULL_HANDLE);
    return Guard([&] {
        auto d = Device(device);
        Need(d->rtp && vkrt_deferred_valid(d, deferred));
        VkResult overall = VK_SUCCESS;
        for (uint32_t n = 0; n < count; ++n) {
            VkResult one = Guard([&] {
                auto ci = infos[n];
                constexpr VkPipelineCreateFlags supported = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
                                                            VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
                                                            VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
                                                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_ANY_HIT_SHADERS_BIT_KHR |
                                                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR |
                                                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_INTERSECTION_SHADERS_BIT_KHR |
                                                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR |
                                                            VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
                                                            VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
                // Maintenance5 flags2 replaces, rather than augments, flags.
                // Inspect the full-width value before narrowing known bits.
                VkPipelineCreateFlags2KHR flags = ci.flags;
                bool has_flags2 = false;
                for (auto next = static_cast<const VkBaseInStructure *>(ci.pNext); next; next = next->pNext) {
                    Need(next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR && !has_flags2);
                    flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfoKHR *>(next)->flags;
                    has_flags2 = true;
                }
                Need(!(flags & ~VkPipelineCreateFlags2KHR(supported)));
                ci.flags = VkPipelineCreateFlags(flags);
                ci.pNext = nullptr;
                Need(!ci.pLibraryInfo || !ci.pLibraryInfo->pNext);
                Need(!ci.pLibraryInterface || !ci.pLibraryInterface->pNext);
                if (ci.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) {
                    std::shared_ptr<Pipeline> base;
                    if (ci.basePipelineHandle) {
                        Need(ci.basePipelineIndex == -1);
                        base = FindPipeline(d, ci.basePipelineHandle);
                    } else {
                        Need(ci.basePipelineIndex >= 0 && uint32_t(ci.basePipelineIndex) < n);
                        base = FindPipeline(d, out[ci.basePipelineIndex]);
                    }
                    Need(base && (base->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT));
                    // Derivation supplies an optimization opportunity, not
                    // inherited state or a lifetime dependency. Compile the
                    // fully specified child independently of its parent.
                }
                auto p = std::make_shared<Pipeline>();
                p->dev = d;
                p->flags = ci.flags;
                p->layout = FindLayout(d, ci.layout);
                Need(p->layout != nullptr);
                p->depth = ci.maxPipelineRayRecursionDepth;
                p->has_interface = ci.pLibraryInterface != nullptr;
                if (ci.pLibraryInterface) {
                    p->max_payload = ci.pLibraryInterface->maxPipelineRayPayloadSize;
                    p->max_attribute = ci.pLibraryInterface->maxPipelineRayHitAttributeSize;
                }
                Need(!(ci.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || p->has_interface);
                p->identity = d->rtp->next_identity.fetch_add(1);
                Need(p->identity != 0);
                if (ci.pDynamicState) {
                    Need(!ci.pDynamicState->pNext);
                    for (uint32_t k = 0; k < ci.pDynamicState->dynamicStateCount; ++k) {
                        Need(ci.pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR);
                        p->dynamic_stack = true;
                    }
                }
                if (ci.groupCount) p->groups.assign(ci.pGroups, ci.pGroups + ci.groupCount);
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
                // Vulkan implicitly appends each library's complete stage and
                // group lists in pLibraries order. A library already owns its
                // flattened dependencies; repeated references deliberately
                // produce repeated lists, rather than a deduplicated DAG.
                if (ci.pLibraryInfo) {
                    for (uint32_t i = 0; i < ci.pLibraryInfo->libraryCount; ++i) {
                        auto library = FindPipeline(d, ci.pLibraryInfo->pLibraries[i]);
                        Need(library && (library->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) && p->has_interface &&
                             library->depth == p->depth && library->max_payload == p->max_payload &&
                             library->max_attribute == p->max_attribute);
                        constexpr VkPipelineCreateFlags no_null =
                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_ANY_HIT_SHADERS_BIT_KHR |
                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR |
                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_INTERSECTION_SHADERS_BIT_KHR |
                            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR;
                        Need((library->flags & (ci.flags & no_null)) == (ci.flags & no_null));
                        Need(library->metadata.size() <= UINT32_MAX - p->metadata.size() &&
                             library->groups.size() <= UINT32_MAX - p->groups.size());
                        const uint32_t base = uint32_t(p->metadata.size());
                        p->metadata.insert(p->metadata.end(), library->metadata.begin(), library->metadata.end());
                        p->kinds.insert(p->kinds.end(), library->kinds.begin(), library->kinds.end());
                        p->entries.insert(p->entries.end(), library->entries.begin(), library->entries.end());
                        for (auto group : library->groups) {
                            for (uint32_t *stage :
                                 {&group.generalShader, &group.closestHitShader, &group.anyHitShader, &group.intersectionShader})
                                if (*stage != VK_SHADER_UNUSED_KHR) {
                                    Need(*stage < library->metadata.size());
                                    *stage += base;
                                }
                            p->groups.push_back(group);
                        }
                    }
                }
                const auto arena = PlanTypedRuntimeArena(p->metadata, p->groups, p->depth);
                // Application cache access is confined to this creation call.
                // Runtime stack budgets do not specialize the native program.
                // Libraries may omit raygen, all groups, or their own stages.
                // They retain typed source/interface definitions, never native
                // RT pipelines or caller-owned shader/cache/library handles.
                if (ci.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)
                    p->library_stack_bytes = arena.stage_stack_bytes;
                else
                    p->initial = Build(*p, cache);
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
        if (token) {
            vkrt_debug_object_forget(d, VK_OBJECT_TYPE_PIPELINE, VKRT_HKEY(h));
            FreeToken(token);
        } else if (h)
            d->vtable.DestroyPipeline(device, h, a);
        return VK_SUCCESS;
    });
}
VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingShaderGroupHandlesKHR(VkDevice device, VkPipeline h, uint32_t first, uint32_t count,
                                                                  size_t bytes, void *data) {
    return Guard([&] {
        auto p = FindPipeline(Device(device), h);
        Need(p != nullptr && p->initial != nullptr);
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
            const auto &sizes = p->initial ? p->initial->program.stage_stack_bytes : p->library_stack_bytes;
            Need(s < sizes.size());
            bytes = sizes[s];
        }
        return VK_SUCCESS;
    });
    return bytes;
}
void CaptureBindings(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, const std::shared_ptr<Layout> &owner,
                     VkPipelineLayout native, uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count,
                     const uint32_t *offsets) {
    if (!cb->device_data->rtp || (point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR && point != VK_PIPELINE_BIND_POINT_COMPUTE))
        return;
    if (State(cb).restoring_descriptors) return;
    Binding binding;
    binding.owner = owner;
    binding.native = native;
    binding.first = first;
    if (count) binding.sets.assign(sets, sets + count);
    binding.live.resize(count, true);
    if (dynamic_count) binding.offsets.assign(offsets, offsets + dynamic_count);
    auto &state = State(cb);
    auto &history = point == VK_PIPELINE_BIND_POINT_COMPUTE ? state.compute_bindings : state.rt_bindings;
    for (auto &old : history)
        for (size_t slot = 0; slot < old.live.size(); ++slot)
            if (uint64_t(old.first) + slot >= first && uint64_t(old.first) + slot < uint64_t(first) + count) old.live[slot] = false;
    history.push_back(std::move(binding));
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                                                 uint32_t first, uint32_t count, const VkDescriptorSet *sets,
                                                 uint32_t dynamic_count, const uint32_t *offsets) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb) return;
    VkResult result = Guard([&] {
        auto d = cb->device_data;
        auto owner = FindCommandLayout(cb, layout);
        if (cb->replaying) Need(owner != nullptr);
        VkPipelineLayout native = owner ? owner->native : layout;
        CaptureBindings(cb, point, owner, native, first, count, sets, dynamic_count, offsets);
        if (point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
            RecordSets(cb, point, layout, first, count, sets, dynamic_count, offsets);
        if (point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
            d->vtable.CmdBindDescriptorSets(command, point, native, first, count, sets, dynamic_count, offsets);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets2(VkCommandBuffer command, const VkBindDescriptorSetsInfo *info) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb) return;
    VkResult result = Guard([&] {
        auto d = cb->device_data;
        Need(d->vtable.CmdBindDescriptorSets2 != nullptr);
        if (!d->rtp) {
            d->vtable.CmdBindDescriptorSets2(command, info);
            return VK_SUCCESS;
        }
        Need(info && info->sType == VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO);
        const VkPipelineLayoutCreateInfo *inline_layout = nullptr;
        for (auto next = static_cast<const VkBaseInStructure *>(info->pNext); next; next = next->pNext) {
            Need(next->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO && !inline_layout);
            inline_layout = reinterpret_cast<const VkPipelineLayoutCreateInfo *>(next);
        }
        VkPipelineLayout layout = info->layout;
        struct TemporaryLayout {
            VkDevice device;
            VkPipelineLayout handle{};
            ~TemporaryLayout() {
                if (handle) DestroyPipelineLayout(device, handle, nullptr);
            }
        } temporary{d->device};
        if (!layout) {
            Need(inline_layout != nullptr);
            auto ci = *inline_layout;
            ci.pNext = nullptr;
            Check(CreatePipelineLayout(d->device, &ci, nullptr, &temporary.handle));
            layout = temporary.handle;
        }
        auto owner = temporary.handle ? FindLayout(d, temporary.handle) : FindCommandLayout(cb, layout);
        Need(owner != nullptr);
        bool all = info->stageFlags == VK_SHADER_STAGE_ALL;
        if (all || (info->stageFlags & rt_bits))
            CaptureBindings(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, owner, owner->native, info->firstSet,
                            info->descriptorSetCount, info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
        if (all || (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT))
            CaptureBindings(cb, VK_PIPELINE_BIND_POINT_COMPUTE, owner, owner->native, info->firstSet, info->descriptorSetCount,
                            info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
        auto native = *info;
        // ALL is a valid special enum, rather than every undefined flag bit.
        // Preserve it as for legacy ALL push constants. Explicit RT stages
        // are consumed, and unrelated native bind points are forwarded once.
        native.stageFlags = all ? VK_SHADER_STAGE_ALL : info->stageFlags & ~rt_bits;
        native.layout = owner->native;
        native.pNext = nullptr;
        if (native.stageFlags) {
            auto identity = layout;
            if (temporary.handle && cb->replaying) Check(vkrt_rtp_replay_layout_capture(cb, temporary.handle, &identity));
            auto saved = native;
            saved.layout = identity;
            RecordDescriptor(cb, [&](vkrt_replay_context *context) {
                auto capture =
                    reinterpret_cast<PFN_vkrt_capture_CmdBindDescriptorSets2>(vkrt_replay_capture_proc("vkCmdBindDescriptorSets2"));
                Need(capture != nullptr);
                return capture(context, &saved);
            });
            d->vtable.CmdBindDescriptorSets2(command, &native);
        }
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
struct DescriptorLayoutUse {
    vkrt_cmd_buffer_data *cb;
    VkPipelineLayout temporary{}, identity{}, native{};
    std::shared_ptr<Layout> owner;
    DescriptorLayoutUse(vkrt_cmd_buffer_data *command, VkPipelineLayout layout, const void *next = nullptr) : cb(command) {
        const VkPipelineLayoutCreateInfo *inline_layout = nullptr;
        for (auto p = static_cast<const VkBaseInStructure *>(next); p; p = p->pNext) {
            Need(p->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO && !inline_layout);
            inline_layout = reinterpret_cast<const VkPipelineLayoutCreateInfo *>(p);
        }
        identity = layout;
        if (!identity) {
            Need(inline_layout != nullptr);
            auto ci = *inline_layout;
            ci.pNext = nullptr;
            Check(CreatePipelineLayout(cb->device_data->device, &ci, nullptr, &temporary));
            identity = temporary;
            // A constructor failure must still release its temporary public
            // handle; the recorded native layout is retained independently.
            auto result = Guard([&] {
                if (cb->replaying) Check(vkrt_rtp_replay_layout_capture(cb, temporary, &identity));
                owner = cb->replaying ? FindCommandLayout(cb, identity) : FindLayout(cb->device_data, temporary);
                Need(owner != nullptr);
                if (!State(cb).restoring_descriptors) State(cb).retained_layouts.push_back(owner);
                return VK_SUCCESS;
            });
            if (result != VK_SUCCESS) {
                DestroyPipelineLayout(cb->device_data->device, temporary, nullptr);
                temporary = VK_NULL_HANDLE;
                Check(result);
            }
        } else
            owner = FindCommandLayout(cb, identity);
        Need(owner != nullptr);
        native = owner->native;
        if (!temporary && !State(cb).restoring_descriptors) State(cb).retained_layouts.push_back(owner);
    }
    ~DescriptorLayoutUse() {
        if (temporary) DestroyPipelineLayout(cb->device_data->device, temporary, nullptr);
    }
};
void RecordPush(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set, uint32_t count,
                const VkWriteDescriptorSet *writes) {
    RecordDescriptor(cb, [&](vkrt_replay_context *context) {
        auto capture =
            reinterpret_cast<PFN_vkrt_capture_CmdPushDescriptorSetKHR>(vkrt_replay_capture_proc("vkCmdPushDescriptorSetKHR"));
        Need(capture != nullptr);
        return capture(context, point, layout, set, count, writes);
    });
}
VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSet(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
                                                uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdPushDescriptorSetKHR>(d, "vkCmdPushDescriptorSet", "vkCmdPushDescriptorSetKHR");
        if (!d->rtp) {
            call(command, point, layout, set, count, writes);
            return VK_SUCCESS;
        }
        Need(point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
        DescriptorLayoutUse use(cb, layout);
        RecordPush(cb, point, use.identity, set, count, writes);
        call(command, point, use.native, set, count, writes);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSet2(VkCommandBuffer command, const VkPushDescriptorSetInfo *info) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdPushDescriptorSet2>(d, "vkCmdPushDescriptorSet2", "vkCmdPushDescriptorSet2KHR");
        if (!d->rtp) {
            call(command, info);
            return VK_SUCCESS;
        }
        Need(info != nullptr);
        bool all = info->stageFlags == VK_SHADER_STAGE_ALL;
        Need(all || !(info->stageFlags & rt_bits));
        DescriptorLayoutUse use(cb, info->layout, info->pNext);
        auto saved = *info;
        saved.layout = use.identity;
        saved.pNext = nullptr;
        RecordDescriptor(cb, [&](vkrt_replay_context *context) {
            auto capture =
                reinterpret_cast<PFN_vkrt_capture_CmdPushDescriptorSet2>(vkrt_replay_capture_proc("vkCmdPushDescriptorSet2"));
            Need(capture != nullptr);
            return capture(context, &saved);
        });
        auto native = *info;
        native.pNext = nullptr;
        native.layout = use.native;
        call(command, &native);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorBuffersEXT(VkCommandBuffer command, uint32_t count,
                                                       const VkDescriptorBufferBindingInfoEXT *bindings) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdBindDescriptorBuffersEXT>(d, "vkCmdBindDescriptorBuffersEXT");
        RecordDescriptor(cb, [&](vkrt_replay_context *context) {
            auto capture = reinterpret_cast<PFN_vkrt_capture_CmdBindDescriptorBuffersEXT>(
                vkrt_replay_capture_proc("vkCmdBindDescriptorBuffersEXT"));
            Need(capture != nullptr);
            return capture(context, count, bindings);
        });
        call(command, count, bindings);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
void RecordOffsets(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count,
                   const uint32_t *indices, const VkDeviceSize *offsets) {
    RecordDescriptor(cb, [&](vkrt_replay_context *context) {
        auto capture = reinterpret_cast<PFN_vkrt_capture_CmdSetDescriptorBufferOffsetsEXT>(
            vkrt_replay_capture_proc("vkCmdSetDescriptorBufferOffsetsEXT"));
        Need(capture != nullptr);
        return capture(context, point, layout, first, count, indices, offsets);
    });
}
VKAPI_ATTR void VKAPI_CALL CmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer command, VkPipelineBindPoint point,
                                                            VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                            const uint32_t *indices, const VkDeviceSize *offsets) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(d, "vkCmdSetDescriptorBufferOffsetsEXT");
        if (!d->rtp) {
            call(command, point, layout, first, count, indices, offsets);
            return VK_SUCCESS;
        }
        Need(point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
        DescriptorLayoutUse use(cb, layout);
        RecordOffsets(cb, point, use.identity, first, count, indices, offsets);
        call(command, point, use.native, first, count, indices, offsets);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
VKAPI_ATTR void VKAPI_CALL CmdSetDescriptorBufferOffsets2EXT(VkCommandBuffer command,
                                                             const VkSetDescriptorBufferOffsetsInfoEXT *info) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdSetDescriptorBufferOffsets2EXT>(d, "vkCmdSetDescriptorBufferOffsets2EXT");
        if (!d->rtp) {
            call(command, info);
            return VK_SUCCESS;
        }
        Need(info != nullptr);
        bool all = info->stageFlags == VK_SHADER_STAGE_ALL;
        Need(all || !(info->stageFlags & rt_bits));
        DescriptorLayoutUse use(cb, info->layout, info->pNext);
        auto saved = *info;
        saved.layout = use.identity;
        saved.pNext = nullptr;
        RecordDescriptor(cb, [&](vkrt_replay_context *context) {
            auto capture = reinterpret_cast<PFN_vkrt_capture_CmdSetDescriptorBufferOffsets2EXT>(
                vkrt_replay_capture_proc("vkCmdSetDescriptorBufferOffsets2EXT"));
            Need(capture != nullptr);
            return capture(context, &saved);
        });
        auto native = *info;
        native.pNext = nullptr;
        native.layout = use.native;
        call(command, &native);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
void RecordSamplers(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set) {
    RecordDescriptor(cb, [&](vkrt_replay_context *context) {
        auto capture = reinterpret_cast<PFN_vkrt_capture_CmdBindDescriptorBufferEmbeddedSamplersEXT>(
            vkrt_replay_capture_proc("vkCmdBindDescriptorBufferEmbeddedSamplersEXT"));
        Need(capture != nullptr);
        return capture(context, point, layout, set);
    });
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorBufferEmbeddedSamplersEXT(VkCommandBuffer command, VkPipelineBindPoint point,
                                                                      VkPipelineLayout layout, uint32_t set) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT>(d, "vkCmdBindDescriptorBufferEmbeddedSamplersEXT");
        if (!d->rtp) {
            call(command, point, layout, set);
            return VK_SUCCESS;
        }
        Need(point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
        DescriptorLayoutUse use(cb, layout);
        RecordSamplers(cb, point, use.identity, set);
        call(command, point, use.native, set);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorBufferEmbeddedSamplers2EXT(VkCommandBuffer command,
                                                                       const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *info) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    auto result = Guard([&] {
        auto d = cb->device_data;
        auto call = Native<PFN_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT>(d, "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT");
        if (!d->rtp) {
            call(command, info);
            return VK_SUCCESS;
        }
        Need(info != nullptr);
        bool all = info->stageFlags == VK_SHADER_STAGE_ALL;
        Need(all || !(info->stageFlags & rt_bits));
        DescriptorLayoutUse use(cb, info->layout, info->pNext);
        auto saved = *info;
        saved.layout = use.identity;
        saved.pNext = nullptr;
        RecordDescriptor(cb, [&](vkrt_replay_context *context) {
            auto capture = reinterpret_cast<PFN_vkrt_capture_CmdBindDescriptorBufferEmbeddedSamplers2EXT>(
                vkrt_replay_capture_proc("vkCmdBindDescriptorBufferEmbeddedSamplers2EXT"));
            Need(capture != nullptr);
            return capture(context, &saved);
        });
        auto native = *info;
        native.pNext = nullptr;
        native.layout = use.native;
        call(command, &native);
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) cb->record_result = result;
}
VKAPI_ATTR void VKAPI_CALL CmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer command, uint32_t size) {
    auto cb = VKRT_FIND(vkrt_cmd_buffer_data, command);
    if (!cb) return;
    VkResult r = Guard([&] {
        State(cb).stack = size;
        State(cb).stack_set = true;
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
        const uint64_t default_bytes = PlanTypedRuntimeArenaBytes(p->metadata, p->groups, p->depth);
        const uint64_t budget = p->dynamic_stack && state.stack_set ? state.stack : default_bytes;
        Need(budget != 0);
        RtpLaunchPlan plan;
        if (!PlanRtpLaunch(width, height, depth, budget, d->rtp->max_dispatch, p->metadata.size(), p->layout->push_bytes,
                           VEL_RT_RUNTIME_CONTEXT_BYTES, plan))
            throw Failure{VK_ERROR_OUT_OF_DEVICE_MEMORY};
        auto v = Build(*p);
        const VkStridedDeviceAddressRegionKHR *regions[] = {rg, miss, hit, call};
        for (const auto *region : regions) Need(region != nullptr);
        for (size_t s = 0; s < p->metadata.size(); ++s) {
            if (p->metadata[s].uses_app_push_constants) {
                auto source = state.push_layouts.find(p->kinds[s]);
                Need(source != state.push_layouts.end() && PushCompatible(*source->second, *p->layout));
            }
        }
        for (const auto &b : state.rt_bindings) {
            uint32_t last = UINT32_MAX;
            for (uint32_t s = 0; s < b.live.size(); ++s)
                if (b.live[s] && v->used_sets.count(b.first + s)) last = s;
            if (last != UINT32_MAX && b.owner) Need(Compatible(*b.owner, *p->layout, b.first + last));
        }
        // The journal captures the public trace and preceding binding/push
        // state. Only per-execution replay creates mutable arena leases.
        if (!cb->replaying) {
            cb->has_rtp_execution = true;
            return VK_SUCCESS;
        }
        auto packet = std::make_unique<Packet>();
        packet->dev = d;
        packet->pipeline = p;
        packet->variant = v;
        Allocate(*packet, plan);
        for (size_t s = 0; s < p->metadata.size(); ++s) {
            Write(*packet, plan.push_table + s * 8, packet->address + plan.push_payload + s * plan.push_stride);
            auto i = state.pushes.find(p->kinds[s]);
            if (i != state.pushes.end())
                std::memcpy((uint8_t *)packet->mapping + plan.push_payload + s * plan.push_stride, i->second.data(),
                            std::min(size_t(plan.push_stride), i->second.size()));
        }
        Write(*packet, VEL_RT_RUN_MAGIC, uint32_t(VEL_RT_RUNTIME_MAGIC));
        Write(*packet, VEL_RT_RUN_VERSION, uint32_t(VEL_RT_RUNTIME_VERSION));
        Write(*packet, VEL_RT_RUN_POOL_ADDRESS, packet->arena.address);
        Write(*packet, VEL_RT_RUN_POOL_BYTES, plan.arena_bytes);
        Write(*packet, VEL_RT_RUN_ARENA_BYTES, plan.arena_stride);
        Write(*packet, VEL_RT_RUN_INVOCATION_COUNT, uint64_t(plan.tile_invocations));
        Write(*packet, VEL_RT_RUN_WIDTH, width);
        Write(*packet, VEL_RT_RUN_HEIGHT, height);
        Write(*packet, VEL_RT_RUN_DEPTH, depth);
        Write(*packet, VEL_RT_RUN_LAUNCH_BASE, uint64_t(0));
        Write(*packet, VEL_RT_RUN_AS_REGISTRY, vkrt_as_registry_address(d));
        Write(*packet, VEL_RT_RUN_PUSH_TABLE, packet->address + plan.push_table);
        Write(*packet, VEL_RT_RUN_PIPELINE_ID, p->identity);
        const uint32_t starts[] = {80, 104, 128, 152};
        for (unsigned i = 0; i < 4; ++i) {
            Write(*packet, starts[i], regions[i]->deviceAddress);
            Write(*packet, starts[i] + 8, i == 0 ? regions[i]->size : regions[i]->stride);
            Write(*packet, starts[i] + 16, regions[i]->size);
        }
        // Preserve ownership before recording native commands referencing it.
        Packet *owned = packet.get();
        state.packets.push_back(std::move(packet));
        for (const auto &b : state.rt_bindings) {
            bool used = false;
            for (uint32_t s = 0; s < b.live.size(); ++s) used |= b.live[s] && v->used_sets.count(b.first + s);
            if (used) Bind(d, command, b, true);
        }
        VkBufferMemoryBarrier context_barrier{};
        context_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        context_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        context_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        context_barrier.srcQueueFamilyIndex = context_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        context_barrier.buffer = owned->context.buffer;
        context_barrier.offset = owned->context.offset;
        context_barrier.size = plan.context_bytes;
        d->vtable.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &context_barrier, 0, nullptr);
        d->vtable.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, v->native);
        for (uint64_t base = 0; base < plan.invocations;) {
            if (base) {
                VkBufferMemoryBarrier arena_barrier{};
                arena_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                arena_barrier.srcAccessMask = arena_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                arena_barrier.srcQueueFamilyIndex = arena_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                arena_barrier.buffer = owned->arena.buffer;
                arena_barrier.offset = owned->arena.offset;
                arena_barrier.size = plan.arena_bytes;
                d->vtable.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                             0, nullptr, 1, &arena_barrier, 0, nullptr);
            }
            const uint32_t count = RtpLaunchTileCount(plan, base);
            const uint32_t words[] = {uint32_t(owned->address), uint32_t(owned->address >> 32), uint32_t(base),
                                      uint32_t(base >> 32)};
            d->vtable.CmdPushConstants(command, p->layout->rt_native, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(words), words);
            d->vtable.CmdDispatch(command, count, 1, 1);
            base += count;  // count <= invocations - base by the checked planner.
        }
        vkrt_replay_compute_binding(cb);
        if (state.descriptors) {
            struct RestoreGuard {
                bool &value;
                explicit RestoreGuard(bool &v) : value(v) { value = true; }
                ~RestoreGuard() { value = false; }
            } restoring(state.restoring_descriptors);
            Check(vkrt_replay_emit(
                state.descriptors.get(), command, [](void *, const char *name) { return vkrt_rtp_proc(name); }, cb, nullptr));
            Check(cb->record_result);
        } else
            for (const auto &b : state.compute_bindings) Bind(d, command, b);
        vkrt_replay_push_constants(cb);
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
    for (auto p : s->pipelines) {
        vkrt_debug_object_forget(dev, VK_OBJECT_TYPE_PIPELINE, p.first);
        FreeToken(p.second);
    }
    for (auto p : s->shaders) {
        vkrt_debug_object_forget(dev, VK_OBJECT_TYPE_SHADER_MODULE, p.first);
        FreeShader(dev, p.second, p.second->allocator);
    }
    for (auto &p : s->layouts) {
        for (auto token = p.second.tokens; token;) {
            auto next = token->next;
            dev->vtable.DestroyPipelineLayout(dev->device, token->public_layout,
                                              token->public_allocator.custom ? &token->public_allocator.callbacks : nullptr);
            FreeToken(token);
            token = next;
        }
    }
    Allocator a = s->allocator;
    s->~vkrt_rtp_state();
    a.free(s);
    dev->rtp = nullptr;
}
extern "C" VkResult vkrt_rtp_track_descriptor_layout(vkrt_device_data *dev, VkDescriptorSetLayout layout,
                                                     const VkDescriptorSetLayoutCreateInfo *info) {
    return Guard([&] {
        if (!dev->rtp) return VK_SUCCESS;
        auto value = std::make_shared<DescriptorLayout>();
        value->flags = info->flags;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = nullptr;
        const VkMutableDescriptorTypeCreateInfoEXT *mutable_info = nullptr;
        for (auto next = static_cast<const VkBaseInStructure *>(info->pNext); next; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
                flags = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo *>(next);
            else if (next->sType == VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT)
                mutable_info = reinterpret_cast<const VkMutableDescriptorTypeCreateInfoEXT *>(next);
            else
                // Preserve ordinary native creation of future extensions. An
                // unrecognized definition can only prove identity with its own
                // retained snapshot, never with another unknown definition.
                value->known = false;
        }
        for (uint32_t i = 0; i < info->bindingCount; ++i) {
            const auto &source = info->pBindings[i];
            DescriptorBinding binding{source.binding, source.descriptorCount, source.descriptorType, source.stageFlags, 0, {}, {}};
            if (flags && i < flags->bindingCount) binding.flags = flags->pBindingFlags[i];
            if (source.descriptorCount && source.pImmutableSamplers &&
                (source.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                 source.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
                binding.samplers.assign(source.pImmutableSamplers, source.pImmutableSamplers + source.descriptorCount);
            if (source.descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_EXT && mutable_info &&
                i < mutable_info->mutableDescriptorTypeListCount) {
                const auto &types = mutable_info->pMutableDescriptorTypeLists[i];
                if (types.descriptorTypeCount)
                    binding.mutable_types.assign(types.pDescriptorTypes, types.pDescriptorTypes + types.descriptorTypeCount);
                std::sort(binding.mutable_types.begin(), binding.mutable_types.end());
            }
            value->bindings.push_back(std::move(binding));
        }
        std::sort(value->bindings.begin(), value->bindings.end(),
                  [](const auto &a, const auto &b) { return std::tie(a.binding, a.stages) < std::tie(b.binding, b.stages); });
        std::lock_guard<std::mutex> lock(dev->rtp->lock);
        auto old = dev->rtp->descriptor_layouts.find(VKRT_HKEY(layout));
        if (old == dev->rtp->descriptor_layouts.end()) {
            dev->rtp->descriptor_layouts.emplace(VKRT_HKEY(layout), DescriptorLayoutRecord{std::move(value), 1});
        } else {
            if (old->second.references == UINT64_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
            if (!(*old->second.definition == *value)) {
                // Different public definitions may lower to the same native
                // handle. Keep every existing pipeline snapshot immutable;
                // this new snapshot cannot prove compatibility by definition.
                // Distinguishing their public identities needs proxy handles.
                value->known = false;
                old->second.definition = std::move(value);
            }
            ++old->second.references;
        }
        return VK_SUCCESS;
    });
}
extern "C" void vkrt_rtp_forget_descriptor_layout(vkrt_device_data *dev, VkDescriptorSetLayout layout) {
    Guard([&] {
        if (dev->rtp) {
            std::lock_guard<std::mutex> lock(dev->rtp->lock);
            auto old = dev->rtp->descriptor_layouts.find(VKRT_HKEY(layout));
            if (old != dev->rtp->descriptor_layouts.end() && --old->second.references == 0) dev->rtp->descriptor_layouts.erase(old);
        }
        return VK_SUCCESS;
    });
}
extern "C" bool vkrt_rtp_layout_compatible(vkrt_device_data *dev, VkPipelineLayout a, VkPipelineLayout b, uint32_t last_set) {
    bool result = false;
    Guard([&] {
        auto first = FindLayout(dev, a), second = FindLayout(dev, b);
        result = first && second && Compatible(*first, *second, last_set);
        return VK_SUCCESS;
    });
    return result;
}
extern "C" VkResult vkrt_rtp_create_shader(vkrt_device_data *dev, const VkShaderModuleCreateInfo *ci,
                                           const VkAllocationCallbacks *a, VkShaderModule *out, bool *handled) {
    *handled = false;
    return Guard([&] {
        if (!dev->rtp) return VK_SUCCESS;
        Need(ci && ci->pCode && ci->codeSize >= 20 && !(ci->codeSize & 3));
        bool rt = false, banked = false;
        for (size_t p = 5; p < ci->codeSize / 4;) {
            uint32_t n = ci->pCode[p] >> 16;
            Need(n && n <= ci->codeSize / 4 - p);
            uint32_t op = ci->pCode[p] & 65535;
            if (op == spv::OpEntryPoint) {
                uint32_t model = ci->pCode[p + 1];
                rt |= model >= spv::ExecutionModelRayGenerationKHR && model <= spv::ExecutionModelCallableKHR;
            }
            // SPV_NV_push_constant_bank: the runtime's source snapshots use
            // bank zero only. Never silently flatten another source bank.
            banked |= op == spv::OpCapability && n >= 2 && ci->pCode[p + 1] == 5423;
            banked |= op == spv::OpDecorate && n >= 3 && ci->pCode[p + 2] == 5397;
            p += n;
        }
        if (!rt) return VK_SUCCESS;
        *handled = true;
        *out = VK_NULL_HANDLE;
        Need(!ci->pNext && !ci->flags && !banked);
        *out = RegisterShader(dev, Token<ShaderToken>(a, Words(ci->pCode, ci->pCode + ci->codeSize / 4)));
        return VK_SUCCESS;
    });
}
extern "C" VkResult vkrt_rtp_wrap_native_shader(vkrt_device_data *dev, VkShaderModule native,
                                                const VkAllocationCallbacks *allocator, VkShaderModule *out) {
    *out = VK_NULL_HANDLE;
    if (!dev->rtp) {
        *out = native;
        return VK_SUCCESS;
    }
    const VkResult result = Guard([&] {
        *out = RegisterShader(dev, Token<ShaderToken>(allocator, native));
        return VK_SUCCESS;
    });
    if (result != VK_SUCCESS) dev->vtable.DestroyShaderModule(dev->device, native, allocator);
    return result;
}
extern "C" bool vkrt_rtp_native_shader(vkrt_device_data *dev, VkShaderModule module, VkShaderModule *native) {
    if (!native) return false;
    *native = VK_NULL_HANDLE;
    if (!dev || !dev->rtp) return false;
    std::lock_guard<std::mutex> lock(dev->rtp->lock);
    auto i = dev->rtp->shaders.find(VKRT_HKEY(module));
    if (i == dev->rtp->shaders.end() || i->second->kind != ShaderToken::Kind::Native) return false;
    *native = i->second->native;
    return true;
}
extern "C" bool vkrt_rtp_destroy_shader(vkrt_device_data *dev, VkShaderModule h, const VkAllocationCallbacks *allocator) {
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
    if (token) {
        vkrt_debug_object_forget(dev, VK_OBJECT_TYPE_SHADER_MODULE, VKRT_HKEY(h));
        FreeShader(dev, token, Allocator(allocator));
    }
    return token != nullptr || status != VK_SUCCESS;
}
extern "C" bool vkrt_rtp_debug_owns(vkrt_device_data *dev, VkObjectType type, uint64_t handle) {
    if (!dev || !dev->rtp) return false;
    std::lock_guard<std::mutex> lock(dev->rtp->lock);
    if (type == VK_OBJECT_TYPE_PIPELINE) return dev->rtp->pipelines.count(handle) != 0;
    if (type == VK_OBJECT_TYPE_SHADER_MODULE) return dev->rtp->shaders.count(handle) != 0;
    return false;
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
extern "C" VkPipelineLayout vkrt_rtp_command_native_layout(vkrt_cmd_buffer_data *cb, VkPipelineLayout h) {
    VkPipelineLayout result = h;
    VkResult status = Guard([&] {
        auto owner = FindCommandLayout(cb, h);
        if (cb->replaying) Need(owner != nullptr);
        if (owner) result = owner->native;
        return VK_SUCCESS;
    });
    if (status != VK_SUCCESS) cb->record_result = status;
    return result;
}
extern "C" VkResult vkrt_rtp_replay_layout_capture(vkrt_cmd_buffer_data *cb, VkPipelineLayout h, VkPipelineLayout *identity) {
    if (!cb || !identity) return VK_ERROR_INITIALIZATION_FAILED;
    *identity = VK_NULL_HANDLE;
    return Guard([&] {
        auto owner = FindLayout(cb->device_data, h);
        Need(owner != nullptr);
        // This is a recording-local key, not a Vulkan object handle. The owner
        // remains live in the table, so its address cannot be reused there.
        auto key = Handle<VkPipelineLayout>(owner.get());
        State(cb).replay_layouts.emplace(VKRT_HKEY(key), std::move(owner));
        *identity = key;
        return VK_SUCCESS;
    });
}
extern "C" VkResult vkrt_rtp_replay_layouts_inherit(vkrt_cmd_buffer_data *target, const vkrt_cmd_buffer_data *source) {
    if (!target || !source || target->device_data != source->device_data || !target->replaying)
        return VK_ERROR_INITIALIZATION_FAILED;
    return Guard([&] {
        // This helper initializes one fresh recording, never a concatenation of
        // histories. Begin/reset must precede inheritance and command emission.
        Need(!target->rtp || target->rtp->replay_layouts.empty());
        if (source->rtp) State(target).replay_layouts = source->rtp->replay_layouts;
        return VK_SUCCESS;
    });
}
extern "C" VkResult vkrt_rtp_replay_layout_native(vkrt_cmd_buffer_data *cb, VkPipelineLayout identity,
                                                  VkPipelineLayout *native_layout) {
    if (!cb || !native_layout || !cb->replaying) return VK_ERROR_INITIALIZATION_FAILED;
    *native_layout = VK_NULL_HANDLE;
    return Guard([&] {
        auto owner = FindCommandLayout(cb, identity);
        Need(owner != nullptr);
        *native_layout = owner->native;
        return VK_SUCCESS;
    });
}
extern "C" bool vkrt_rtp_bind_pipeline(vkrt_cmd_buffer_data *cb, VkPipelineBindPoint point, VkPipeline h) {
    if (point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) return false;
    VkResult r = Guard([&] {
        State(cb).bound = FindPipeline(cb->device_data, h);
        Need(State(cb).bound != nullptr && State(cb).bound->initial != nullptr);
        return VK_SUCCESS;
    });
    if (r != VK_SUCCESS) cb->record_result = r;
    return true;
}
extern "C" VkShaderStageFlags vkrt_rtp_push(vkrt_cmd_buffer_data *cb, VkPipelineLayout h, VkShaderStageFlags stages,
                                            uint32_t offset, uint32_t size, const void *values) {
    if (!cb->device_data->rtp) return stages;
    VkResult r = Guard([&] {
        auto p = FindCommandLayout(cb, h);
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
    ENTRY(CmdBindDescriptorSets2);
    if (!std::strcmp(name, "vkCmdBindDescriptorSets2KHR")) return (PFN_vkVoidFunction)CmdBindDescriptorSets2;
    ENTRY(CmdPushDescriptorSet);
    if (!std::strcmp(name, "vkCmdPushDescriptorSetKHR")) return (PFN_vkVoidFunction)CmdPushDescriptorSet;
    ENTRY(CmdPushDescriptorSet2);
    if (!std::strcmp(name, "vkCmdPushDescriptorSet2KHR")) return (PFN_vkVoidFunction)CmdPushDescriptorSet2;
    ENTRY(CmdBindDescriptorBuffersEXT);
    ENTRY(CmdSetDescriptorBufferOffsetsEXT);
    ENTRY(CmdSetDescriptorBufferOffsets2EXT);
    ENTRY(CmdBindDescriptorBufferEmbeddedSamplersEXT);
    ENTRY(CmdBindDescriptorBufferEmbeddedSamplers2EXT);
    ENTRY(CmdSetRayTracingPipelineStackSizeKHR);
    ENTRY(CmdTraceRaysKHR);
#undef ENTRY
    return nullptr;
}
