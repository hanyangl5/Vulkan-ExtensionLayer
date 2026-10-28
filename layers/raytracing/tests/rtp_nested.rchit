#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "rtp_nested_common.glsl"
hitAttributeEXT vec2 hitAttribute;
void main() { nested_visit(17u); }
