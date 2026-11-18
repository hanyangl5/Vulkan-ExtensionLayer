#version 460
#extension GL_EXT_ray_tracing : require
// Scene benchmark closest hit: returns the hit distance.
layout(location = 0) rayPayloadInEXT float payload;
void main() { payload = gl_HitTEXT; }
