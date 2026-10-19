// Copyright 2026 Xiaomi. SPDX-License-Identifier: Apache-2.0
// Shared helpers for host tests; the implementation is included to inspect private cache keys.
#pragma once
#include "../../layers/graphics_pipeline_library/graphics_pipeline_library.cpp"

using namespace graphics_pipeline_library;

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static LibBuildCtx Context(const VkGraphicsPipelineCreateInfo& ci) {
    LibBuildCtx ctx;
    ctx.ci = &ci;
    ctx.raster_normalized.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ctx.raster_normalized.lineWidth = 1.0f;
    if (ci.pRasterizationState) ctx.raster_normalized = *ci.pRasterizationState;
    ctx.has_depth = RasterizationEnabled(ci) && ci.pDepthStencilState != nullptr;
    if (ctx.has_depth) ctx.depth_normalized = *ci.pDepthStencilState;
    for (uint32_t i = 0; i < ci.stageCount; ++i) {
        if (ci.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            if (RasterizationEnabled(ci)) ctx.fragment_stages.push_back(ci.pStages[i]);
        } else
            ctx.pre_raster_stages.push_back(ci.pStages[i]);
    }
    BuildDynamicStateBuckets(ci, 0, &ctx);
    return ctx;
}

static std::string Key(const VkGraphicsPipelineCreateInfo& ci, GplSubset subset) {
    DeviceData data;
    auto ctx = Context(ci);
    switch (subset) {
        case GplSubset::kVertexInput:
            return MakeVertexInputKey(ctx);
        case GplSubset::kPreRaster:
            return MakePreRasterKey(ctx, data);
        case GplSubset::kFragmentShader:
            return MakeFragmentShaderKey(ctx, data);
        case GplSubset::kFragmentOutput:
            return MakeFragmentOutputKey(ctx, data);
        default:
            std::exit(2);
    }
}
