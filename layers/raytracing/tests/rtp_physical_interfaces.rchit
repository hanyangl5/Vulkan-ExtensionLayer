// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require
#include "rtp_physical_interfaces_common.glsl"
layout(location = 0) rayPayloadInEXT Transfer payload;
layout(location = 0) callableDataEXT Transfer callData;
hitAttributeEXT PointerPack attributes;
void main() {
    payload.groups[0] = attributes;
    payload.groups[1].tag = gl_HitKindEXT;
    payload.chosen = attributes.refs[0];
    payload.total += 100u + attributes.tag;
    attributes.refs[1].v[payload.index + 2u] += 7u + attributes.tag;
    callData = payload;
    executeCallableEXT(0u, 0);
    payload = callData;
    payload.groups[0].refs[1].v[payload.index + 6u] += 23u;
}
