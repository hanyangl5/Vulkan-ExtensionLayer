// SPDX-License-Identifier: MIT
#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(location=0) out vec4 colour;
layout(push_constant,std430) uniform Push { uvec2 address; uint query; uint bias; } pc;
void main() {
    uint value=pc.bias;
    if(pc.query!=0) {
        rayQueryEXT ray;
        rayQueryInitializeEXT(ray,accelerationStructureEXT(uint64_t(pc.address.x)|(uint64_t(pc.address.y)<<32)),
            gl_RayFlagsOpaqueEXT,255,vec3(0,0,-1),0,vec3(0,0,1),10);
        while(rayQueryProceedEXT(ray)) {}
        value+=rayQueryGetIntersectionTypeEXT(ray,true);
    }
    colour=vec4(float(value));
}
