// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "rtp_launch_plan.h"
#include <stdexcept>
#include <iostream>

void Require(bool value) {
    if (!value) throw std::runtime_error("RTP launch planner oracle failed");
}
int main() {
    using namespace vel::rt;
    RtpLaunchPlan p;
    auto plan = [&](uint32_t w, uint32_t h, uint32_t d, uint64_t budget, uint32_t dispatch = 65535, uint64_t stages = 7,
                    uint64_t pushes = 128) { return PlanRtpLaunch(w, h, d, budget, dispatch, stages, pushes, 208, p); };
    Require(plan(0, UINT32_MAX, UINT32_MAX, 0) && !p.invocations && !p.context_bytes);
    Require(plan(17, 3, 2, 131977));
    Require(p.invocations == 102 && p.arena_stride == 131984 && p.tile_invocations == 102);
    Require(p.push_table == 208 && p.push_payload == 264 && p.context_bytes == 1160);
    Require(plan(100, 4, 3, 64, 37));
    uint64_t rays = 0;
    while (const auto n = RtpLaunchTileCount(p, rays)) {
        Require(n <= 37);
        rays += n;
    }
    Require(rays == 1200 && RtpLaunchTileCount(p, 1184) == 16 && !RtpLaunchTileCount(p, 1200));
    Require(plan(1u << 30, 1, 1, kRtpArenaTargetBytes + 1));
    const auto metadata = p.context_bytes;
    Require(p.tile_invocations == 1 && p.arena_stride == kRtpArenaTargetBytes + 16);
    Require(p.arena_bytes == p.arena_stride && !ReduceRtpLaunchTile(p));
    Require(plan(1u << 30, 1, 1, (uint64_t(1) << 32) + 17));
    Require(p.tile_invocations == 1 && p.arena_stride == (uint64_t(1) << 32) + 32 && p.context_bytes == metadata);
    Require(plan(UINT32_MAX, UINT32_MAX, 1, 16));
    Require(p.invocations == uint64_t(UINT32_MAX) * UINT32_MAX && p.context_bytes == metadata);
    Require(!plan(UINT32_MAX, UINT32_MAX, 2, 16));
    Require(!plan(1, 1, 1, UINT64_MAX));
    Require(!plan(1, 1, 1, 0));
    Require(!plan(1, 1, 1, 16, 0));
    Require(!plan(1, 1, 1, 16, 1, UINT64_MAX));
    Require(!plan(1, 1, 1, 16, 1, 1, UINT64_MAX));
    Require(plan(1000, 1, 1, 16, 99));
    const auto stride = p.arena_stride;
    while (ReduceRtpLaunchTile(p)) Require(p.arena_stride == stride && p.arena_bytes == stride * p.tile_invocations);
    Require(p.tile_invocations == 1);
    std::cout << "Checked RTP launch tiling/64-bit budgets/constant metadata passed\n";
}
