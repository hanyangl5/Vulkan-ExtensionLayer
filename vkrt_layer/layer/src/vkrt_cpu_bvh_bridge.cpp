/* SPDX-License-Identifier: MIT
 * Test fixture bridge to the shared CPU reference encoder. This library is
 * never linked into the production Vulkan layer.
 */
#include "vkrt_cpu_bvh.h"
#include "mesa_ir_adapter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

extern "C" int vkrt_cpu_encode_mesa_bvh4(struct vkrt_cpu_bvh *temporary, uint32_t geometry_id, uint32_t geometry_flags,
                                         int top_level) {
    using namespace vel::rt::bvh4;
    try {
        std::vector<uint32_t> flags;
        if (!top_level) {
            flags.resize(size_t(geometry_id) + 1);
            flags[geometry_id] = geometry_flags;
        }
        /* Fixture TLAS inputs already contain shared-runtime BLAS addresses.
         * The explicit identity mapping is intentional, never format detection. */
        const auto adapted =
            AdaptMesaContainerV2({temporary->data, temporary->size}, flags, [](uint64_t address) { return address; });
        const auto encoded = Encode(adapted.tree, adapted.root);
        const auto layout = PlanLayout(encoded.nodes.size(), encoded.counts);
        if (layout.size > std::numeric_limits<size_t>::max()) throw std::overflow_error("CPU fixture size");
        std::unique_ptr<uint8_t, decltype(&std::free)> bytes(static_cast<uint8_t *>(std::calloc(1, size_t(layout.size))),
                                                             std::free);
        if (!bytes) throw std::bad_alloc();
        VelRtBvh4Header header{};
        header.magic = VEL_RT_BVH4_MAGIC;
        header.version = VEL_RT_BVH4_ABI_VERSION;
        header.type = top_level ? 0u : 1u; /* VkAccelerationStructureTypeKHR */
        header.root_id = encoded.root_id;
        header.node_count = uint32_t(encoded.nodes.size());
        header.triangle_count = uint32_t(adapted.triangles.size());
        header.aabb_count = uint32_t(adapted.aabbs.size());
        header.instance_count = uint32_t(adapted.instances.size());
        header.node_offset = layout.nodes;
        header.triangle_offset = layout.triangles;
        header.aabb_offset = layout.aabbs;
        header.instance_offset = layout.instances;
        header.leaf_parent_offset = layout.leaf_parents;
        header.total_size = layout.size;
        for (size_t axis = 0; axis < 3; ++axis) {
            header.bounds_min[axis] =
                adapted.tree.empty() ? std::numeric_limits<float>::infinity() : adapted.tree[adapted.root].bounds.min[axis];
            header.bounds_max[axis] =
                adapted.tree.empty() ? -std::numeric_limits<float>::infinity() : adapted.tree[adapted.root].bounds.max[axis];
        }
        std::memcpy(bytes.get(), &header, sizeof(header));
        auto copy = [&](uint64_t offset, const auto &source) {
            if (!source.empty()) std::memcpy(bytes.get() + offset, source.data(), source.size() * sizeof(source[0]));
        };
        copy(layout.nodes, encoded.nodes);
        copy(layout.triangles, adapted.triangles);
        copy(layout.aabbs, adapted.aabbs);
        copy(layout.instances, adapted.instances);
        copy(layout.leaf_parents, encoded.leaf_parents);
        std::free(temporary->data);
        temporary->data = bytes.release();
        temporary->size = size_t(layout.size);
        temporary->node_count = header.node_count;
        temporary->leaf_count = header.triangle_count + header.aabb_count + header.instance_count;
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "CPU BVH4 fixture encoding failed: %s\n", error.what());
        std::free(temporary->data);
        std::memset(temporary, 0, sizeof(*temporary));
        return -1;
    }
}
