// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "vkrt_build_layout.h"
#include "bvh4_reference.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void check(bool success, const char *message) {
    if (!success) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    using namespace vel::rt::bvh4;
    for (uint32_t type : {0u, 1u, 2u}) {
        for (uint32_t capacity : {0u, 1u, 2u, 19u, 8192u, (1u << 25), (1u << 29) - 1u}) {
            VkAccelerationStructureGeometryKHR geometry{};
            geometry.geometryType = static_cast<VkGeometryTypeKHR>(type);
            VkAccelerationStructureBuildGeometryInfoKHR info{};
            info.type = type == 2 ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geometry;
            auto result = vkrt_build_layout(&info, &capacity, nullptr);
            Counts counts;
            if (type == 0) counts.triangles = capacity;
            if (type == 1) counts.aabbs = capacity;
            if (type == 2) counts.instances = capacity;
            const auto reference = PlanLayout(capacity > 1 ? uint64_t(capacity) - 1 : 0, counts);
            check(result.valid && result.as_size == reference.size && result.nodes == reference.nodes &&
                      result.triangles == reference.triangles && result.aabbs == reference.aabbs &&
                      result.instances == reference.instances && result.parents == reference.leaf_parents,
                  "production size query disagrees with shared ABI planner");
            check(result.source >= result.ir.scratch_size && result.mapping >= result.source + result.ir.as_size &&
                      result.geometry_flags >=
                          result.mapping + (uint64_t(capacity) + (capacity > 1 ? uint64_t(capacity) - 1 : 1)) * 4 &&
                      result.status >= result.geometry_flags + 4 && result.scratch_size >= result.status + 4,
                  "transient construction/encoder scratch regions overlap");
            check((result.source | result.mapping | result.scratch_size | result.as_size) % 256 == 0,
                  "construction/scratch/AS alignment");
            if (capacity == (1u << 29) - 1u) check(result.as_size > UINT32_MAX, "large AS planning truncated at 4 GiB");
            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = capacity;
            auto recorded = vkrt_build_layout(&info, nullptr, &range);
            check(recorded.as_size == result.as_size && recorded.scratch_size == result.scratch_size,
                  "recorded build and max-count query disagree");
        }
    }
    VkDeviceSize cursor = std::numeric_limits<uint64_t>::max() - 1, offset;
    check(!vkrt_layout_region(&cursor, 1, 4, 16, &offset), "alignment overflow accepted");
    cursor = 0;
    check(!vkrt_layout_region(&cursor, UINT64_MAX, 128, 128, &offset), "region multiplication overflow accepted");
    std::puts("BVH4 production/query planner agrees with shared ABI, including >4 GiB sizes");
}
