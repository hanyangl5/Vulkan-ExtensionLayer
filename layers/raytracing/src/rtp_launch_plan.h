// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace vel::rt {
constexpr uint64_t kRtpArenaTargetBytes = uint64_t(64) << 20;
struct RtpLaunchPlan {
    uint64_t invocations = 0, arena_stride = 0, arena_bytes = 0;
    uint64_t context_bytes = 0, push_table = 0, push_payload = 0, push_stride = 0;
    uint32_t tile_invocations = 0;
};
inline bool RtpCheckedAdd(uint64_t a, uint64_t b, uint64_t &out) {
    if (b > UINT64_MAX - a) return false;
    out = a + b;
    return true;
}
inline bool RtpCheckedMul(uint64_t a, uint64_t b, uint64_t &out) {
    if (a && b > UINT64_MAX / a) return false;
    out = a * b;
    return true;
}
// The target bounds preferred concurrent allocation, not shader stack size.
// A larger real budget still receives one complete arena, subject to native
// allocation success. Immutable metadata is independent of the tile count.
inline bool PlanRtpLaunch(uint32_t width, uint32_t height, uint32_t depth, uint64_t budget, uint32_t native_dispatch,
                          uint64_t stages, uint64_t push_bytes, uint64_t context_size, RtpLaunchPlan &out) {
    out = {};
    if (!width || !height || !depth) return true;
    if (!budget || !native_dispatch || !RtpCheckedMul(width, height, out.invocations) ||
        !RtpCheckedMul(out.invocations, depth, out.invocations) || !RtpCheckedAdd(budget, 15, out.arena_stride))
        return false;
    out.arena_stride &= ~uint64_t(15);
    const uint64_t preferred = std::max(uint64_t(1), kRtpArenaTargetBytes / out.arena_stride);
    out.tile_invocations = uint32_t(std::min({out.invocations, uint64_t(native_dispatch), preferred}));
    if (!RtpCheckedMul(out.arena_stride, out.tile_invocations, out.arena_bytes) || !RtpCheckedAdd(push_bytes, 7, out.push_stride))
        return false;
    out.push_stride &= ~uint64_t(7);
    out.push_table = context_size;
    uint64_t table_bytes, payload_bytes;
    if (!RtpCheckedMul(stages, 8, table_bytes) || !RtpCheckedMul(stages, out.push_stride, payload_bytes) ||
        !RtpCheckedAdd(out.push_table, table_bytes, out.push_payload) ||
        !RtpCheckedAdd(out.push_payload, payload_bytes, out.context_bytes) || out.context_bytes > SIZE_MAX)
        return false;
    return true;
}
inline bool ReduceRtpLaunchTile(RtpLaunchPlan &plan) {
    if (plan.tile_invocations <= 1) return false;
    plan.tile_invocations = std::max(1u, plan.tile_invocations / 2);
    // Both operands only decrease from an already checked product.
    plan.arena_bytes = plan.arena_stride * plan.tile_invocations;
    return true;
}
inline uint32_t RtpLaunchTileCount(const RtpLaunchPlan &plan, uint64_t base) {
    return base < plan.invocations ? uint32_t(std::min(uint64_t(plan.tile_invocations), plan.invocations - base)) : 0;
}
}  // namespace vel::rt
