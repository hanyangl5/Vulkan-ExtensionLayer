// Copyright (c) 2015 Microsoft. All rights reserved.
// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: MIT
// CPU treelet oracle adapts the pinned TreeletReorder algorithm.
// Original license: upstream/d3d12/LICENSE.
#pragma once
#include <algorithm>
#include <functional>
#include <numeric>
#include <set>
#include <tuple>

// Included after the shared Vulkan harness definition in tests.cpp.
static void CheckBVHBuilder(Harness &t) {
    auto getSizes = t.Proc<PFN_vkGetAccelerationStructureBuildSizesKHR>("vkGetAccelerationStructureBuildSizesKHR");
    auto create = t.Proc<PFN_vkCreateAccelerationStructureKHR>("vkCreateAccelerationStructureKHR");
    auto destroy = t.Proc<PFN_vkDestroyAccelerationStructureKHR>("vkDestroyAccelerationStructureKHR");
    auto build = t.Proc<PFN_vkCmdBuildAccelerationStructuresKHR>("vkCmdBuildAccelerationStructuresKHR");
    auto copy = t.Proc<PFN_vkCmdCopyAccelerationStructureKHR>("vkCmdCopyAccelerationStructureKHR");
    struct Box {
        std::array<float, 3> lo, hi;
    };
    auto unite = [](Box a, const Box &b) {
        for (size_t k = 0; k < 3; ++k) {
            a.lo[k] = std::min(a.lo[k], b.lo[k]);
            a.hi[k] = std::max(a.hi[k], b.hi[k]);
        }
        return a;
    };
    auto equalBox = [](const Box &a, const Box &b) {
        for (size_t k = 0; k < 3; ++k)
            if (a.lo[k] != b.lo[k] || a.hi[k] != b.hi[k]) return false;
        return true;
    };
    auto bitCount = [](uint32_t n) {
        int result = 0;
        for (; n; n &= n - 1) ++result;
        return result;
    };
    auto firstBit = [](uint32_t n) {
        int result = 0;
        for (; !(n & 1u); n >>= 1) ++result;
        return result;
    };
    auto leadingZeros = [](uint32_t n) {
        int result = 0;
        for (uint32_t mask = 0x80000000u; !(n & mask); mask >>= 1) ++result;
        return result;
    };
    const float infinity = std::numeric_limits<float>::infinity();
    for (auto [count, duplicate, preference] : {std::tuple<uint32_t, bool, uint32_t>{0, false, 0},
                                                {1, false, 0},
                                                {3, false, 0},
                                                {7, false, 0},
                                                {14, false, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR},
                                                {28, false, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR},
                                                {31, true, 0},
                                                {61, false, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR},
                                                {4099, false, 0},
                                                {10243, false, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR},
                                                {4099, true, 0}}) {
        uint32_t counts[5] = {0, count / 3, 0, count - count / 3, 0};
        std::vector<float> vertices(std::max(count, 1u) * 9 + 3);
        std::vector<uint32_t> indices(std::max(count, 1u) * 3 + 1);
        for (uint32_t i = 0; i < indices.size(); ++i) indices[i] = i - 1;
        auto fill = [&](int version) {
            for (uint32_t i = 0; i < count; ++i) {
                float x = duplicate ? 0.0f : float((i * 73u) % 197u);
                float y = duplicate ? 0.0f : float((i * 97u) % 101u);
                float z = duplicate ? 0.0f : float((i * 37u) % 47u);
                x = version == 0 ? x : version == 1 ? 100.0f - x : x - 200.0f;
                y += float(version * 2);
                const float v[] = {x, y, z, x + 0.25f, y, z, x, y + 0.5f, z};
                std::copy(v, v + 9, vertices.begin() + 3 + i * 9);
                if (count == 31) std::fill(vertices.begin() + 3 + i * 9, vertices.begin() + 12 + i * 9, 0.0f);
            }
        };
        fill(0);
        auto vb = t.Make(vertices.size() * sizeof(float), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                         vertices.data());
        auto ib = t.Make(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                         indices.data());
        float transform[] = {0, 0, 0, 0, 1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
        if (duplicate) transform[7] = transform[11] = transform[15] = 0;
        auto tr = t.Make(sizeof(transform), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, transform);
        std::array<VkAccelerationStructureGeometryKHR, 5> geometries{};
        std::array<const VkAccelerationStructureGeometryKHR *, 5> pointers{};
        std::array<VkAccelerationStructureBuildRangeInfoKHR, 5> ranges{};
        uint32_t base = 0;
        for (uint32_t g = 0; g < 5; ++g) {
            auto &geo = geometries[g];
            geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            auto &tri = geo.geometry.triangles;
            tri = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tri.vertexStride = 12;
            tri.vertexData.deviceAddress = vb.address;
            tri.maxVertex = std::max(count, 1u) * 3;
            tri.indexType = g == 1 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR;
            tri.indexData.deviceAddress = g == 1 ? ib.address : 0;
            if (g == 1) tri.transformData.deviceAddress = tr.address;
            ranges[g] = {counts[g], g == 1 ? (base * 3 + 1) * 4 : base * 9 * 4, 1, g == 1 ? 16u : 0u};
            pointers[g] = &geo;
            base += counts[g];
        }
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                     VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR | preference;
        info.geometryCount = 5;
        info.ppGeometries = pointers.data();
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getSizes(t.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, counts, &sizes);
        Require(sizes.accelerationStructureSize > 0 && sizes.buildScratchSize > 0 && sizes.updateScratchSize > 0,
                "Build/update size query failed");
        std::array<Buffer, 4> storage;
        std::array<VkAccelerationStructureKHR, 4> as{};
        for (uint32_t a = 0; a < 4; ++a) {
            storage[a] = t.Make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = storage[a].buffer;
            ci.size = sizes.accelerationStructureSize;
            ci.type = info.type;
            Check(create(t.device, &ci, nullptr, &as[a]));
        }
        auto scratch = t.Make(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Require(scratch.address % 256 == 0, "Test scratch alignment");
        info.dstAccelerationStructure = as[0];
        info.scratchData.deviceAddress = scratch.address;
        const auto *range = ranges.data();
        auto submit = [&] {
            t.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            t.Submit();
            Check(vkResetCommandPool(t.device, t.pool, 0));
        };
        auto word = [&](uint32_t buffer, uint32_t offset) { return static_cast<const uint32_t *>(storage[buffer].mapped)[offset]; };
        auto getBox = [&](uint32_t buffer, uint32_t node) {
            Box box;
            auto address = static_cast<const float *>(storage[buffer].mapped) + 16 + node * 8;
            for (size_t k = 0; k < 3; ++k) {
                box.lo[k] = address[k];
                box.hi[k] = address[k + 4];
            }
            return box;
        };
        auto expectedBoxes = [&] {
            std::vector<Box> boxes(count);
            for (uint32_t i = 0; i < count; ++i) {
                Box b;
                bool transformed = i < counts[1] && !duplicate;
                for (uint32_t k = 0; k < 3; ++k) {
                    float shift = transformed ? float((k + 1) * 10) : 0.0f;
                    b.lo[k] = std::min({vertices[3 + i * 9 + k], vertices[3 + i * 9 + 3 + k], vertices[3 + i * 9 + 6 + k]}) + shift;
                    b.hi[k] = std::max({vertices[3 + i * 9 + k], vertices[3 + i * 9 + 3 + k], vertices[3 + i * 9 + 6 + k]}) + shift;
                }
                boxes[i] = b;
            }
            return boxes;
        };
        auto validate = [&](uint32_t buffer, const std::vector<Box> &boxes) {
            Require(word(buffer, 0) == 0 && word(buffer, 1) == count && word(buffer, 4) == 5 && word(buffer, 10) == 2,
                    "AS header incorrect");
            Require(word(buffer, 8) == sizes.accelerationStructureSize, "AS stored size differs from size query");
            uint32_t inverse = word(buffer, 6), parents = word(buffer, 7), data = word(buffer, 3), leafBase = count ? count - 1 : 0;
            for (uint32_t g = 0; g < 5; ++g)
                Require(word(buffer, word(buffer, 5) + g) == counts[g], "Per-geometry counts not retained");
            std::vector<bool> visited(std::max(2 * count, 1u), false), primitives(count, false);
            std::function<std::pair<Box, uint32_t>(uint32_t)> visit = [&](uint32_t node) -> std::pair<Box, uint32_t> {
                Require(node < (count ? count * 2 - 1 : 1) && !visited[node], "Hierarchy cycle or out-of-range node");
                visited[node] = true;
                uint32_t a = word(buffer, 16 + node * 8 + 3), b = word(buffer, 16 + node * 8 + 7);
                Box box;
                uint32_t leaves = 0;
                if (a & 0x80000000u) {
                    uint32_t slot = a & 0x7fffffffu;
                    Require(count != 0 && slot < count && node == leafBase + slot, "Leaf index incorrect");
                    uint32_t record = data + slot * 32;
                    uint32_t local = word(buffer, record + 12), geo = word(buffer, record + 13);
                    Require(geo == 1 || geo == 3, "Original geometry index lost");
                    uint32_t original = local + (geo == 3 ? counts[1] : 0);
                    Require(original < count && !primitives[original], "Original primitive ID lost/duplicated");
                    primitives[original] = true;
                    Require(word(buffer, inverse + original) == slot, "Inverse sorting cache incorrect");
                    Require(word(buffer, record + 14) == VK_GEOMETRY_OPAQUE_BIT_KHR && word(buffer, record + 15) == 0,
                            "Geometry metadata incorrect");
                    box = boxes[original];
                    leaves = 1;
                } else {
                    Require(a != b && word(buffer, parents + a) == node && word(buffer, parents + b) == node,
                            "Parent cache incorrect");
                    auto left = visit(a), right = visit(b);
                    box = unite(left.first, right.first);
                    leaves = left.second + right.second;
                }
                Require(equalBox(getBox(buffer, node), box), "Computed AABB differs from independent CPU geometry bounds");
                return {box, leaves};
            };
            if (count)
                Require(visit(0).second == count, "Root does not contain all primitives");
            else
                Require(equalBox(getBox(buffer, 0), Box{{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}}),
                        "Empty AS root not empty");
            Require(word(buffer, parents) == UINT32_MAX, "Root parent sentinel incorrect");
        };
        t.Begin();
        build(t.cb, 1, &info, &range);
        submit();
        auto boxes = expectedBoxes();
        validate(0, boxes);
        // std::stable_sort is independent of the GPU bitonic network. The
        // reference code intentionally interleaves Y/X/Z, matching the source.
        if (count) {
            Box scene = boxes[0];
            for (auto b : boxes) scene = unite(scene, b);
            std::vector<uint32_t> order(count), keys(count);
            std::iota(order.begin(), order.end(), 0);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t xyz[3];
                for (uint32_t k = 0; k < 3; ++k) {
                    float center = (boxes[i].lo[k] + boxes[i].hi[k]) * 0.5f;
                    float unit = (center - scene.lo[k]) / std::max(scene.hi[k] - scene.lo[k], 0.00001f);
                    xyz[k] = uint32_t(std::clamp(unit * 1024.0f, 0.0f, 1023.0f));
                }
                const uint32_t coord[] = {xyz[1], xyz[0], xyz[2]};
                uint32_t key = 0;
                for (uint32_t k = 0; k < 30; ++k) key |= ((coord[k % 3] >> (k / 3)) & 1u) << k;
                keys[i] = key;
            }
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
            for (uint32_t i = 0; i < count; ++i)
                Require(word(0, word(0, 6) + order[i]) == i, "GPU bitonic sort differs from CPU stable Morton ordering");
            // Independent CPU implementation: recursively split sorted ranges
            // (not the GPU's parallel range discovery), then postorder treelet
            // DP. Compare the entire unordered child graph, not only its bounds.
            if (count >= 7) {
                struct Node {
                    uint32_t left = 0, right = 0;
                    Box box{};
                };
                std::vector<Node> nodes(count * 2 - 1);
                auto area = [](const Box &box) {
                    float x = box.hi[0] - box.lo[0], y = box.hi[1] - box.lo[1], z = box.hi[2] - box.lo[2];
                    return 2.0f * (x * y + x * z + y * z);
                };
                auto prefix = [&](uint32_t a, uint32_t b) {
                    uint32_t x = keys[order[a]] ^ keys[order[b]];
                    return x ? leadingZeros(x) : 31 + leadingZeros(a ^ b);
                };
                std::function<void(uint32_t, uint32_t, uint32_t)> splitRange = [&](uint32_t node, uint32_t first, uint32_t last) {
                    uint32_t split = first, step = last - first;
                    do {
                        step = (step + 1) / 2;
                        uint32_t candidate = split + step;
                        if (candidate < last && prefix(first, candidate) > prefix(first, last)) split = candidate;
                    } while (step > 1);
                    auto &n = nodes[node];
                    n.left = split == first ? count - 1 + split : split;
                    n.right = split + 1 == last ? count + split : split + 1;
                    if (split != first) splitRange(n.left, first, split);
                    if (split + 1 != last) splitRange(n.right, split + 1, last);
                    n.box = unite(nodes[n.left].box, nodes[n.right].box);
                };
                for (uint32_t i = 0; i < count; ++i) nodes[count - 1 + i].box = boxes[order[i]];
                splitRange(0, 0, count - 1);
                auto optimize = [&](uint32_t root) {
                    std::vector<uint32_t> frontier{nodes[root].left, nodes[root].right}, internals{root};
                    while (frontier.size() < 7) {
                        float largest = -1;
                        size_t selected = 0;
                        for (size_t i = 0; i < frontier.size(); ++i)
                            if (frontier[i] < count - 1 && area(nodes[frontier[i]].box) > largest) {
                                largest = area(nodes[frontier[i]].box);
                                selected = i;
                            }
                        uint32_t node = frontier[selected];
                        internals.push_back(node);
                        frontier[selected] = nodes[node].left;
                        frontier.push_back(nodes[node].right);
                    }
                    std::array<float, 128> costs{};
                    std::array<uint32_t, 128> partitions{};
                    for (uint32_t mask = 1; mask < 128; ++mask) {
                        Box box{{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}};
                        for (uint32_t i = 0; i < 7; ++i)
                            if (mask & (1u << i)) box = unite(box, nodes[frontier[i]].box);
                        costs[mask] = area(box);
                    }
                    for (uint32_t i = 0; i < 7; ++i)
                        costs[1u << i] =
                            area(nodes[root].box) > 0 ? 1.2f * area(nodes[frontier[i]].box) / area(nodes[root].box) : 0;
                    for (uint32_t size = 2; size <= 7; ++size)
                        for (uint32_t mask = 1; mask < 128; ++mask)
                            if (bitCount(mask) == int(size)) {
                                uint32_t delta = (mask - 1) & mask, part = (-delta) & mask, best = 0;
                                float lowest = infinity;
                                do {
                                    float cost = costs[part] + costs[mask ^ part];
                                    if (!best || cost < lowest) {
                                        lowest = cost;
                                        best = part;
                                    }
                                    part = (part - delta) & mask;
                                } while (part);
                                costs[mask] = std::min(costs[mask] * float(size), 1.2f * costs[mask] + lowest);
                                partitions[mask] = best;
                            }
                    std::vector<std::pair<uint32_t, uint32_t>> stack{{127, root}};
                    uint32_t allocated = 1;
                    while (!stack.empty()) {
                        auto [mask, node] = stack.back();
                        stack.pop_back();
                        uint32_t lm = partitions[mask], rm = mask ^ lm;
                        uint32_t l = bitCount(lm) > 1 ? internals[allocated++] : frontier[firstBit(lm)];
                        if (bitCount(lm) > 1) stack.push_back({lm, l});
                        uint32_t r = bitCount(rm) > 1 ? internals[allocated++] : frontier[firstBit(rm)];
                        if (bitCount(rm) > 1) stack.push_back({rm, r});
                        nodes[node].left = l;
                        nodes[node].right = r;
                    }
                    for (auto it = internals.rbegin(); it != internals.rend(); ++it)
                        nodes[*it].box = unite(nodes[nodes[*it].left].box, nodes[nodes[*it].right].box);
                };
                uint32_t passes = preference == VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR   ? 0
                                  : preference == VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR ? 3
                                                                                                            : 1;
                for (uint32_t pass = 0, threshold = 7; pass < passes && threshold <= count; ++pass, threshold *= 2) {
                    std::function<uint32_t(uint32_t)> visit = [&](uint32_t node) {
                        if (node >= count - 1) return 1u;
                        uint32_t n = visit(nodes[node].left) + visit(nodes[node].right);
                        if (n >= threshold) optimize(node);
                        return n;
                    };
                    visit(0);
                }
                for (uint32_t node = 0; node < count - 1; ++node) {
                    uint32_t l = word(0, 16 + node * 8 + 3), r = word(0, 16 + node * 8 + 7);
                    if (std::min(l, r) != std::min(nodes[node].left, nodes[node].right) ||
                        std::max(l, r) != std::max(nodes[node].left, nodes[node].right))
                        throw std::runtime_error(
                            "GPU treelet topology differs from independent CPU partition DP: count=" + std::to_string(count) +
                            " node=" + std::to_string(node) + " GPU=(" + std::to_string(l) + "," + std::to_string(r) + ") CPU=(" +
                            std::to_string(nodes[node].left) + "," + std::to_string(nodes[node].right) + ")");
                }
            }
        }
        std::vector<uint32_t> topology;
        for (uint32_t node = 0; node + 1 < count; ++node) {
            uint32_t a = word(0, 16 + node * 8 + 3), b = word(0, 16 + node * 8 + 7);
            topology.push_back(std::min(a, b));
            topology.push_back(std::max(a, b));
        }
        std::vector<uint32_t> inverse(count);
        for (uint32_t i = 0; i < count; ++i) inverse[i] = word(0, word(0, 6) + i);
        for (int version = 1; version <= 2; ++version) {
            fill(version);
            std::memcpy(vb.mapped, vertices.data(), vertices.size() * sizeof(float));
            std::vector<uint8_t> sourceSnapshot(sizes.accelerationStructureSize);
            std::memcpy(sourceSnapshot.data(), storage[0].mapped, sourceSnapshot.size());
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            info.srcAccelerationStructure = as[0];
            info.dstAccelerationStructure = as[version - 1];
            t.Begin();
            build(t.cb, 1, &info, &range);
            submit();
            boxes = expectedBoxes();
            validate(version - 1, boxes);
            if (version == 2)
                Require(std::memcmp(sourceSnapshot.data(), storage[0].mapped, sourceSnapshot.size()) == 0,
                        "Out-of-place update modified its source");
            for (uint32_t node = 0; node + 1 < count; ++node) {
                uint32_t a = word(version - 1, 16 + node * 8 + 3), b = word(version - 1, 16 + node * 8 + 7);
                Require(std::min(a, b) == topology[node * 2] && std::max(a, b) == topology[node * 2 + 1],
                        "UPDATE changed hierarchy connectivity");
            }
            for (uint32_t i = 0; i < count; ++i)
                Require(word(version - 1, word(version - 1, 6) + i) == inverse[i], "UPDATE rebuilt the sort mapping");
        }
        t.Begin();
        for (uint32_t target = 2; target < 4; ++target) {
            VkCopyAccelerationStructureInfoKHR ci{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            ci.src = as[1];
            ci.dst = as[target];
            ci.mode = target == 2 ? VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR : VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            copy(t.cb, &ci);
        }
        submit();
        for (uint32_t target = 2; target < 4; ++target) {
            validate(target, boxes);
            Require(std::memcmp(storage[1].mapped, storage[target].mapped, sizes.accelerationStructureSize) == 0,
                    "AS clone differs from source");
        }
        for (auto handle : as) destroy(t.device, handle, nullptr);
        std::printf("BVH %u%s: multi-geometry, Morton/bitonic, topology, refit and copies verified\n", count,
                    duplicate ? " duplicate centers" : "");
    }
}

static void CheckTLASBuilder(Harness &t) {
    auto getSizes = t.Proc<PFN_vkGetAccelerationStructureBuildSizesKHR>("vkGetAccelerationStructureBuildSizesKHR");
    auto create = t.Proc<PFN_vkCreateAccelerationStructureKHR>("vkCreateAccelerationStructureKHR");
    auto destroy = t.Proc<PFN_vkDestroyAccelerationStructureKHR>("vkDestroyAccelerationStructureKHR");
    auto address = t.Proc<PFN_vkGetAccelerationStructureDeviceAddressKHR>("vkGetAccelerationStructureDeviceAddressKHR");
    auto build = t.Proc<PFN_vkCmdBuildAccelerationStructuresKHR>("vkCmdBuildAccelerationStructuresKHR");
    struct Allocation {
        Buffer buffer;
        VkAccelerationStructureKHR as;
        Buffer scratch;
    };
    auto allocate = [&](const VkAccelerationStructureBuildGeometryInfoKHR &info, uint32_t count) {
        VkAccelerationStructureBuildSizesInfoKHR size{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getSizes(t.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &size);
        Allocation a;
        a.buffer = t.Make(size.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        a.scratch = t.Make(size.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = a.buffer.buffer;
        ci.size = size.accelerationStructureSize;
        ci.type = info.type;
        Check(create(t.device, &ci, nullptr, &a.as));
        return a;
    };
    float data[] = {-99, -99, -1, -2, -3, 1, 2, 3, 0, 0, 2, 3, 4, 4, 5, 6, 0, 0};
    auto aabbs = t.Make(sizeof(data), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, data);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
    geometry.geometry.aabbs.data.deviceAddress = aabbs.address;
    geometry.geometry.aabbs.stride = 32;
    VkAccelerationStructureBuildGeometryInfoKHR bottom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bottom.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bottom.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bottom.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    bottom.geometryCount = 1;
    bottom.pGeometries = &geometry;
    auto blas = allocate(bottom, 2);
    bottom.dstAccelerationStructure = blas.as;
    bottom.scratchData.deviceAddress = blas.scratch.address;
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = blas.as;
    VkDeviceAddress blasAddress = address(t.device, &ai);
    constexpr uint32_t count = 2051;
    std::vector<VkAccelerationStructureInstanceKHR> instances(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto &v = instances[i];
        v.transform.matrix[0][0] = (i & 1u) ? -2.0f : 1.0f;
        v.transform.matrix[0][1] = 0.25f;
        v.transform.matrix[1][1] = 0.5f;
        v.transform.matrix[2][2] = 1.0f;
        v.transform.matrix[0][3] = float((i * 73u) % 127u);
        v.transform.matrix[1][3] = float((i * 53u) % 107u);
        v.instanceCustomIndex = 0xab0000 + i;
        v.mask = 0xff;
        v.instanceShaderBindingTableRecordOffset = i % 7;
        v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        v.accelerationStructureReference = i + 1 == count ? 0 : blasAddress;
    }
    auto inst =
        t.Make(instances.size() * 64, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, instances.data());
    std::vector<VkDeviceAddress> pointers(count + 2);
    for (uint32_t i = 0; i < count; ++i) pointers[i + 2] = inst.address + uint64_t(i) * 64;
    auto ptr = t.Make(pointers.size() * 8, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, pointers.data());
    VkAccelerationStructureGeometryKHR ig{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    ig.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    ig.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    ig.geometry.instances.arrayOfPointers = VK_TRUE;
    ig.geometry.instances.data.deviceAddress = ptr.address;
    VkAccelerationStructureBuildGeometryInfoKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    top.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    top.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    top.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    top.geometryCount = 1;
    top.pGeometries = &ig;
    auto tlas = allocate(top, count);
    top.dstAccelerationStructure = tlas.as;
    top.scratchData.deviceAddress = tlas.scratch.address;
    VkAccelerationStructureBuildRangeInfoKHR br{2, 8, 0, 0}, tr{count, 16, 0, 0};
    const auto *brp = &br, *trp = &tr;
    const float inf = std::numeric_limits<float>::infinity();
    for (uint32_t phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            for (uint32_t a = 0; a < 2; ++a)
                for (uint32_t k = 0; k < 6; ++k) data[2 + a * 8 + k] += 10.0f;
            std::memcpy(aabbs.mapped, data, sizeof(data));
            bottom.mode = top.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            bottom.srcAccelerationStructure = blas.as;
            top.srcAccelerationStructure = tlas.as;
            for (auto &i : instances) i.transform.matrix[2][3] += 5.0f;
        } else if (phase == 2) {
            for (auto &i : instances) i.accelerationStructureReference = 0;
            top.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            top.srcAccelerationStructure = VK_NULL_HANDLE;
        }
        std::memcpy(inst.mapped, instances.data(), instances.size() * 64);
        t.Begin();
        if (phase < 2) {
            build(t.cb, 1, &bottom, &brp);
            t.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        }
        build(t.cb, 1, &top, &trp);
        t.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        t.Submit();
        Check(vkResetCommandPool(t.device, t.pool, 0));
        const auto *words = static_cast<const uint32_t *>(tlas.buffer.mapped);
        const auto *floats = static_cast<const float *>(tlas.buffer.mapped);
        Require(words[1] == count && words[0] == 1, "TLAS header incorrect");
        std::set<uint32_t> originals;
        float rootLo[3] = {inf, inf, inf}, rootHi[3] = {-inf, -inf, -inf};
        for (uint32_t slot = 0; slot < count; ++slot) {
            uint32_t record = words[3] + slot * 32, original = words[record + 28];
            Require(original < count && originals.insert(original).second, "Sorted instance ID lost");
            Require(std::memcmp(words + record, &instances[original], 64) == 0, "Instance transform/mask/SBT metadata lost");
            Require(words[words[6] + original] == slot, "TLAS inverse sort map incorrect");
            float lo[3] = {inf, inf, inf}, hi[3] = {-inf, -inf, -inf};
            if (instances[original].accelerationStructureReference) {
                const float baseLo[] = {data[2], data[3], data[4]}, baseHi[] = {data[13], data[14], data[15]};
                for (uint32_t corner = 0; corner < 8; ++corner)
                    for (uint32_t row = 0; row < 3; ++row) {
                        auto *m = instances[original].transform.matrix[row];
                        float v = m[3];
                        for (uint32_t k = 0; k < 3; ++k) v += m[k] * ((corner & (1u << k)) ? baseHi[k] : baseLo[k]);
                        lo[row] = std::min(lo[row], v);
                        hi[row] = std::max(hi[row], v);
                    }
            }
            uint32_t node = 16 + (count - 1 + slot) * 8;
            for (uint32_t k = 0; k < 3; ++k) {
                Require(floats[node + k] == lo[k] && floats[node + 4 + k] == hi[k],
                        "Transformed instance bounds differ from CPU reference");
                rootLo[k] = std::min(rootLo[k], lo[k]);
                rootHi[k] = std::max(rootHi[k], hi[k]);
            }
        }
        for (uint32_t k = 0; k < 3; ++k)
            Require(floats[16 + k] == rootLo[k] && floats[20 + k] == rootHi[k], "TLAS root bounds incorrect");
        std::printf("TLAS phase %u: 2051 pointer-array instances, transforms, inactive instances and AABB BLAS verified\n", phase);
    }
    destroy(t.device, tlas.as, nullptr);
    destroy(t.device, blas.as, nullptr);
}

// Exercise the optimizer's empty-box handling independently of triangle input.
static void CheckTreeletAABBs(Harness &t) {
    auto getSizes = t.Proc<PFN_vkGetAccelerationStructureBuildSizesKHR>("vkGetAccelerationStructureBuildSizesKHR");
    auto create = t.Proc<PFN_vkCreateAccelerationStructureKHR>("vkCreateAccelerationStructureKHR");
    auto destroy = t.Proc<PFN_vkDestroyAccelerationStructureKHR>("vkDestroyAccelerationStructureKHR");
    auto build = t.Proc<PFN_vkCmdBuildAccelerationStructuresKHR>("vkCmdBuildAccelerationStructuresKHR");
    constexpr uint32_t count = 257;
    std::vector<VkAabbPositionsKHR> boxes(count);
    auto input = t.Make(boxes.size() * sizeof(boxes[0]), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
    geometry.geometry.aabbs.data.deviceAddress = input.address;
    geometry.geometry.aabbs.stride = sizeof(boxes[0]);
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR size{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(t.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &size);
    auto storage = t.Make(size.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    auto scratch = t.Make(size.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = storage.buffer;
    ci.size = size.accelerationStructureSize;
    ci.type = info.type;
    VkAccelerationStructureKHR as;
    Check(create(t.device, &ci, nullptr, &as));
    info.dstAccelerationStructure = as;
    info.scratchData.deviceAddress = scratch.address;
    VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const auto *rp = &range;
    float infinity = std::numeric_limits<float>::infinity();
    for (uint32_t phase = 0; phase < 2; ++phase) {
        for (uint32_t i = 0; i < count; ++i) {
            float x = float(i % 17), y = float(i / 17);
            boxes[i] = {x, y, 0, x + 0.5f, y + 0.25f, 1};
            if (phase == 1 || i % 3 == 0) boxes[i].minX = std::numeric_limits<float>::quiet_NaN();
        }
        std::memcpy(input.mapped, boxes.data(), boxes.size() * sizeof(boxes[0]));
        t.Begin();
        build(t.cb, 1, &info, &rp);
        t.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        t.Submit();
        Check(vkResetCommandPool(t.device, t.pool, 0));
        auto words = static_cast<const uint32_t *>(storage.mapped);
        auto floats = static_cast<const float *>(storage.mapped);
        std::vector<bool> seen(count * 2 - 1, false);
        using Bounds = std::array<float, 6>;
        std::function<Bounds(uint32_t)> visit = [&](uint32_t node) {
            Require(node < count * 2 - 1 && !seen[node], "Inactive AABB treelet produced a cycle");
            seen[node] = true;
            uint32_t l = words[16 + node * 8 + 3], r = words[16 + node * 8 + 7];
            Bounds result;
            if (l & 0x80000000u) {
                uint32_t record = words[3] + (l & 0x7fffffffu) * 32, original = words[record + 12];
                Require(original < count, "AABB ID lost during treelet reordering");
                auto box = boxes[original];
                result = std::isnan(box.minX) ? Bounds{infinity, infinity, infinity, -infinity, -infinity, -infinity}
                                              : Bounds{box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ};
            } else {
                Require(l < count * 2 - 1 && r < count * 2 - 1, "AABB treelet child out of bounds");
                Require(words[words[7] + l] == node && words[words[7] + r] == node, "AABB treelet parent cache incorrect");
                auto a = visit(l), b = visit(r);
                for (uint32_t k = 0; k < 3; ++k) {
                    result[k] = std::min(a[k], b[k]);
                    result[k + 3] = std::max(a[k + 3], b[k + 3]);
                }
            }
            for (uint32_t k = 0; k < 3; ++k)
                Require(floats[16 + node * 8 + k] == result[k] && floats[20 + node * 8 + k] == result[k + 3],
                        "AABB treelet bounds mismatch");
            return result;
        };
        visit(0);
        Require(std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }), "AABB treelet lost nodes");
    }
    destroy(t.device, as, nullptr);
    std::puts("Treelets: mixed/all-inactive procedural AABB bounds and graph verified");
}
