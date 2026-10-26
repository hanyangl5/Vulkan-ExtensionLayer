// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Explicit M0 context experiment; this is not a general shader instrumentation ABI.
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require
#include "../omm_opacity.glsl"
struct OmmRecord { uint state; uint associated; uint instanceFlags; uint geometryFlags; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer OmmContext {
    uint64_t scene;
    uint flags; uint mode; uint action; uint applicationAnyHit; uint reserved0; uint reserved1;
    OmmRecord records[3];
    uint hit; uint instanceId; uint anyHitCalls; uint intersectionCalls;
    uint anyHitFlags; uint intersectionFlags; uint closestHitFlags; uint nativeFlags;
    uint reportAccepted; uint payloadMarker; uint wrapperCalls; uint missCalls;
    float hitT; float attributeU; float attributeV; uint reserved2;
};
layout(constant_id=0) const uint64_t ContextAddress = uint64_t(0);
struct OmmPayload { uint hit; uint instanceId; uint marker; float t; vec2 attributes; };
uint logicalDecision(uint instanceId) {
    OmmContext context = OmmContext(ContextAddress);
    OmmRecord record = context.records[instanceId];
    return ommResolveOpacity(record.state, record.associated != 0u, context.flags,
                             record.instanceFlags, record.geometryFlags);
}
