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
void main() {
    SourceData a = payload.groups[0].refs[0];
    payload.groups[0].refs[0] = payload.groups[0].refs[1];
    payload.groups[0].refs[1] = a;
    payload.groups[0].tag = 3u;
    payload.chosen = payload.groups[0].refs[0];
    payload.total += 200u;
    callData = payload;
    executeCallableEXT(0u, 0);
    payload = callData;
    payload.groups[0].refs[1].v[payload.index + 6u] += 23u;
}
