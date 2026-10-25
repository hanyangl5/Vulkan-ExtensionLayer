// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "mesa_ir_adapter.h"

#include "../../../vkrt_layer/layer/bvh/bvh_ir_contract.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace vel::rt::bvh4 {
namespace {
constexpr uint32_t kMesaTriangle = 0, kMesaBox = 1, kMesaInstance = 2, kMesaAabb = 3;
constexpr uint32_t kMesaOffsetMask = 0xfffffffcu;
constexpr uint32_t kMesaOpaque = 0x80000000u;

class Reader {
  public:
    explicit Reader(ByteSpan bytes) : bytes_(bytes) {
        if (!bytes.data && bytes.size) throw std::invalid_argument("Mesa IR null storage");
    }
    void Region(uint64_t offset, uint64_t size) const {
        if (offset > bytes_.size || size > bytes_.size - offset) throw std::invalid_argument("Mesa IR truncated record");
    }
    uint32_t U32(uint64_t offset) const {
        Region(offset, 4);
        const auto* p = static_cast<const uint8_t*>(bytes_.data) + size_t(offset);
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    uint64_t U64(uint64_t offset) const { return uint64_t(U32(offset)) | (uint64_t(U32(offset + 4)) << 32); }
    float F32(uint64_t offset, bool emulated = false) const {
        uint32_t bits = U32(offset);
        if (emulated && (bits & 0x80000000u)) bits = 0x80000000u - bits;
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    Bounds Box(uint64_t offset, bool emulated = false) const {
        Bounds out;
        for (size_t axis = 0; axis < 3; ++axis) {
            out.min[axis] = F32(offset + axis * 4, emulated);
            out.max[axis] = F32(offset + 12 + axis * 4, emulated);
        }
        return out;
    }

  private:
    ByteSpan bytes_;
};

void ValidateBounds(const Bounds& bounds) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) || bounds.min[axis] > bounds.max[axis]) {
            throw std::invalid_argument("Mesa IR invalid active bounds");
        }
    }
}

bool Contains(const Bounds& parent, const Bounds& child) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (parent.min[axis] > child.min[axis] || parent.max[axis] < child.max[axis]) return false;
    }
    return true;
}

void RequireEmptyBounds(const Bounds& bounds) {
    for (size_t axis = 0; axis < 3; ++axis) {
        if (bounds.min[axis] != std::numeric_limits<float>::infinity() ||
            bounds.max[axis] != -std::numeric_limits<float>::infinity()) {
            throw std::invalid_argument("Mesa IR noncanonical empty bounds");
        }
    }
}

uint32_t GeometryFlags(uint32_t packed, const std::vector<uint32_t>& flags) {
    const uint32_t geometry = packed & ~kMesaOpaque;
    if (geometry >= flags.size()) throw std::invalid_argument("Mesa IR missing geometry flags");
    const uint32_t value = flags[geometry];
    if ((value & ~3u) || bool(value & 1u) != bool(packed & kMesaOpaque)) {
        throw std::invalid_argument("Mesa IR geometry flags mismatch or unsupported flags");
    }
    return value;
}

// The source mat3x4 has three packed rows, like VkTransformMatrixKHR. Use
// double intermediates for the affine inverse and reject unrepresentable
// results, rather than silently storing infinities into the runtime record.
void Inverse(const float* source, float* destination) {
    const double a = source[0], b = source[1], c = source[2];
    const double d = source[4], e = source[5], f = source[6];
    const double g = source[8], h = source[9], i = source[10];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (determinant == 0 || !std::isfinite(determinant)) throw std::invalid_argument("Mesa IR singular instance transform");
    const double inverse[9] = {(e * i - f * h) / determinant, (c * h - b * i) / determinant, (b * f - c * e) / determinant,
                               (f * g - d * i) / determinant, (a * i - c * g) / determinant, (c * d - a * f) / determinant,
                               (d * h - e * g) / determinant, (b * g - a * h) / determinant, (a * e - b * d) / determinant};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            const double value =
                column < 3 ? inverse[row * 3 + column]
                           : -(inverse[row * 3] * source[3] + inverse[row * 3 + 1] * source[7] + inverse[row * 3 + 2] * source[11]);
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                throw std::invalid_argument("Mesa IR unrepresentable inverse transform");
            }
            destination[row * 4 + column] = float(value);
        }
    }
}
}  // namespace

MesaAdapted AdaptMesaIr(const MesaIrInput& input, const std::vector<uint32_t>& geometry_flags,
                        const BlasAddressResolver& resolve_blas) {
    Reader header(input.header), nodes(input.nodes);
    header.Region(0, BVH_IR_HEADER_SIZE);
    if (input.build_flags != BVH_IR_SUPPORTED_INPUT_FLAGS) throw std::invalid_argument("Mesa IR unsupported BUILD variant");
    const uint32_t type = uint32_t(input.geometry_type);
    if (type > 2) throw std::invalid_argument("Mesa IR geometry type");
    const uint32_t stride = type == 0 ? BVH_IR_TRIANGLE_SIZE : type == 1 ? BVH_IR_AABB_SIZE : BVH_IR_INSTANCE_SIZE;
    const uint32_t leaf_kind = type == 0 ? kMesaTriangle : type == 1 ? kMesaAabb : kMesaInstance;
    const uint32_t active = header.U32(24), internal_count = header.U32(32);
    if (active > input.leaf_capacity || internal_count != BVH_IR_INTERNAL_COUNT(active) || header.U32(36) != 1 ||
        header.U32(40) != 1) {
        throw std::invalid_argument("Mesa IR counts or dispatch dimensions");
    }
    const uint64_t leaf_bytes = uint64_t(input.leaf_capacity) * stride;
    const uint64_t root_offset = leaf_bytes + uint64_t(internal_count - 1) * BVH_IR_BOX_SIZE;
    if (root_offset > kMesaOffsetMask || (active && uint64_t(active) * 2 - 1 > VEL_RT_BVH4_MAX_COUNT)) {
        throw std::overflow_error("Mesa IR construction node-ID capacity");
    }
    nodes.Region(0, root_offset + BVH_IR_BOX_SIZE);
    const Bounds header_bounds = header.Box(0, true), root_bounds = nodes.Box(root_offset);
    const uint32_t root_left = nodes.U32(root_offset + 24), root_right = nodes.U32(root_offset + 28);
    MesaAdapted out;
    if (!active) {
        if (root_left != BVH_IR_INVALID_ID || root_right != BVH_IR_INVALID_ID) {
            throw std::invalid_argument("Mesa IR nonempty root for empty tree");
        }
        RequireEmptyBounds(header_bounds);
        RequireEmptyBounds(root_bounds);
        return out;
    }
    ValidateBounds(header_bounds);
    ValidateBounds(root_bounds);
    if (!Contains(header_bounds, root_bounds)) throw std::invalid_argument("Mesa IR header bounds not conservative");

    uint32_t start_id = uint32_t(root_offset) | kMesaBox;
    size_t boxes_seen = 0;
    std::vector<uint8_t> seen_boxes(internal_count, 0);
    if (active == 1) {
        // Default Mesa BUILD emits one wrapper box even for a singleton. Its
        // second child is invalid; accepting arbitrary unary nodes would hide
        // count errors and violate the generic binary-tree input contract.
        if (root_left == BVH_IR_INVALID_ID || root_right != BVH_IR_INVALID_ID || (root_left & 3u) != leaf_kind) {
            throw std::invalid_argument("Mesa IR malformed singleton wrapper");
        }
        start_id = root_left;
        seen_boxes[0] = 1;
        boxes_seen = 1;
    }
    std::unordered_set<uint32_t> seen_leaves;
    struct Work {
        uint32_t id, parent;
        bool right;
        uint64_t parent_offset;
    };
    std::vector<Work> work{{start_id, VEL_RT_BVH4_INVALID_ID, false, root_offset + BVH_IR_BOX_SIZE}};
    while (!work.empty()) {
        const Work current = work.back();
        work.pop_back();
        const uint32_t kind = current.id & 3u;
        const uint64_t offset = current.id & kMesaOffsetMask;
        if (current.id == BVH_IR_INVALID_ID) throw std::invalid_argument("Mesa IR invalid child in nonempty tree");
        BuildNode node;
        if (kind == kMesaBox) {
            if (offset < leaf_bytes || (offset - leaf_bytes) % BVH_IR_BOX_SIZE || offset >= current.parent_offset ||
                offset > root_offset) {
                throw std::invalid_argument("Mesa IR box range or children-before-parents order");
            }
            const size_t box = size_t((offset - leaf_bytes) / BVH_IR_BOX_SIZE);
            if (seen_boxes[box]++) throw std::invalid_argument("Mesa IR box has multiple parents");
            ++boxes_seen;
        } else {
            if (kind != leaf_kind || offset >= leaf_bytes || offset % stride) {
                throw std::invalid_argument("Mesa IR leaf kind or record boundary");
            }
            if (!seen_leaves.insert(uint32_t(offset / stride)).second) throw std::invalid_argument("Mesa IR duplicate leaf");
        }
        node.bounds = nodes.Box(offset);
        ValidateBounds(node.bounds);
        const Bounds& parent_bounds = current.parent == VEL_RT_BVH4_INVALID_ID ? root_bounds : out.tree[current.parent].bounds;
        if (!Contains(parent_bounds, node.bounds)) throw std::invalid_argument("Mesa IR parent bounds not conservative");
        if (kind == kMesaTriangle) {
            VelRtBvh4Triangle leaf{};
            for (size_t vertex = 0; vertex < 3; ++vertex) {
                for (size_t axis = 0; axis < 3; ++axis) {
                    const float value = nodes.F32(offset + 24 + (vertex * 3 + axis) * 4);
                    if (!std::isfinite(value) || value < node.bounds.min[axis] || value > node.bounds.max[axis]) {
                        throw std::invalid_argument("Mesa IR triangle coordinates outside bounds");
                    }
                    leaf.vertices[vertex][axis] = value;
                }
            }
            leaf.primitive_id = nodes.U32(offset + 60);
            const uint32_t packed = nodes.U32(offset + 64);
            leaf.geometry_id = packed & ~kMesaOpaque;
            leaf.geometry_flags = GeometryFlags(packed, geometry_flags);
            node.leaf_id = MakeId(VEL_RT_BVH4_TRIANGLE, out.triangles.size());
            out.triangles.push_back(leaf);
            out.triangle_source_slots.push_back(uint32_t(offset / stride));
        } else if (kind == kMesaAabb) {
            VelRtBvh4Aabb leaf{};
            std::copy(node.bounds.min.begin(), node.bounds.min.end(), leaf.bounds_min);
            std::copy(node.bounds.max.begin(), node.bounds.max.end(), leaf.bounds_max);
            leaf.primitive_id = nodes.U32(offset + 24);
            const uint32_t packed = nodes.U32(offset + 28);
            leaf.geometry_id = packed & ~kMesaOpaque;
            leaf.geometry_flags = GeometryFlags(packed, geometry_flags);
            node.leaf_id = MakeId(VEL_RT_BVH4_AABB, out.aabbs.size());
            out.aabbs.push_back(leaf);
            out.aabb_source_slots.push_back(uint32_t(offset / stride));
        } else if (kind == kMesaInstance) {
            VelRtBvh4Instance leaf{};
            const uint64_t source_address = nodes.U64(offset + 24);
            leaf.custom_index_mask = nodes.U32(offset + 32);
            leaf.sbt_offset_flags = nodes.U32(offset + 36);
            if (!source_address || !(leaf.custom_index_mask >> 24) || !resolve_blas) {
                throw std::invalid_argument("Mesa IR inactive instance or missing BLAS resolver");
            }
            leaf.blas_address = resolve_blas(source_address);
            if (!leaf.blas_address) throw std::invalid_argument("Mesa IR unresolved BLAS address");
            for (size_t element = 0; element < 12; ++element) {
                leaf.object_to_world[element] = nodes.F32(offset + 40 + element * 4);
                if (!std::isfinite(leaf.object_to_world[element])) throw std::invalid_argument("Mesa IR nonfinite transform");
            }
            Inverse(leaf.object_to_world, leaf.world_to_object);
            leaf.instance_id = nodes.U32(offset + 88);
            // root_flags are Mesa culling acceleration metadata, not additional
            // Vulkan instance flags. Runtime flags are preserved in the SBT word.
            node.leaf_id = MakeId(VEL_RT_BVH4_INSTANCE, out.instances.size());
            out.instances.push_back(leaf);
            out.instance_source_slots.push_back(uint32_t(offset / stride));
        }
        const uint32_t output = uint32_t(out.tree.size());
        out.tree.push_back(node);
        if (current.parent == VEL_RT_BVH4_INVALID_ID) {
            out.root = output;
        } else if (current.right) {
            out.tree[current.parent].right = output;
        } else {
            out.tree[current.parent].left = output;
        }
        if (kind == kMesaBox) {
            work.push_back({nodes.U32(offset + 28), output, true, offset});
            work.push_back({nodes.U32(offset + 24), output, false, offset});
        }
    }
    if (boxes_seen != internal_count || seen_leaves.size() != active) {
        throw std::invalid_argument("Mesa IR disconnected records or active count mismatch");
    }
    return out;
}

MesaAdapted AdaptMesaContainerV2(ByteSpan input, const std::vector<uint32_t>& geometry_flags,
                                 const BlasAddressResolver& resolve_blas) {
    const Reader reader(input);
    reader.Region(0, BVH_IR_NODE_OFFSET);
    if (reader.U32(BVH_IR_MAGIC_OFFSET) != BVH_IR_MAGIC || reader.U32(BVH_IR_VERSION_OFFSET) != BVH_IR_VERSION) {
        throw std::invalid_argument("Mesa IR unsupported container identity");
    }
    const auto* data = static_cast<const uint8_t*>(input.data);
    MesaIrInput raw{{data, BVH_IR_HEADER_SIZE},
                    {data + BVH_IR_NODE_OFFSET, input.size - BVH_IR_NODE_OFFSET},
                    reader.U32(BVH_IR_LEAF_CAPACITY_OFFSET),
                    MesaGeometryType(reader.U32(BVH_IR_GEOMETRY_TYPE_OFFSET)),
                    reader.U32(BVH_IR_BUILD_FLAGS_OFFSET)};
    return AdaptMesaIr(raw, geometry_flags, resolve_blas);
}

}  // namespace vel::rt::bvh4
