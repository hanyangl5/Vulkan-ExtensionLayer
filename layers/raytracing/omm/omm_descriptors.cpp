// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_descriptors.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <unordered_map>

namespace vel::omm {
namespace {
constexpr VkResult invalid = VK_ERROR_VALIDATION_FAILED_EXT;
struct Native {
    VkDevice device{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<uint32_t> queue_families;
    VkAllocationCallbacks allocator{};
    bool custom = false;
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements requirements{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
    PFN_vkBindBufferMemory bind{};
    PFN_vkMapMemory map{};
    PFN_vkUnmapMemory unmap{};
    PFN_vkGetBufferDeviceAddress address{};
    const VkAllocationCallbacks* callbacks() const { return custom ? &allocator : nullptr; }
};
struct Storage {
    std::shared_ptr<Native> native;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    uint64_t bytes = 0, address = 0;
    ~Storage() {
        if (mapped) native->unmap(native->device, memory);
        if (buffer) native->destroy_buffer(native->device, buffer, native->callbacks());
        if (memory) native->free(native->device, memory, native->callbacks());
    }
};
VkResult make_storage(const std::shared_ptr<Native>& native, uint64_t bytes, std::shared_ptr<Storage>* output) {
    if (!bytes || bytes > SIZE_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto storage = std::make_shared<Storage>();
    storage->native = native;
    storage->bytes = bytes;
    VkBufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create.size = bytes;
    create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (native->queue_families.size() > 1) {
        create.sharingMode = VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = uint32_t(native->queue_families.size());
        create.pQueueFamilyIndices = native->queue_families.data();
    }
    VkResult result = native->create_buffer(native->device, &create, native->callbacks(), &storage->buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements{};
    native->requirements(native->device, storage->buffer, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < native->memory.memoryTypeCount; ++i) {
        const auto flags = native->memory.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &flags;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = native->allocate(native->device, &allocate, native->callbacks(), &storage->memory);
    if (result != VK_SUCCESS) return result;
    result = native->bind(native->device, storage->buffer, storage->memory, 0);
    if (result != VK_SUCCESS) return result;
    result = native->map(native->device, storage->memory, 0, bytes, 0, &storage->mapped);
    if (result != VK_SUCCESS) return result;
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = storage->buffer;
    storage->address = native->address(native->device, &address);
    if (!storage->address || (storage->address & 15) || bytes > UINT64_MAX - storage->address) return invalid;
    std::memset(storage->mapped, 0, size_t(bytes));
    *output = std::move(storage);
    return VK_SUCCESS;
}
struct Binding {
    uint32_t number = 0, count = 0;
    VkDescriptorType type{};
    VkShaderStageFlags stages = 0;
    VkDescriptorBindingFlags flags = 0;
    bool immutable = false;
    uint64_t record_base = 0;
};
struct Layout {
    uint32_t references = 1;
    VkDescriptorSetLayoutCreateFlags flags = 0;
    std::vector<Binding> bindings;
};
struct Set {
    VkDescriptorPool pool{};
    std::shared_ptr<Layout> layout;
    std::vector<Binding> bindings;
    std::vector<DescriptorASIdentity> identities;
    std::vector<DescriptorASIdentity> staging;
    bool staged = false;
    std::shared_ptr<Storage> storage;
    mutable std::mutex mutex;
};
using DescriptorSetMap = std::unordered_map<VkDescriptorSet, std::shared_ptr<Set>>;
struct Pool {
    VkDescriptorPoolCreateFlags flags = 0;
};
struct ContextEntry {
    uint32_t set, binding;
    std::shared_ptr<Set> source;
    uint64_t base, count;
};
const VkBaseInStructure* find_chain(const void* chain, VkStructureType type) {
    for (auto* item = static_cast<const VkBaseInStructure*>(chain); item; item = item->pNext)
        if (item->sType == type) return item;
    return nullptr;
}
// Flatten valid consecutive binding spans, including zero-count bindings. A
// numeric binding gap is not an implicit zero-count layout binding.
struct DescriptorSpan {
    const Set* set{};
    size_t binding = 0;
    uint32_t element = 0;
    VkDescriptorType type{};
    uint64_t next() {
        while (element == set->bindings[binding].count) {
            ++binding;
            element = 0;
        }
        return set->bindings[binding].record_base + element++;
    }
};
VkResult span(const Set& set, uint32_t number, uint32_t element, uint32_t count, DescriptorSpan* output) {
    auto first = std::lower_bound(set.bindings.begin(), set.bindings.end(), number,
                                  [](const Binding& a, uint32_t b) { return a.number < b; });
    if (first == set.bindings.end() || first->number != number || !first->count || !count) return invalid;
    *output = {&set, size_t(first - set.bindings.begin()), element, first->type};
    auto current = first;
    while (count) {
        if (current == set.bindings.end() || current->number != number || element > current->count) return invalid;
        if (current->count && (current->type != first->type || current->stages != first->stages || current->flags != first->flags ||
                               current->immutable != first->immutable))
            return invalid;
        const uint32_t take = std::min(count, current->count - element);
        count -= take;
        if (!count) break;
        if (number == UINT32_MAX) return invalid;
        ++number;
        ++current;
        element = 0;
    }
    return VK_SUCCESS;
}
}  // namespace

struct DescriptorContext {
    std::vector<std::shared_ptr<Set>> slots;
    std::vector<ContextEntry> entries;
    std::shared_ptr<Storage> storage;
};
struct DescriptorBackend {
    std::shared_ptr<Native> native;
    DescriptorASResolver resolver{};
    void* resolver_user_data{};
    std::mutex mutex;
    std::unordered_map<VkDescriptorSetLayout, std::shared_ptr<Layout>> layouts;
    std::unordered_map<VkDescriptorPool, Pool> pools;
    DescriptorSetMap sets;
    size_t allocation_reservations = 0;
    std::vector<std::weak_ptr<const DescriptorContext>> contexts;
};
struct DescriptorAllocation {
    DescriptorBackend* backend{};
    VkDescriptorPool pool{};
    VkDescriptorPoolCreateFlags pool_flags{};
    std::vector<DescriptorSetMap::node_type> nodes;
};

VkResult descriptor_backend_create(const DescriptorBackendCreateInfo& info, DescriptorBackend** output) {
    if (!output || !info.physical_device || !info.device || !info.get_device_proc_addr || !info.resolver) return invalid;
    try {
        auto backend = std::make_unique<DescriptorBackend>();
        backend->native = std::make_shared<Native>();
        auto& native = *backend->native;
        native.device = info.device;
        native.custom = info.allocator != nullptr;
        if (info.allocator) native.allocator = *info.allocator;
        const auto get_memory = info.get_memory_properties ? info.get_memory_properties : vkGetPhysicalDeviceMemoryProperties;
        const auto get_families =
            info.get_queue_family_properties ? info.get_queue_family_properties : vkGetPhysicalDeviceQueueFamilyProperties;
        get_memory(info.physical_device, &native.memory);
        uint32_t family_count = 0;
        get_families(info.physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        get_families(info.physical_device, &family_count, families.data());
        for (uint32_t i = 0; i < family_count; ++i)
            if (families[i].queueCount) native.queue_families.push_back(i);
#define OMM_LOAD(member, name)                                                                   \
    native.member = reinterpret_cast<PFN_##name>(info.get_device_proc_addr(info.device, #name)); \
    if (!native.member) return VK_ERROR_INITIALIZATION_FAILED
        OMM_LOAD(create_buffer, vkCreateBuffer);
        OMM_LOAD(destroy_buffer, vkDestroyBuffer);
        OMM_LOAD(requirements, vkGetBufferMemoryRequirements);
        OMM_LOAD(allocate, vkAllocateMemory);
        OMM_LOAD(free, vkFreeMemory);
        OMM_LOAD(bind, vkBindBufferMemory);
        OMM_LOAD(map, vkMapMemory);
        OMM_LOAD(unmap, vkUnmapMemory);
        OMM_LOAD(address, vkGetBufferDeviceAddress);
#undef OMM_LOAD
        backend->resolver = info.resolver;
        backend->resolver_user_data = info.resolver_user_data;
        *output = backend.release();
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void descriptor_backend_destroy(DescriptorBackend* backend) { delete backend; }

VkResult descriptor_track_layout(DescriptorBackend* backend, VkDescriptorSetLayout handle,
                                 const VkDescriptorSetLayoutCreateInfo* info) {
    if (!backend || !handle || !info || (info->bindingCount && !info->pBindings)) return invalid;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        auto existing = backend->layouts.find(handle);
        if (existing != backend->layouts.end()) {
            ++existing->second->references;
            return VK_SUCCESS;
        }
        auto layout = std::make_shared<Layout>();
        layout->flags = info->flags;
        const auto* binding_flags = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(
            find_chain(info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO));
        if (binding_flags && binding_flags->bindingCount &&
            (binding_flags->bindingCount != info->bindingCount || !binding_flags->pBindingFlags))
            return invalid;
        for (uint32_t i = 0; i < info->bindingCount; ++i) {
            const auto& source = info->pBindings[i];
            Binding binding{source.binding,
                            source.descriptorCount,
                            source.descriptorType,
                            source.stageFlags,
                            binding_flags && binding_flags->bindingCount ? binding_flags->pBindingFlags[i] : 0,
                            source.pImmutableSamplers != nullptr,
                            0};
            if (binding.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR &&
                ((binding.flags &
                  (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT)) ||
                 (info->flags & (VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR |
                                 VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT))))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (binding.type == VK_DESCRIPTOR_TYPE_MUTABLE_EXT) return VK_ERROR_FEATURE_NOT_PRESENT;
            layout->bindings.push_back(binding);
        }
        std::sort(layout->bindings.begin(), layout->bindings.end(),
                  [](const Binding& a, const Binding& b) { return a.number < b.number; });
        for (size_t i = 1; i < layout->bindings.size(); ++i)
            if (layout->bindings[i - 1].number == layout->bindings[i].number) return invalid;
        backend->layouts.emplace(handle, std::move(layout));
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void descriptor_untrack_layout(DescriptorBackend* backend, VkDescriptorSetLayout handle) {
    if (!backend) return;
    std::lock_guard<std::mutex> lock(backend->mutex);
    auto found = backend->layouts.find(handle);
    if (found != backend->layouts.end() && --found->second->references == 0) backend->layouts.erase(found);
}
VkResult descriptor_track_pool(DescriptorBackend* backend, VkDescriptorPool handle, const VkDescriptorPoolCreateInfo* info) {
    if (!backend || !handle || !info) return invalid;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        backend->pools[handle] = {info->flags};
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void descriptor_reset_pool(DescriptorBackend* backend, VkDescriptorPool handle) {
    if (!backend) return;
    std::lock_guard<std::mutex> lock(backend->mutex);
    for (auto item = backend->sets.begin(); item != backend->sets.end();)
        if (item->second->pool == handle)
            item = backend->sets.erase(item);
        else
            ++item;
}
void descriptor_untrack_pool(DescriptorBackend* backend, VkDescriptorPool handle) {
    if (!backend) return;
    descriptor_reset_pool(backend, handle);
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->pools.erase(handle);
}
VkResult descriptor_prepare_allocate_sets(DescriptorBackend* backend, const VkDescriptorSetAllocateInfo* info,
                                          DescriptorAllocation** output) {
    if (!backend || !info || !output || (info->descriptorSetCount && !info->pSetLayouts)) return invalid;
    *output = nullptr;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        const auto pool = backend->pools.find(info->descriptorPool);
        if (pool == backend->pools.end()) return invalid;
        auto transaction = std::make_unique<DescriptorAllocation>();
        transaction->backend = backend;
        transaction->pool = info->descriptorPool;
        transaction->pool_flags = pool->second.flags;
        transaction->nodes.reserve(info->descriptorSetCount);
        DescriptorSetMap staged;
        staged.reserve(info->descriptorSetCount);
        const auto* variable = reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(
            find_chain(info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO));
        if (variable && variable->descriptorSetCount &&
            (variable->descriptorSetCount != info->descriptorSetCount || !variable->pDescriptorCounts))
            return invalid;
        for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
            auto found = backend->layouts.find(info->pSetLayouts[i]);
            if (found == backend->layouts.end()) return invalid;
            auto set = std::make_shared<Set>();
            set->pool = info->descriptorPool;
            set->layout = found->second;
            set->bindings = found->second->bindings;
            uint64_t count = 0;
            for (auto& binding : set->bindings) {
                if (binding.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
                    const uint32_t requested = variable && variable->descriptorSetCount ? variable->pDescriptorCounts[i] : 0;
                    if (requested > binding.count || &binding != &set->bindings.back()) return invalid;
                    binding.count = requested;
                }
                binding.record_base = count;
                if (binding.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) count += binding.count;
            }
            if (count > SIZE_MAX / sizeof(DescriptorASIdentity) || count > UINT64_MAX / 16) return VK_ERROR_OUT_OF_HOST_MEMORY;
            set->identities.resize(size_t(count));
            set->staging.resize(size_t(count));
            // A reserved invalid pair distinguishes an unwritten descriptor
            // from an explicitly written valid robustness2 NULL pair {0,0}.
            for (auto& identity : set->identities) identity.native_address = UINT64_MAX;
            if (count) {
                VkResult result = make_storage(backend->native, count * 16, &set->storage);
                if (result != VK_SUCCESS) return result;
            }
            if (count) {
                auto* records = static_cast<DescriptorShadowRecord*>(set->storage->mapped);
                for (size_t item = 0; item < size_t(count); ++item) records[item] = {UINT64_MAX, 0};
            }
            // Allocate the exact node type/allocator used by the destination
            // map. The temporary numeric key is replaced after native success.
            VkDescriptorSet key{};
            const uint64_t bits = uint64_t(i) + 1;
            static_assert(sizeof(key) <= sizeof(bits));
            std::memcpy(&key, &bits, sizeof(key));
            auto inserted = staged.emplace(key, std::move(set));
            transaction->nodes.push_back(staged.extract(inserted.first));
        }
        const size_t maximum = backend->sets.max_size();
        if (backend->allocation_reservations > maximum - backend->sets.size() ||
            info->descriptorSetCount > maximum - backend->sets.size() - backend->allocation_reservations)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        const size_t reserved = backend->allocation_reservations + info->descriptorSetCount;
        // Only prepare can grow this map. Every outstanding token is included,
        // so another pool's prepare/commit cannot invalidate this reservation.
        backend->sets.reserve(backend->sets.size() + reserved);
        backend->allocation_reservations = reserved;
        *output = transaction.release();
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::length_error&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void descriptor_commit_allocate_sets(DescriptorAllocation* transaction, const VkDescriptorSet* handles) noexcept {
    if (!transaction) return;
    auto* backend = transaction->backend;
    {
        std::lock_guard<std::mutex> lock(backend->mutex);
        for (size_t i = 0; i < transaction->nodes.size(); ++i) {
            // Duplicate/null handles violate the native successful-allocation
            // contract. There is no fallible allocation or rollback here.
            if (!handles || !handles[i]) std::terminate();
            auto& node = transaction->nodes[i];
            node.key() = handles[i];
            if (!backend->sets.insert(std::move(node)).inserted) std::terminate();
        }
        backend->allocation_reservations -= transaction->nodes.size();
    }
    delete transaction;
}
void descriptor_abort_allocate_sets(DescriptorAllocation* transaction) noexcept {
    if (!transaction) return;
    {
        std::lock_guard<std::mutex> lock(transaction->backend->mutex);
        transaction->backend->allocation_reservations -= transaction->nodes.size();
    }
    delete transaction;
}
VkResult descriptor_allocate_sets(DescriptorBackend* backend, const VkDescriptorSetAllocateInfo* info,
                                  const VkDescriptorSet* handles) {
    if (!backend || !info || (info->descriptorSetCount && !handles)) return invalid;
    {
        std::lock_guard<std::mutex> lock(backend->mutex);
        for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
            if (!handles[i] || backend->sets.count(handles[i])) return invalid;
            for (uint32_t j = 0; j < i; ++j)
                if (handles[i] == handles[j]) return invalid;
        }
    }
    DescriptorAllocation* transaction{};
    const VkResult result = descriptor_prepare_allocate_sets(backend, info, &transaction);
    if (result != VK_SUCCESS) return result;
    descriptor_commit_allocate_sets(transaction, handles);
    return VK_SUCCESS;
}
void descriptor_free_sets(DescriptorBackend* backend, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet* handles) {
    if (!backend || (count && !handles)) return;
    std::lock_guard<std::mutex> lock(backend->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        auto found = backend->sets.find(handles[i]);
        if (found != backend->sets.end() && found->second->pool == pool) backend->sets.erase(found);
    }
}

VkResult descriptor_update(DescriptorBackend* backend, uint32_t write_count, const VkWriteDescriptorSet* writes,
                           uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    if (!backend || (write_count && !writes) || (copy_count && !copies)) return invalid;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        // Staging storage/ref slots are allocated before native set allocation.
        // Scanning the map replaces a fallible touched-set list. The guard also
        // discards every staged reference on invalid input/resolver failure.
        struct StagingReset {
            DescriptorBackend& backend;
            ~StagingReset() {
                for (const auto& item : backend.sets) {
                    auto& set = *item.second;
                    if (!set.staged) continue;
                    for (auto& identity : set.staging) identity = {};
                    set.staged = false;
                }
            }
        } cleanup{*backend};
        auto edit = [](Set& set) -> std::vector<DescriptorASIdentity>& {
            if (!set.staged) {
                std::copy(set.identities.begin(), set.identities.end(), set.staging.begin());
                set.staged = true;
            }
            return set.staging;
        };
        for (uint32_t i = 0; i < write_count; ++i) {
            const auto& write = writes[i];
            auto found = backend->sets.find(write.dstSet);
            if (found == backend->sets.end()) return invalid;
            DescriptorSpan destination_span;
            VkResult result =
                span(*found->second, write.dstBinding, write.dstArrayElement, write.descriptorCount, &destination_span);
            if (result != VK_SUCCESS || destination_span.type != write.descriptorType) return invalid;
            if (destination_span.type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) continue;
            const auto* structures = reinterpret_cast<const VkWriteDescriptorSetAccelerationStructureKHR*>(
                find_chain(write.pNext, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR));
            if (!structures || structures->accelerationStructureCount != write.descriptorCount ||
                !structures->pAccelerationStructures)
                return invalid;
            auto& destination = edit(*found->second);
            for (uint32_t j = 0; j < write.descriptorCount; ++j) {
                DescriptorASIdentity identity;
                if (structures->pAccelerationStructures[j]) {
                    result = backend->resolver(backend->resolver_user_data, structures->pAccelerationStructures[j], &identity);
                    if (result != VK_SUCCESS) return result;
                    if (!identity.native_address || !identity.root_cell_address || (identity.root_cell_address & 15))
                        return invalid;
                }
                destination[size_t(destination_span.next())] = std::move(identity);
            }
        }
        for (uint32_t i = 0; i < copy_count; ++i) {
            const auto& copy = copies[i];
            auto source = backend->sets.find(copy.srcSet), destination = backend->sets.find(copy.dstSet);
            if (source == backend->sets.end() || destination == backend->sets.end()) return invalid;
            DescriptorSpan from, to;
            if (span(*source->second, copy.srcBinding, copy.srcArrayElement, copy.descriptorCount, &from) != VK_SUCCESS ||
                span(*destination->second, copy.dstBinding, copy.dstArrayElement, copy.descriptorCount, &to) != VK_SUCCESS ||
                from.type != to.type)
                return invalid;
            if (from.type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) continue;
            if (source->second == destination->second) {
                auto source_first = from, destination_first = to;
                const uint64_t a = source_first.next(), b = destination_first.next();
                if (a < b + copy.descriptorCount && b < a + copy.descriptorCount) return invalid;
            }
            const auto& source_values = source->second->staged ? source->second->staging : source->second->identities;
            auto& destination_values = edit(*destination->second);
            for (uint32_t j = 0; j < copy.descriptorCount; ++j)
                destination_values[size_t(to.next())] = source_values[size_t(from.next())];
        }
        for (const auto& item : backend->sets) {
            auto& set = *item.second;
            if (!set.staged) continue;
            std::lock_guard<std::mutex> set_lock(set.mutex);
            set.identities.swap(set.staging);
            auto* records = static_cast<DescriptorShadowRecord*>(set.storage->mapped);
            for (size_t i = 0; i < set.identities.size(); ++i)
                records[i] = {set.identities[i].native_address, set.identities[i].root_cell_address};
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VkResult descriptor_build_context(DescriptorBackend* backend, const std::vector<VkDescriptorSet>& handles,
                                  DescriptorContextRef* output) {
    if (!backend || !output || handles.size() > UINT32_MAX) return invalid;
    try {
        std::lock_guard<std::mutex> lock(backend->mutex);
        auto context = std::make_shared<DescriptorContext>();
        context->slots.resize(handles.size());
        for (size_t i = 0; i < handles.size(); ++i) {
            if (!handles[i]) continue;
            auto found = backend->sets.find(handles[i]);
            if (found == backend->sets.end()) return invalid;
            context->slots[i] = found->second;
        }
        for (auto item = backend->contexts.begin(); item != backend->contexts.end();) {
            auto cached = item->lock();
            if (!cached) {
                item = backend->contexts.erase(item);
                continue;
            }
            if (cached->slots == context->slots) {
                *output = {cached->storage->address, cached};
                return VK_SUCCESS;
            }
            ++item;
        }
        for (size_t i = 0; i < context->slots.size(); ++i) {
            const auto& set = context->slots[i];
            if (!set) continue;
            for (const auto& binding : set->bindings)
                if (binding.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR && binding.count)
                    context->entries.push_back({uint32_t(i), binding.number, set, binding.record_base, binding.count});
        }
        if (context->entries.size() > UINT32_MAX || context->entries.size() > (SIZE_MAX - 32) / 32)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        const uint64_t bytes = 32 + uint64_t(context->entries.size()) * 32;
        VkResult result = make_storage(backend->native, bytes, &context->storage);
        if (result != VK_SUCCESS) return result;
        DescriptorContextHeader header{kDescriptorContextMagic, 1, uint32_t(context->entries.size()), 32, bytes, 0};
        std::memcpy(context->storage->mapped, &header, sizeof(header));
        auto* bindings = reinterpret_cast<DescriptorContextBinding*>(static_cast<uint8_t*>(context->storage->mapped) + 32);
        for (size_t i = 0; i < context->entries.size(); ++i) {
            const auto& entry = context->entries[i];
            bindings[i] = {entry.set, entry.binding, entry.source->storage->address + entry.base * 16, entry.count,
                           entry.count * 16};
        }
        backend->contexts.push_back(context);
        *output = {context->storage->address, std::move(context)};
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
VkResult descriptor_context_fetch(const DescriptorContextRef& context, uint32_t set, uint32_t binding, uint64_t index,
                                  DescriptorASIdentity* output) {
    if (!context.resource || !output || context.address != context.resource->storage->address) return invalid;
    for (const auto& entry : context.resource->entries) {
        if (entry.set != set || entry.binding != binding) continue;
        if (index >= entry.count) return invalid;
        std::lock_guard<std::mutex> lock(entry.source->mutex);
        const auto& identity = entry.source->identities[size_t(entry.base + index)];
        if ((!identity.native_address || !identity.root_cell_address) && (identity.native_address || identity.root_cell_address))
            return invalid;
        *output = identity;
        return VK_SUCCESS;
    }
    return invalid;
}
}  // namespace vel::omm
