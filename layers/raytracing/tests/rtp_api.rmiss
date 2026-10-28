#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT uint payload;
layout(push_constant) uniform Push { layout(offset=8) uint bias; } pc;
void main() { payload=9u+pc.bias; }
