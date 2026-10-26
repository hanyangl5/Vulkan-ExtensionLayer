// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT uint payload;
void main() { payload=7; }
