// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "bvh4_reference.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace vel::rt::bvh4 {
static_assert(sizeof(VelRtBvh4Header) == 128);
static_assert(sizeof(VelRtBvh4Node) == 128);
static_assert(offsetof(VelRtBvh4Node, child_id) == 96);
static_assert(offsetof(VelRtBvh4Header, node_offset) == 48);
static_assert(sizeof(VelRtBvh4Triangle) == 48);
static_assert(sizeof(VelRtBvh4Aabb) == 48);
static_assert(sizeof(VelRtBvh4Instance) == 128);

static uint64_t Add(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a) throw std::overflow_error("BVH4 size addition");
    return a + b;
}

static uint64_t Multiply(uint64_t a, uint64_t b) {
    if (b && a > std::numeric_limits<uint64_t>::max() / b) throw std::overflow_error("BVH4 size multiplication");
    return a * b;
}

static uint64_t Align(uint64_t value, uint64_t alignment) { return Add(value, alignment - 1) & ~(alignment - 1); }

uint32_t MakeId(uint32_t kind, uint64_t index) {
    if (kind > VEL_RT_BVH4_INSTANCE) throw std::invalid_argument("BVH4 kind");
    if (index > VEL_RT_BVH4_MAX_INDEX) throw std::overflow_error("BVH4 index capacity");
    return (uint32_t(kind) << VEL_RT_BVH4_KIND_SHIFT) | uint32_t(index);
}

Layout PlanLayout(uint64_t node_count, Counts counts) {
    for (uint64_t count : {node_count, counts.triangles, counts.aabbs, counts.instances}) {
        if (count > VEL_RT_BVH4_MAX_COUNT) throw std::overflow_error("BVH4 count capacity");
    }
    Layout out;
    uint64_t cursor = sizeof(VelRtBvh4Header);
    auto region = [&](uint64_t count, uint64_t stride, uint64_t alignment) {
        cursor = Align(cursor, alignment);
        uint64_t offset = cursor;
        cursor = Add(cursor, Multiply(count, stride));
        return offset;
    };
    out.nodes = region(node_count, sizeof(VelRtBvh4Node), 128);
    out.triangles = region(counts.triangles, sizeof(VelRtBvh4Triangle), 16);
    out.aabbs = region(counts.aabbs, sizeof(VelRtBvh4Aabb), 16);
    out.instances = region(counts.instances, sizeof(VelRtBvh4Instance), 16);
    const uint64_t leaves = Add(Add(counts.triangles, counts.aabbs), counts.instances);
    out.leaf_parents = region(leaves, sizeof(uint32_t), 16);
    out.size = Align(cursor, 256);
    return out;
}

static void ValidateBounds(const Bounds& bounds) {
    for (size_t axis = 0; axis < 3; axis++) {
        if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) || bounds.min[axis] > bounds.max[axis]) {
            throw std::invalid_argument("BVH4 invalid bounds");
        }
    }
}

static bool Contains(const Bounds& parent, const Bounds& child) {
    for (size_t axis = 0; axis < 3; axis++) {
        if (parent.min[axis] > child.min[axis] || parent.max[axis] < child.max[axis]) return false;
    }
    return true;
}

static double Area(const Bounds& bounds) {
    const double x = double(bounds.max[0]) - bounds.min[0];
    const double y = double(bounds.max[1]) - bounds.min[1];
    const double z = double(bounds.max[2]) - bounds.min[2];
    return x * y + y * z + z * x;
}

Encoded Encode(const std::vector<BuildNode>& tree, uint32_t root) {
    Encoded out;
    if (tree.empty()) {
        if (root != VEL_RT_BVH4_INVALID_ID) throw std::invalid_argument("BVH4 nonempty root for empty tree");
        return out;
    }
    if (tree.size() > VEL_RT_BVH4_MAX_COUNT || root >= tree.size()) throw std::invalid_argument("BVH4 input root or size");

    std::array<std::unordered_set<uint32_t>, 3> leaf_indices;
    std::vector<uint8_t> visited(tree.size(), 0);
    std::vector<uint32_t> pending{root};
    size_t reached = 0;
    while (!pending.empty()) {
        uint32_t index = pending.back();
        pending.pop_back();
        if (index >= tree.size()) throw std::invalid_argument("BVH4 child out of range");
        if (visited[index]++) throw std::invalid_argument("BVH4 cycle or multiple parents");
        reached++;
        const auto& node = tree[index];
        ValidateBounds(node.bounds);
        if (node.leaf_id != VEL_RT_BVH4_INVALID_ID) {
            uint32_t kind = node.leaf_id >> VEL_RT_BVH4_KIND_SHIFT;
            uint32_t leaf = node.leaf_id & VEL_RT_BVH4_INDEX_MASK;
            if (!kind || leaf > VEL_RT_BVH4_MAX_INDEX || node.left != VEL_RT_BVH4_INVALID_ID ||
                node.right != VEL_RT_BVH4_INVALID_ID || !leaf_indices[kind - 1].insert(leaf).second) {
                throw std::invalid_argument("BVH4 malformed or duplicate leaf");
            }
        } else {
            if (node.left >= tree.size() || node.right >= tree.size()) throw std::invalid_argument("BVH4 child out of range");
            if (!Contains(node.bounds, tree[node.left].bounds) || !Contains(node.bounds, tree[node.right].bounds)) {
                throw std::invalid_argument("BVH4 parent bounds are not conservative");
            }
            pending.push_back(node.left);
            pending.push_back(node.right);
        }
    }
    if (reached != tree.size()) throw std::invalid_argument("BVH4 disconnected input");
    for (const auto& indices : leaf_indices) {
        for (uint32_t index : indices) {
            if (index >= indices.size()) throw std::invalid_argument("BVH4 non-dense leaf indices");
        }
    }
    out.counts = {leaf_indices[0].size(), leaf_indices[1].size(), leaf_indices[2].size()};
    out.leaf_parents.assign(leaf_indices[0].size() + leaf_indices[1].size() + leaf_indices[2].size(), VEL_RT_BVH4_INVALID_ID);
    auto parent_slot = [&](uint32_t id) {
        const uint32_t kind = id >> VEL_RT_BVH4_KIND_SHIFT;
        size_t slot = id & VEL_RT_BVH4_INDEX_MASK;
        if (kind > VEL_RT_BVH4_TRIANGLE) slot += leaf_indices[0].size();
        if (kind > VEL_RT_BVH4_AABB) slot += leaf_indices[1].size();
        return slot;
    };
    if (tree[root].leaf_id != VEL_RT_BVH4_INVALID_ID) {
        out.root_id = tree[root].leaf_id;
        return out;
    }

    out.root_id = MakeId(VEL_RT_BVH4_INTERNAL, 0);
    out.nodes.emplace_back();
    std::vector<std::pair<uint32_t, uint32_t>> work{{root, 0}};
    out.nodes[0].parent_id = VEL_RT_BVH4_INVALID_ID;
    for (size_t task = 0; task < work.size(); task++) {
        auto [input, output] = work[task];
        std::vector<uint32_t> frontier{tree[input].left, tree[input].right};
        while (frontier.size() < 4) {
            size_t selected = frontier.size();
            double largest_area = -1;
            for (size_t i = 0; i < frontier.size(); i++) {
                if (tree[frontier[i]].leaf_id == VEL_RT_BVH4_INVALID_ID && Area(tree[frontier[i]].bounds) > largest_area) {
                    largest_area = Area(tree[frontier[i]].bounds);
                    selected = i;
                }
            }
            if (selected == frontier.size()) break;
            uint32_t expanded = frontier[selected];
            frontier[selected] = tree[expanded].left;
            frontier.insert(frontier.begin() + selected + 1, tree[expanded].right);
        }
        VelRtBvh4Node node{};
        node.parent_id = out.nodes[output].parent_id;
        node.child_count = uint32_t(frontier.size());
        std::fill(std::begin(node.child_id), std::end(node.child_id), VEL_RT_BVH4_INVALID_ID);
        for (size_t i = 0; i < frontier.size(); i++) {
            const auto& child = tree[frontier[i]];
            std::copy(child.bounds.min.begin(), child.bounds.min.end(), node.bounds_min[i]);
            std::copy(child.bounds.max.begin(), child.bounds.max.end(), node.bounds_max[i]);
            if (child.leaf_id != VEL_RT_BVH4_INVALID_ID) {
                node.child_id[i] = child.leaf_id;
                out.leaf_parents[parent_slot(child.leaf_id)] = MakeId(VEL_RT_BVH4_INTERNAL, output);
            } else {
                uint32_t child_output = uint32_t(out.nodes.size());
                node.child_id[i] = MakeId(VEL_RT_BVH4_INTERNAL, child_output);
                out.nodes.emplace_back();
                out.nodes.back().parent_id = MakeId(VEL_RT_BVH4_INTERNAL, output);
                work.emplace_back(frontier[i], child_output);
            }
        }
        out.nodes[output] = node;
    }
    return out;
}

}  // namespace vel::rt::bvh4
