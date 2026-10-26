#version 460
// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#extension GL_GOOGLE_include_directive : require
#include "native_pipeline.glsl"
layout(location=0) rayPayloadInEXT OmmPayload payload;
hitAttributeEXT vec2 attributes;
void main() {
    OmmContext context = OmmContext(ContextAddress);
    context.closestHitFlags = context.flags;
    context.nativeFlags = gl_IncomingRayFlagsEXT;
    payload.hit = 1u; payload.instanceId = gl_InstanceID; payload.t = gl_HitTEXT;
    payload.attributes = attributes;
}
