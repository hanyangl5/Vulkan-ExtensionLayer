#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT uint payload;
layout(push_constant) uniform Push { layout(offset=12) uint bias; } pc;
void main() { payload=40u+pc.bias+uint(gl_PrimitiveID); }
