// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "bvh4_reference.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vel::rt::bvh4;

static void Check(bool success, const char* message) {
    if (!success) throw std::runtime_error(message);
}

template <typename Exception, typename Function>
static void Reject(Function function) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("expected rejection");
}

static Bounds Union(Bounds a, const Bounds& b) {
    for (size_t axis = 0; axis < 3; axis++) {
        a.min[axis] = std::min(a.min[axis], b.min[axis]);
        a.max[axis] = std::max(a.max[axis], b.max[axis]);
    }
    return a;
}

static bool Contains(const Bounds& a, const Bounds& b) {
    for (size_t axis = 0; axis < 3; axis++) {
        if (a.min[axis] > b.min[axis] || a.max[axis] < b.max[axis]) return false;
    }
    return true;
}

static std::vector<BuildNode> MakeTree(uint32_t leaves, uint32_t seed, bool comb = false) {
    std::mt19937 random(seed);
    std::vector<BuildNode> nodes;
    std::vector<uint32_t> roots;
    uint32_t counters[3]{};
    for (uint32_t i = 0; i < leaves; i++) {
        uint32_t kind = (i % 3) + 1;
        float x = float(random() % 10000) / 100;
        float y = float(random() % 10000) / 100;
        float z = float(random() % 10000) / 100;
        Bounds bounds{{x, y, z}, {x + float(i % 7), y + float(i % 11), z + float(i % 5)}};
        roots.push_back(uint32_t(nodes.size()));
        nodes.push_back(
            {bounds, VEL_RT_BVH4_INVALID_ID, VEL_RT_BVH4_INVALID_ID, MakeId(VelRtBvh4Kind(kind), counters[kind - 1]++)});
    }
    while (roots.size() > 1) {
        size_t a = comb ? roots.size() - 1 : random() % roots.size();
        uint32_t left = roots[a];
        roots.erase(roots.begin() + std::ptrdiff_t(a));
        size_t b = comb ? roots.size() - 1 : random() % roots.size();
        uint32_t right = roots[b];
        roots.erase(roots.begin() + std::ptrdiff_t(b));
        roots.push_back(uint32_t(nodes.size()));
        nodes.push_back({Union(nodes[left].bounds, nodes[right].bounds), left, right});
    }
    return nodes;
}

// Derive output subtree bounds from leaf input data, independently of encoded
// internal bounds. This checks coverage, every parent and conservative boxes.
static void Verify(const std::vector<BuildNode>& tree, uint32_t root, const Encoded& encoded) {
    std::unordered_map<uint32_t, Bounds> leaf_bounds;
    for (const auto& node : tree) {
        if (node.leaf_id != VEL_RT_BVH4_INVALID_ID) leaf_bounds.emplace(node.leaf_id, node.bounds);
    }
    if (tree.empty()) {
        Check(encoded.root_id == VEL_RT_BVH4_INVALID_ID && encoded.nodes.empty() && encoded.leaf_parents.empty(), "empty output");
        return;
    }
    std::set<uint32_t> visited;
    std::unordered_map<uint32_t, Bounds> aggregates;
    struct Task {
        uint32_t id, parent;
        bool finish;
    };
    std::vector<Task> stack{{encoded.root_id, VEL_RT_BVH4_INVALID_ID, false}};
    size_t leaves_seen = 0, internal_seen = 0;
    while (!stack.empty()) {
        const auto task = stack.back();
        stack.pop_back();
        if (task.finish) {
            const auto& node = encoded.nodes.at(task.id & VEL_RT_BVH4_INDEX_MASK);
            Bounds aggregate = aggregates.at(node.child_id[0]);
            for (uint32_t i = 0; i < node.child_count; i++) {
                const auto& child_bounds = aggregates.at(node.child_id[i]);
                Bounds stored;
                std::copy_n(node.bounds_min[i], 3, stored.min.begin());
                std::copy_n(node.bounds_max[i], 3, stored.max.begin());
                Check(Contains(stored, child_bounds), "encoded child box lost leaf bounds");
                aggregate = Union(aggregate, child_bounds);
            }
            aggregates.emplace(task.id, aggregate);
            continue;
        }
        Check(visited.insert(task.id).second, "output cycle/duplicate leaf");
        const uint32_t kind = task.id >> VEL_RT_BVH4_KIND_SHIFT;
        const uint32_t index = task.id & VEL_RT_BVH4_INDEX_MASK;
        if (kind) {
            leaves_seen++;
            aggregates.emplace(task.id, leaf_bounds.at(task.id));
            uint64_t slot = index;
            if (kind > VEL_RT_BVH4_TRIANGLE) slot += encoded.counts.triangles;
            if (kind > VEL_RT_BVH4_AABB) slot += encoded.counts.aabbs;
            Check(encoded.leaf_parents.at(size_t(slot)) == task.parent, "leaf parent mismatch");
        } else {
            internal_seen++;
            const auto& node = encoded.nodes.at(index);
            Check(node.parent_id == task.parent, "internal parent mismatch");
            Check(node.child_count >= 2 && node.child_count <= 4, "invalid arity");
            for (uint32_t i = node.child_count; i < 4; i++) {
                Check(node.child_id[i] == VEL_RT_BVH4_INVALID_ID, "unused child not invalid");
            }
            stack.push_back({task.id, task.parent, true});
            for (uint32_t i = 0; i < node.child_count; i++) stack.push_back({node.child_id[i], task.id, false});
        }
    }
    Check(leaves_seen == leaf_bounds.size(), "leaf coverage mismatch");
    Check(internal_seen == encoded.nodes.size(), "unreachable output node");
    Check(Contains(tree.at(root).bounds, aggregates.at(encoded.root_id)), "root bounds mismatch");
    Check(PlanLayout(encoded.nodes.size(), encoded.counts).size >= sizeof(VelRtBvh4Header), "layout");
}

static void LayoutTests() {
    const uint64_t primitive_minimum = (UINT64_C(1) << 29) - 1;
    const auto layout = PlanLayout(primitive_minimum - 1, {primitive_minimum, 0, 0});
    Check(layout.size > (UINT64_C(1) << 32), "layout truncated to 32 bits");
    Check(layout.triangles == 128 + (primitive_minimum - 1) * 128, "internal region size");
    Check(layout.aabbs >= layout.triangles + primitive_minimum * 48, "triangle region size");
    Check(layout.size >= layout.leaf_parents + primitive_minimum * 4, "parent region size");
    Check(layout.nodes % 128 == 0 && layout.instances % 16 == 0 && layout.size % 256 == 0, "layout alignment");
    Check((MakeId(VEL_RT_BVH4_TRIANGLE, primitive_minimum - 1) & VEL_RT_BVH4_INDEX_MASK) == primitive_minimum - 1,
          "required primitive ID capacity");
    for (int k = 0; k < 4; k++) {
        Check(MakeId(VelRtBvh4Kind(k), VEL_RT_BVH4_MAX_INDEX) != VEL_RT_BVH4_INVALID_ID, "invalid ID collision");
    }
    Reject<std::overflow_error>([] { MakeId(VEL_RT_BVH4_INTERNAL, UINT64_MAX); });
    Reject<std::invalid_argument>([] { MakeId(4, 0); });
    Reject<std::overflow_error>([] { PlanLayout(UINT64_MAX, {}); });
    Reject<std::overflow_error>([] { PlanLayout(0, {UINT64_MAX, 0, 0}); });
    Reject<std::overflow_error>([] { PlanLayout(0, {0, uint64_t(VEL_RT_BVH4_MAX_COUNT) + 1, 0}); });
    const auto all_regions =
        PlanLayout(VEL_RT_BVH4_MAX_COUNT, {VEL_RT_BVH4_MAX_COUNT, VEL_RT_BVH4_MAX_COUNT, VEL_RT_BVH4_MAX_COUNT});
    Check(all_regions.size > layout.size && all_regions.size < UINT64_MAX, "maximum arithmetic");
}

static void MalformedTests() {
    const auto valid = MakeTree(3, 1);
    const uint32_t root = uint32_t(valid.size() - 1);
    auto reject = [&](const std::function<void(std::vector<BuildNode>&)>& mutate) {
        auto nodes = valid;
        mutate(nodes);
        Reject<std::invalid_argument>([&] { Encode(nodes, root); });
    };
    reject([&](auto& nodes) { nodes[root].left = uint32_t(nodes.size()); });
    reject([&](auto& nodes) { nodes[root].right = nodes[root].left; });
    reject([&](auto& nodes) { nodes[root].left = root; });
    reject([&](auto& nodes) { nodes[root].bounds.min[0] = std::numeric_limits<float>::quiet_NaN(); });
    reject([&](auto& nodes) { nodes[0].bounds.max[1] = std::numeric_limits<float>::infinity(); });
    reject([&](auto& nodes) { nodes[root].bounds.max[0] = -1; });
    reject([&](auto& nodes) { nodes[0].bounds.max[0] = nodes[root].bounds.max[0] + 1; });
    reject([&](auto& nodes) { nodes[1].leaf_id = nodes[0].leaf_id; });
    reject([&](auto& nodes) { nodes[0].leaf_id = MakeId(VEL_RT_BVH4_TRIANGLE, 7); });
    reject([&](auto& nodes) { nodes[0].left = 1; });
    reject([&](auto& nodes) { nodes.push_back(nodes[0]); });
    Reject<std::invalid_argument>([] { Encode({}, 0); });
    Reject<std::invalid_argument>([&] { Encode(valid, VEL_RT_BVH4_INVALID_ID); });
}

int main() {
    try {
        LayoutTests();
        MalformedTests();
        Verify({}, VEL_RT_BVH4_INVALID_ID, Encode({}, VEL_RT_BVH4_INVALID_ID));
        for (uint32_t n : {1u, 2u, 3u, 4u, 5u, 17u, 127u, 1025u}) {
            for (uint32_t seed = 0; seed < 8; seed++) {
                auto tree = MakeTree(n, seed);
                auto root = uint32_t(tree.size() - 1);
                Verify(tree, root, Encode(tree, root));
            }
        }
        // No recursive host algorithm: a deep binary input must not exhaust
        // the C++ call stack during validation, collapse, or output checks.
        auto deep = MakeTree(8192, 23, true);
        auto root = uint32_t(deep.size() - 1);
        Verify(deep, root, Encode(deep, root));
        std::cout << "portable BVH4: ABI, large layouts, 64 random trees, deep tree, malformed inputs passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
