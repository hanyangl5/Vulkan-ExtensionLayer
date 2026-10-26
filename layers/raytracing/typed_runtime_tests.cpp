// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "typed_runtime.h"
#include "glsl_compiler.h"
#include "bvh4/bvh4_abi.h"
#include <spirv-tools/libspirv.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
#include "frame_test_gpu.h"
std::string Prefix() {
    return R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
struct P { uint total; vec3 vector; uint16_t small; bool flag; };
layout(set=0,binding=0,std430) buffer Output { uint words[]; } outputData;
layout(push_constant,std430) uniform Push { uint64_t token; uint delta; } pc;
layout(shaderRecordEXT,std430) buffer Record { uint delta; } recordData;
)glsl";
}
std::string Raygen() {
    return Prefix() + R"glsl(
layout(location=7) rayPayloadEXT P p;
layout(location=3) callableDataEXT P c;
void helperTrace(vec3 origin,uint flags){
    flags|=(recordData.delta&16u)!=0u?4u:0u;
    traceRayEXT(accelerationStructureEXT(pc.token),flags,255,recordData.delta,0,0,origin,0.0,vec3(0,0,1),10.0,7);
}
void main(){
    uint id=gl_LaunchIDEXT.x, row=id*16;
    p.total=id;p.vector=vec3(1,2,3);p.small=uint16_t(id);p.flag=false;
    helperTrace((id%4==3)?vec3(4,0,0):vec3(0),id%4==0?1u:2u);
    uint snapshot=p.total;c=p;
    for(uint i=0;i<1+(id&1);++i)executeCallableEXT(0,3);
    outputData.words[row]=c.total;outputData.words[row+1]=uint(c.small);outputData.words[row+2]=uint(c.flag);
    outputData.words[row+3]=floatBitsToUint(c.vector.x);outputData.words[row+4]=floatBitsToUint(c.vector.y);
    outputData.words[row+5]=floatBitsToUint(c.vector.z);outputData.words[row+6]=snapshot;
    outputData.words[row+7]=id;outputData.words[row+8]=gl_LaunchSizeEXT.x;
}
)glsl";
}
std::string Miss() {
    return Prefix() + R"glsl(
layout(location=9) rayPayloadInEXT P p;
void main(){p.total+=7+recordData.delta+pc.delta;p.vector.z+=1.0;}
)glsl";
}
std::string Closest(bool unused_attribute) {
    return Prefix() + "\nlayout(location=9) rayPayloadInEXT P p;\n" +
           // A larger unused declaration must not prevent a triangle shader
           // from executing. The ordinary case still reads valid vec2 values.
           (unused_attribute ? "hitAttributeEXT vec3 attr;\n" : "hitAttributeEXT vec2 attr;\n") + R"glsl(
void nested(){traceRayEXT(accelerationStructureEXT(pc.token),0,255,0,0,0,vec3(4,0,0),0.0,vec3(0,0,1),10.0,9);}
void main(){
    p.total+=100+recordData.delta+pc.delta+uint(gl_PrimitiveID)+uint(gl_InstanceCustomIndexEXT)+uint(gl_GeometryIndexEXT);
)glsl" + (unused_attribute ? "p.vector+=vec3(0,0,gl_RayTmaxEXT);" : "p.vector+=vec3(attr,gl_RayTmaxEXT);") +
           R"glsl(
    nested();
    outputData.words[gl_LaunchIDEXT.x*16+9]=gl_HitKindEXT;
    outputData.words[gl_LaunchIDEXT.x*16+10]=uint(gl_InstanceID);
    outputData.words[gl_LaunchIDEXT.x*16+11]=floatBitsToUint(gl_ObjectRayDirectionEXT.z);
}
)glsl";
}
std::string Any(bool procedural) {
    return Prefix() + (procedural ? R"glsl(
layout(location=12) rayPayloadInEXT P p;
hitAttributeEXT vec2 attr;
void main(){
    if(attr.x<0.2){p.total+=1;ignoreIntersectionEXT;}
    p.total+=2;
    if(pc.delta==51u&&gl_LaunchIDEXT.x%4==2u)terminateRayEXT;
}
)glsl"
                                  : R"glsl(
layout(location=12) rayPayloadInEXT P p;
hitAttributeEXT vec2 attr;
void main(){
    uint which=gl_LaunchIDEXT.x%4;
    if(which==1){p.total+=10+pc.delta;ignoreIntersectionEXT;}
    if(which==2){p.total+=20+pc.delta;terminateRayEXT;}
    p.total+=30+pc.delta;
}
)glsl");
}
std::string Intersection() {
    return Prefix() + R"glsl(
hitAttributeEXT vec2 attr;
void main(){
    attr=vec2(0.125,0.25);bool first=reportIntersectionEXT(2.0,33);
    float firstMax=gl_RayTmaxEXT;
    attr=vec2(0.5,0.75);bool second=reportIntersectionEXT(1.0,34);
    attr=vec2(9,9);
    outputData.words[gl_LaunchIDEXT.x*16+12]=uint(first);
    outputData.words[gl_LaunchIDEXT.x*16+13]=uint(second);
    outputData.words[gl_LaunchIDEXT.x*16+14]=floatBitsToUint(firstMax);
    outputData.words[gl_LaunchIDEXT.x*16+15]=floatBitsToUint(gl_RayTmaxEXT);
}
)glsl";
}
std::string Callable() {
    return Prefix() + R"glsl(
layout(location=19) callableDataInEXT P c;
void main(){
    c.total+=5+recordData.delta+pc.delta;c.vector+=vec3(0.5,1,2);
    c.small=uint16_t(c.small+uint16_t(3));c.flag=!c.flag;
}
)glsl";
}
std::vector<VkRayTracingShaderGroupCreateInfoKHR> Groups() {
    auto Group = [](VkRayTracingShaderGroupTypeKHR type, uint32_t general, uint32_t closest = VK_SHADER_UNUSED_KHR,
                    uint32_t any = VK_SHADER_UNUSED_KHR, uint32_t intersection = VK_SHADER_UNUSED_KHR) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = type;
        g.generalShader = general;
        g.closestHitShader = closest;
        g.anyHitShader = any;
        g.intersectionShader = intersection;
        return g;
    };
    return {Group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0), Group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1),
            Group(VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 2, 3),
            Group(VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 4, 5, 6),
            Group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 7)};
}
template <class T>
void Write(GPU &gpu, uint64_t offset, const T &v) {
    std::memcpy(reinterpret_cast<uint8_t *>(gpu.mapped) + offset, &v, sizeof(v));
}
void WriteBytes(GPU &gpu, uint64_t offset, const void *v, size_t n) {
    std::memcpy(reinterpret_cast<uint8_t *>(gpu.mapped) + offset, v, n);
}
uint32_t Bits(float x) {
    uint32_t v;
    std::memcpy(&v, &x, 4);
    return v;
}
void AS(GPU &gpu, uint64_t blas, uint64_t tlas, bool procedural) {
    VelRtBvh4Header h{};
    h.magic = VEL_RT_BVH4_MAGIC;
    h.version = VEL_RT_BVH4_ABI_VERSION;
    h.type = 1;
    h.root_id = (procedural ? VEL_RT_BVH4_AABB : VEL_RT_BVH4_TRIANGLE) << 30;
    h.triangle_count = procedural ? 0 : 1;
    h.aabb_count = procedural ? 1 : 0;
    h.node_offset = h.triangle_offset = h.aabb_offset = h.instance_offset = 128;
    h.leaf_parent_offset = 176;
    h.total_size = 192;
    h.bounds_min[0] = h.bounds_min[1] = -1;
    h.bounds_min[2] = 1;
    h.bounds_max[0] = h.bounds_max[1] = 1;
    h.bounds_max[2] = 3;
    Write(gpu, blas, h);
    Write(gpu, blas + 176, UINT32_MAX);
    if (procedural) {
        VelRtBvh4Aabb a{};
        std::copy(h.bounds_min, h.bounds_min + 3, a.bounds_min);
        std::copy(h.bounds_max, h.bounds_max + 3, a.bounds_max);
        a.primitive_id = 3;
        Write(gpu, blas + 128, a);
    } else {
        VelRtBvh4Triangle tri{{{-1, -1, 1}, {1, -1, 1}, {0, 1, 1}}, 3, 0, 0};
        Write(gpu, blas + 128, tri);
    }
    h.type = 0;
    h.root_id = VEL_RT_BVH4_INSTANCE << 30;
    h.triangle_count = h.aabb_count = 0;
    h.instance_count = 1;
    h.leaf_parent_offset = 256;
    h.total_size = 272;
    Write(gpu, tlas, h);
    Write(gpu, tlas + 256, UINT32_MAX);
    VelRtBvh4Instance instance{};
    instance.blas_address = gpu.address + blas;
    instance.custom_index_mask = 0xff000005;
    instance.instance_id = 4;
    instance.object_to_world[0] = instance.object_to_world[5] = instance.object_to_world[10] = 1;
    instance.world_to_object[0] = instance.world_to_object[5] = instance.world_to_object[10] = 1;
    Write(gpu, tlas + 128, instance);
}
bool Execute(bool private_frames, bool compile_only, const std::string &dump_path, bool unused_attribute = false) {
    std::string sources[] = {Raygen(),       Miss(),    Closest(unused_attribute), Any(false), Closest(false), Any(true),
                             Intersection(), Callable()};
    const VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR,       VK_SHADER_STAGE_MISS_BIT_KHR,
                                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,  VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,  VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                           VK_SHADER_STAGE_INTERSECTION_BIT_KHR, VK_SHADER_STAGE_CALLABLE_BIT_KHR};
    std::vector<std::vector<uint32_t>> native;
    std::vector<TypedContinuationProgram> stages;
    for (uint32_t i = 0; i < 8; ++i) {
        native.push_back(CompileGLSL(sources[i], kinds[i]));
        stages.push_back(CompileTypedPipelineStage(native.back(), "main", kinds[i], nullptr, "vel_rt_stage_" + std::to_string(i)));
    }
    auto groups = Groups();
    auto plan = PlanTypedRuntimeArena(stages, groups, 2);
    TypedContinuationCompileOptions config;
    config.transport = private_frames ? TypedFrameTransport::PrivateArray : TypedFrameTransport::PhysicalAddress;
    config.private_frame_words = private_frames ? uint32_t(plan.default_arena_bytes / 4) : 0;
    for (uint32_t i = 0; i < 8; ++i)
        stages[i] = CompileTypedPipelineStage(native[i], "main", kinds[i], nullptr, "vel_rt_stage_" + std::to_string(i), config);
    auto program = BuildTypedRuntimeProgram(stages, groups, {2, UINT64_C(0x100000007)});
    if (!dump_path.empty()) {
        std::ofstream dump(dump_path, std::ios::binary);
        dump.write(reinterpret_cast<const char *>(program.spirv.data()), program.spirv.size() * 4);
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(program.spirv), "Final native runtime validation");
    Require(program.recommended_arena_bytes == plan.default_arena_bytes, "Private transport changed frame sizing");
    std::cout << "Runtime SPIR-V validated; per-invocation Private arena=" << plan.default_arena_bytes << " bytes\n";
    if (compile_only) return true;
    constexpr uint32_t count = 8, base = 2;
    constexpr uint64_t bytes = 65536, registry = 256, pages = 512, blas = 15360, tlas = 15872, sbt = 16384, pushes = 16896,
                       output = 17408, status = 18432;
    GPU gpu;
    if (!gpu.Init(program.spirv, bytes, VK_WHOLE_SIZE, true, VK_WHOLE_SIZE, 0)) return false;
    std::memset(gpu.mapped, 0, bytes);
    Write(gpu, 0, VEL_RT_RUNTIME_MAGIC);
    Write(gpu, 4, VEL_RT_RUNTIME_VERSION);
    if (!private_frames) {
        Write(gpu, 8, gpu.address + 20480);
        Write(gpu, 16, UINT64_C(45056));
    }
    Write(gpu, 24, plan.default_arena_bytes);
    Write(gpu, 32, uint64_t(count));
    Write(gpu, 40, 10u);
    Write(gpu, 44, 1u);
    Write(gpu, 48, 1u);
    Write(gpu, 56, uint64_t(base));
    Write(gpu, 64, gpu.address + registry);
    Write(gpu, 72, gpu.address + pushes);
    for (uint32_t i = 0; i < 8; ++i) {
        Write(gpu, pushes + i * 8, gpu.address + pushes + 128 + i * 16);
        Write(gpu, pushes + 128 + i * 16, UINT64_C(0x1000));
        Write(gpu, pushes + 136 + i * 16, i * 10u);
    }
    Write(gpu, 176, program.pipeline_identity);
    Write(gpu, 184, gpu.address + status);
    Write(gpu, 192, uint64_t(count));
    Write(gpu, registry, 0x52534156u);
    Write(gpu, registry + 4, 1u);
    Write(gpu, registry + 8, 3u);
    Write(gpu, registry + 12, 8u);
    Write(gpu, registry + 16, 7u);
    Write(gpu, registry + 20, 8u);
    Write(gpu, registry + 32, gpu.address + pages);
    for (uint32_t i = 0; i < 6; ++i) Write(gpu, pages + i * 2048, gpu.address + pages + (i + 1) * 2048);
    Write(gpu, pages + 6 * 2048 + 16 * 8, gpu.address + tlas);
    for (uint32_t i = 0; i < 5; ++i) WriteBytes(gpu, sbt + i * 64, program.group_handles[i].data(), 32);
    Write(gpu, sbt + 1 * 64 + 32, 13u);
    Write(gpu, sbt + 2 * 64 + 32, 11u);
    Write(gpu, sbt + 3 * 64 + 32, 19u);
    Write(gpu, sbt + 4 * 64 + 32, 17u);
    auto Region = [&](uint32_t offset, uint64_t address, uint64_t size) {
        Write(gpu, offset, gpu.address + address);
        Write(gpu, offset + 8, uint64_t(64));
        Write(gpu, offset + 16, size);
    };
    Region(80, sbt, 64);
    Region(104, sbt + 64, 64);
    Region(128, sbt + 2 * 64, 128);
    Region(152, sbt + 4 * 64, 64);
    VkDescriptorBufferInfo out{gpu.buffer, output, 10 * 64};
    VkWriteDescriptorSet descriptor{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptor.dstSet = gpu.set;
    descriptor.dstBinding = 0;
    descriptor.descriptorCount = 1;
    descriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor.pBufferInfo = &out;
    vkUpdateDescriptorSets(gpu.device, 1, &descriptor, 0, nullptr);
    for (uint32_t phase = 0; phase < (unused_attribute ? 1u : 4u); ++phase) {
        bool procedural = phase != 0, first_hit = phase == 2, terminate_report = phase == 3;
        AS(gpu, blas, tlas, procedural);
        Write(gpu, sbt + 32, uint32_t(procedural) | (first_hit ? 16u : 0u));
        Write(gpu, pushes + 136 + 5 * 16, terminate_report ? 51u : 50u);
        std::fill(gpu.mapped + output / 4, gpu.mapped + (output + 10 * 64) / 4, 0xdeadbeefu);
        std::fill(gpu.mapped + status / 4, gpu.mapped + status / 4 + count, 0xdeadbeefu);
        gpu.Dispatch(0, 0, count + 2);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = i + base, which = id % 4, calls = 1 + (id & 1);
            auto row = gpu.mapped + output / 4 + id * 16;
            Require(gpu.mapped[status / 4 + i] == 0,
                    "Runtime GPU status=" + std::to_string(gpu.mapped[status / 4 + i]) + " ray=" + std::to_string(id));
            uint32_t traced = id + 30;
            bool hit = which != 3 && (procedural || which != 1);
            if (procedural && hit) traced = id + 197 + (which == 0 ? 0 : 3);
            if (!procedural && which == 0) traced = id + 169;
            if (!procedural && which == 1) traced = id + 70;
            if (!procedural && which == 2) traced = id + 219;
            Require(row[0] == traced + 92 * calls && row[6] == traced, "Typed payload/late SSA return ray=" + std::to_string(id) +
                                                                           " actual=" + std::to_string(row[0]) +
                                                                           " snapshot=" + std::to_string(row[6]));
            Require(row[1] == id + 3 * calls && row[2] == (calls & 1) && row[7] == id && row[8] == 10,
                    "Callable/builtin state changed");
            bool first_accepted = hit && procedural && first_hit && which == 0;
            float ax = hit ? (procedural ? (first_accepted ? .125f : .5f) : .25f) : 0.f;
            float ay = hit ? (procedural ? (first_accepted ? .25f : .75f) : .5f) : 0.f;
            if (unused_attribute) ax = ay = 0.f;
            float hit_distance = first_accepted ? 2.f : 1.f;
            Require(row[3] == Bits(1 + ax + .5f * calls) && row[4] == Bits(2 + ay + calls) &&
                        row[5] == Bits(4 + (hit ? hit_distance : 0.f) + 2 * calls),
                    "Hit attribute snapshot/vector return changed");
            if (hit) {
                Require(row[10] == 4 && row[11] == Bits(1.f), "Shared BVH4 instance/object ray builtins changed");
                Require(procedural ? row[9] == (first_accepted ? 33u : 34u) : row[9] >= 254, "Hit kind state changed");
            }
            if (procedural && hit) {
                if (first_hit || (terminate_report && which == 2)) {
                    Require(row[12] == 0xdeadbeefu && row[13] == 0xdeadbeefu && row[14] == 0xdeadbeefu && row[15] == 0xdeadbeefu,
                            "Intersection continued after ray termination");
                    continue;
                }
                Require(row[12] == uint32_t(which == 0) && row[13] == 1, "Report late bool/any-hit disposition changed");
                Require(row[14] == Bits(which == 0 ? 2.f : 10.f) && row[15] == Bits(1.f),
                        "Intersection RayTmax did not update after accepted Report");
            }
        }
        std::cout << "One-dispatch shared BVH4/opaque token runtime: "
                  << (procedural ? "procedural Report/AH snapshot" : "opaque/nonopaque triangles Ignore/Terminate")
                  << (first_hit ? ", terminate-on-first-hit" : "") << (terminate_report ? ", Report AnyHit Terminate" : "")
                  << (unused_attribute ? ", unused vec3 triangle attribute" : "") << ", nested Trace+MISS+CHIT+Callable PASS\n";
    }
    Write(gpu, sbt + 4, 0u);
    gpu.Dispatch(0, 0, count);
    for (uint32_t i = 0; i < count; ++i)
        Require(gpu.mapped[status / 4 + i] == VEL_RT_STATUS_SBT, "Invalid SBT did not fail closed");
    Write(gpu, 40, UINT32_MAX);
    Write(gpu, 44, UINT32_MAX);
    Write(gpu, 48, 2u);
    gpu.Dispatch(0, 0, count);
    for (uint32_t i = 0; i < count; ++i)
        Require(gpu.mapped[status / 4 + i] == VEL_RT_STATUS_CONTEXT, "Full-width launch product overflow did not fail closed");
    return true;
}
}  // namespace
int main(int argc, char **argv) {
    try {
        bool private_frames = true, compile_only = false;
        std::string dump;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--bda")
                private_frames = false;
            else if (arg == "--compile-only")
                compile_only = true;
            else if (arg == "--dump" && i + 1 < argc)
                dump = argv[++i];
            else
                throw std::runtime_error("Unknown runtime test argument");
        }
        if (!Execute(private_frames, compile_only, dump)) return 77;
        if (!Execute(private_frames, compile_only, {}, true)) return 77;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
