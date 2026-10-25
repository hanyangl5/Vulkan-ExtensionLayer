// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "mesa_ir_adapter.h"
#include "mesa_ir_test_fixture.h"

#if defined(VEL_RT_MESA_CHECK_VULKAN_ABI)
#include "../../../vkrt_layer/layer/bvh/vk_bvh_defines.h"
static_assert(sizeof(vk_ir_header) == 108);
static_assert(sizeof(vk_ir_box_node) == 40);
static_assert(sizeof(vk_ir_triangle_node) == 68);
static_assert(sizeof(vk_ir_aabb_node) == 32);
static_assert(sizeof(vk_ir_instance_node) == 96);
static_assert(offsetof(vk_ir_header, active_leaf_count) == 24);
static_assert(offsetof(vk_ir_header, ir_internal_node_count) == 32);
static_assert(offsetof(vk_ir_header, dst_leaf_node_offset) == 80);
static_assert(offsetof(vk_ir_box_node, children) == 24);
static_assert(offsetof(vk_ir_triangle_node, coords) == 24);
static_assert(offsetof(vk_ir_triangle_node, triangle_id) == 60);
static_assert(offsetof(vk_ir_aabb_node, primitive_id) == 24);
static_assert(offsetof(vk_ir_instance_node, base_ptr) == 24);
static_assert(offsetof(vk_ir_instance_node, otw_matrix) == 40);
static_assert(offsetof(vk_ir_instance_node, instance_id) == 88);
#endif

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace vel::rt::bvh4;
using namespace vel::rt::bvh4::mesa_test;

static void Check(bool success, const char* message) {
    if (!success) throw std::runtime_error(message);
}
template <typename Exception = std::invalid_argument, typename Function>
static void Reject(Function function) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("expected Mesa adapter rejection");
}

static void Triangles() {
    auto fixture = SparseTriangles();
    const auto output = AdaptMesaContainerV2(fixture.Span(), SparseTriangleFlags());
    Check(output.tree.size() == 5 && output.root == 0, "sparse generic tree/root");
    Check(output.triangle_source_slots == std::vector<uint32_t>({2, 0, 4}), "source slot remap preserves sparse inputs");
    Check(output.triangles.size() == 3 && output.aabbs.empty() && output.instances.empty(), "triangle-only record output");
    const uint32_t primitives[] = {4, 27, 91}, geometries[] = {2, 1, 1}, flags[] = {2, 3, 3};
    const float first_x[] = {0, -3, 3};
    for (size_t i = 0; i < 3; ++i) {
        Check(output.triangles[i].primitive_id == primitives[i], "original primitive ID");
        Check(output.triangles[i].geometry_id == geometries[i], "original geometry ID");
        Check(output.triangles[i].geometry_flags == flags[i], "full geometry flags, including NO_DUPLICATE_ANY_HIT");
        Check(output.triangles[i].vertices[0][0] == first_x[i], "triangle positions");
        Check(output.triangles[i].vertices[1][0] == first_x[i] + 1, "triangle second vertex");
    }
    const auto encoded = Encode(output.tree, output.root);
    Check(encoded.nodes.size() == 1 && encoded.nodes[0].child_count == 3, "Mesa-to-BVH4 frontier collapse");
    Check(encoded.counts.triangles == 3 && encoded.leaf_parents == std::vector<uint32_t>({0, 0, 0}), "Mesa output parents");
    for (uint32_t child = 0; child < 3; ++child) {
        Check(encoded.nodes[0].child_id[child] == MakeId(VEL_RT_BVH4_TRIANGLE, child), "dense runtime leaf references");
    }
    // Mesa's native API supplies separate header and node bases; the envelope is
    // a convenience, not an implicit requirement of the new adapter.
    MesaIrInput raw{
        {fixture.bytes.data(), 108}, {fixture.bytes.data() + 128, fixture.bytes.size() - 128}, 5, MesaGeometryType::Triangles, 0};
    const auto separate = AdaptMesaIr(raw, SparseTriangleFlags());
    Check(separate.triangle_source_slots == output.triangle_source_slots, "separate Mesa header/node bases");
    // Extra worst-case allocation padding is not a disconnected node.
    fixture.bytes.resize(fixture.bytes.size() + 400, 0xee);
    Check(AdaptMesaContainerV2(fixture.Span(), SparseTriangleFlags()).tree.size() == 5, "unused allocation tail");
}

static void EmptyAndSingleton() {
    for (uint32_t type = 0; type < 3; ++type) {
        Fixture empty(8, 0, type);
        empty.Empty();
        unsigned resolver_calls = 0;
        const auto result = AdaptMesaContainerV2(empty.Span(), {}, [&](uint64_t address) {
            ++resolver_calls;
            return address;
        });
        Check(result.tree.empty() && result.root == VEL_RT_BVH4_INVALID_ID && resolver_calls == 0, "empty inactive slots");
    }
    Fixture singleton(4, 1, 0);
    singleton.Triangle(3, -2, 510, 0x80000000u);
    singleton.InternalBox(0, singleton.LeafId(3), 0xffffffffu, {{-2, 0, 0}, {-1, 1, 1}});
    const auto output = AdaptMesaContainerV2(singleton.Span(), {1});
    Check(output.tree.size() == 1 && output.tree[0].leaf_id == MakeId(VEL_RT_BVH4_TRIANGLE, 0), "collapse singleton wrapper");
    Check(output.triangle_source_slots == std::vector<uint32_t>({3}), "singleton sparse source slot");
    const auto encoded = Encode(output.tree, output.root);
    Check(encoded.nodes.empty() && encoded.root_id == MakeId(VEL_RT_BVH4_TRIANGLE, 0), "direct runtime root leaf");
}

static void AabbsAndDeepTrees() {
    constexpr uint32_t active = 4096;
    Fixture fixture(active * 2, active, 1);
    for (uint32_t leaf = 0; leaf < active; ++leaf) {
        fixture.Aabb(leaf * 2, {{float(leaf), 0, 0}, {float(leaf + 1), 1, 1}}, 100000 - leaf, leaf % 2);
    }
    fixture.InternalBox(0, fixture.LeafId(0), fixture.LeafId(2), {{0, 0, 0}, {2, 1, 1}});
    for (uint32_t box = 1; box < active - 1; ++box) {
        fixture.InternalBox(box, fixture.InternalId(box - 1), fixture.LeafId((box + 1) * 2), {{0, 0, 0}, {float(box + 2), 1, 1}});
    }
    const auto output = AdaptMesaContainerV2(fixture.Span(), {0, 2});
    Check(output.tree.size() == active * 2 - 1 && output.aabbs.size() == active, "iterative deep tree adaptation");
    for (uint32_t leaf = 0; leaf < active; ++leaf) {
        Check(output.aabb_source_slots[leaf] == leaf * 2 && output.aabbs[leaf].primitive_id == 100000 - leaf,
              "AABB original IDs and inactive gaps");
        Check(output.aabbs[leaf].geometry_flags == (leaf % 2) * 2, "AABB full geometry flags");
        Check(output.aabbs[leaf].bounds_min[0] == float(leaf) && output.aabbs[leaf].bounds_max[0] == float(leaf + 1),
              "AABB bounds records");
    }
    Check(Encode(output.tree, output.root).counts.aabbs == active, "deep adapter output accepted by BVH4 encoder");
}

static Fixture InstanceFixture() {
    Fixture fixture(4, 1, 2);
    // Rotation/shear/nonuniform scale plus translation; inverse has exact binary
    // fractions, making independent round-trip checks stable.
    const float transform[12] = {0, -2, 0, 4, 4, 1, 0, -8, 0, 0, 0.5f, 2};
    fixture.Instance(2, 0x123456780000ULL, 313, transform);
    fixture.InternalBox(0, fixture.LeafId(2), 0xffffffffu, {{-8, -8, -8}, {8, 8, 8}});
    return fixture;
}

static void Instances() {
    const auto fixture = InstanceFixture();
    unsigned calls = 0;
    const auto result = AdaptMesaContainerV2(fixture.Span(), {}, [&](uint64_t source) {
        Check(source == 0x123456780000ULL, "resolver receives exact original BLAS BDA");
        ++calls;
        return 0xfedcba980000ULL;
    });
    Check(calls == 1 && result.instances.size() == 1 && result.instance_source_slots[0] == 2, "sparse active instance");
    const auto& instance = result.instances[0];
    Check(instance.blas_address == 0xfedcba980000ULL && instance.instance_id == 313, "relocated 64-bit BDA and original ID");
    Check(instance.custom_index_mask == 0xab123456u && instance.sbt_offset_flags == 0x05876543u, "packed Vulkan instance words");
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            double sum = column == 3 ? instance.world_to_object[row * 4 + 3] : 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                sum += double(instance.world_to_object[row * 4 + axis]) * instance.object_to_world[axis * 4 + column];
            }
            Check(sum == double(row == column), "world-to-object times object-to-world is affine identity");
        }
    }
    Check(instance.world_to_object[3] == 1.5f && instance.world_to_object[7] == 2 && instance.world_to_object[11] == -4,
          "independent affine inverse translation");
    Check(Encode(result.tree, result.root).root_id == MakeId(VEL_RT_BVH4_INSTANCE, 0), "instance runtime root");
}

static void RejectContainersAndHeaders() {
    const auto good = SparseTriangles();
    for (size_t size : {size_t(0), size_t(107), size_t(127), good.bytes.size() - 1}) {
        Reject([&] { AdaptMesaContainerV2({good.bytes.data(), size}, SparseTriangleFlags()); });
    }
    Reject([&] { AdaptMesaContainerV2({nullptr, 128}, {}); });
    for (const auto mutation : std::vector<std::pair<size_t, uint32_t>>{
             {108, 0}, {112, 1}, {116, 2}, {120, 3}, {124, 1}, {24, 6}, {32, 0}, {32, 3}, {36, 0}, {40, 2}}) {
        auto fixture = good;
        U32(fixture.bytes, mutation.first, mutation.second);
        Reject([&] { AdaptMesaContainerV2(fixture.Span(), SparseTriangleFlags()); });
    }
    auto overflow = good;
    U32(overflow.bytes, 116, 0xffffffffu);
    Reject<std::overflow_error>([&] { AdaptMesaContainerV2(overflow.Span(), SparseTriangleFlags()); });
    auto header_bounds = good;
    Box(header_bounds.bytes, 0, {{0, 0, 0}, {1, 1, 1}}, true);
    Reject([&] { AdaptMesaContainerV2(header_bounds.Span(), SparseTriangleFlags()); });
    Fixture empty(1, 0, 0);
    empty.Empty();
    U32(empty.bytes, empty.Internal(0) + 24, 0);
    Reject([&] { AdaptMesaContainerV2(empty.Span(), {}); });
    empty.Empty();
    F32(empty.bytes, 0, 0);
    Reject([&] { AdaptMesaContainerV2(empty.Span(), {}); });
}

static void RejectGraphsAndRecords() {
    const auto good = SparseTriangles();
    const size_t root = good.Internal(1), first = good.Internal(0);
    for (const auto mutation : std::vector<std::pair<size_t, uint32_t>>{{root + 24, good.InternalId(1)},
                                                                        {root + 24, good.InternalId(0) + 4},
                                                                        {root + 28, good.InternalId(0)},
                                                                        {root + 28, 0xffffffffu},
                                                                        {first + 24, good.LeafId(4) + 68},
                                                                        {first + 24, good.LeafId(0) + 4},
                                                                        {first + 24, good.LeafId(0) | 3u},
                                                                        {first + 24, good.LeafId(0)},
                                                                        {first + 24, good.InternalId(1)},
                                                                        {root + 24, good.LeafId(2)}}) {
        auto fixture = good;
        U32(fixture.bytes, mutation.first, mutation.second);
        Reject([&] { AdaptMesaContainerV2(fixture.Span(), SparseTriangleFlags()); });
    }
    auto bounds = good;
    F32(bounds.bytes, root + 12, 2);
    Reject([&] { AdaptMesaContainerV2(bounds.Span(), SparseTriangleFlags()); });
    bounds = good;
    F32(bounds.bytes, good.Leaf(2), std::numeric_limits<float>::quiet_NaN());
    Reject([&] { AdaptMesaContainerV2(bounds.Span(), SparseTriangleFlags()); });
    bounds = good;
    F32(bounds.bytes, good.Leaf(2) + 24, 100);
    Reject([&] { AdaptMesaContainerV2(bounds.Span(), SparseTriangleFlags()); });
    Reject([&] { AdaptMesaContainerV2(good.Span(), {}); });
    Reject([&] { AdaptMesaContainerV2(good.Span(), {0, 2, 2}); });
    Reject([&] { AdaptMesaContainerV2(good.Span(), {0, 3, 8}); });
    Fixture singleton(1, 1, 0);
    singleton.Triangle(0, 0, 0, 0);
    singleton.InternalBox(0, 0, 0, {{0, 0, 0}, {1, 1, 1}});
    Reject([&] { AdaptMesaContainerV2(singleton.Span(), {0}); });
}

static void RejectInstances() {
    const auto good = InstanceFixture();
    const auto identity = [](uint64_t address) { return address; };
    Reject([&] { AdaptMesaContainerV2(good.Span(), {}); });
    Reject([&] { AdaptMesaContainerV2(good.Span(), {}, [](uint64_t) { return uint64_t(0); }); });
    auto mutation = good;
    U64(mutation.bytes, good.Leaf(2) + 24, 0);
    Reject([&] { AdaptMesaContainerV2(mutation.Span(), {}, identity); });
    mutation = good;
    U32(mutation.bytes, good.Leaf(2) + 32, 0x00123456u);
    Reject([&] { AdaptMesaContainerV2(mutation.Span(), {}, identity); });
    mutation = good;
    F32(mutation.bytes, good.Leaf(2) + 40, std::numeric_limits<float>::infinity());
    Reject([&] { AdaptMesaContainerV2(mutation.Span(), {}, identity); });
    mutation = good;
    // Zero the full 3x3 linear part, retaining the translation.
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) F32(mutation.bytes, good.Leaf(2) + 40 + (row * 4 + column) * 4, 0);
    }
    Reject([&] { AdaptMesaContainerV2(mutation.Span(), {}, identity); });
}

int main() {
    try {
        Triangles();
        EmptyAndSingleton();
        AabbsAndDeepTrees();
        Instances();
        RejectContainersAndHeaders();
        RejectGraphsAndRecords();
        RejectInstances();
        std::cout << "PASS: Mesa scalar IR adapter, sparse records/IDs/flags/transforms, deep trees and malformed inputs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
