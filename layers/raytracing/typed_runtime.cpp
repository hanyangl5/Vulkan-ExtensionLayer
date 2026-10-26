// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "typed_runtime.h"
#include "compiler.h"
#include "typed_runtime_sources.h"
#include <spirv/unified1/spirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vel::rt {
namespace {
using W = std::vector<uint32_t>;
void Need(bool value, const std::string &message) {
    if (!value) throw std::runtime_error("Typed runtime: " + message);
}
uint64_t Add(uint64_t a, uint64_t b) {
    Need(b <= UINT64_MAX - a, "stack sizing overflow");
    return a + b;
}
uint64_t Mul(uint64_t a, uint64_t b) {
    Need(!b || a <= UINT64_MAX / b, "stack sizing overflow");
    return a * b;
}
uint64_t Align(uint64_t a) { return Add(a, 15) & ~UINT64_C(15); }
// SPIR-V nominal IDs and source locations do not define the Vulkan payload
// interface. Canonical keys describe the complete scalar/composite structure.
struct Types {
    std::map<uint32_t, W> types;
    std::map<uint32_t, uint64_t> constants;
    std::map<uint32_t, std::string> keys;
    uint32_t model = UINT32_MAX;
    explicit Types(const W &words) {
        for (size_t p = 5; p < words.size();) {
            uint32_t n = words[p] >> 16, op = words[p] & 65535;
            Need(n && p + n <= words.size(), "malformed source metadata");
            if (op == spv::OpEntryPoint) {
                Need(model == UINT32_MAX, "selected source must have one entry point");
                model = words[p + 1];
            }
            if (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer)
                types[words[p + 1]] = W(words.begin() + p, words.begin() + p + n);
            if (op == spv::OpConstant && n >= 4) {
                uint64_t value = words[p + 3];
                if (n > 4) value |= uint64_t(words[p + 4]) << 32;
                constants[words[p + 2]] = value;
            }
            p += n;
        }
    }
    std::string Key(uint32_t id) {
        if (keys.count(id)) return keys.at(id);
        Need(types.count(id) != 0, "missing invocation value type");
        const auto &t = types.at(id);
        auto op = t[0] & 65535;
        std::string result;
        switch (op) {
            case spv::OpTypeBool:
                result = "b";
                break;
            case spv::OpTypeInt:
                result = "i" + std::to_string(t[2]) + ":" + std::to_string(t[3]);
                break;
            case spv::OpTypeFloat:
                result = "f" + std::to_string(t[2]);
                break;
            case spv::OpTypeVector:
            case spv::OpTypeMatrix:
                result = std::to_string(op) + "[" + std::to_string(t[3]) + "]" + Key(t[2]);
                break;
            case spv::OpTypeArray:
                Need(constants.count(t[3]) != 0, "nonconstant invocation array length");
                result = "a[" + std::to_string(constants.at(t[3])) + "]" + Key(t[2]);
                break;
            case spv::OpTypeStruct:
                result = "s{";
                for (size_t i = 2; i < t.size(); ++i) result += Key(t[i]) + ";";
                result += "}";
                break;
            default:
                Need(false, "unsupported invocation interface type");
        }
        keys[id] = result;
        return result;
    }
};
std::string Filter(const char *text, bool traversal = false) {
    std::istringstream in(text);
    std::ostringstream out;
    for (std::string line; std::getline(in, line);) {
        if (line.find("#include") != std::string::npos || line.find("#pragma once") != std::string::npos) continue;
        if (traversal && line.find("layout(constant_id = 6)") != std::string::npos)
            out << "uint64_t vkrt_as_registry_context;\n";
        else
            out << line << '\n';
    }
    return out.str();
}
template <class F>
void Table(std::ostringstream &out, const char *name, size_t count, F fn, const char *type = "uint") {
    out << "const " << type << " " << name << "[" << count << "]=" << type << "[" << count << "](";
    for (size_t i = 0; i < count; ++i) out << (i ? "," : "") << fn(i) << (std::string(type) == "uint64_t" ? "ul" : "u");
    out << ");\n";
}
}  // namespace

TypedRuntimeArenaPlan PlanTypedRuntimeArena(const std::vector<TypedContinuationProgram> &stages,
                                            const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups, uint32_t depth) {
    TypedRuntimeArenaPlan plan;
    uint64_t attribute = 8;
    for (const auto &s : stages)
        for (const auto &v : s.invocation_variables)
            if (v.storage_class == spv::StorageClassHitAttributeKHR) attribute = std::max(attribute, v.size);
    plan.trace_node_bytes = Add(512, Mul(2, Align(attribute)));
    std::vector<uint32_t> models;
    for (const auto &s : stages) {
        models.push_back(Types(s.source).model);
        uint64_t charge = Align(Add(128, s.frame_stride_bytes));
        // A Trace caller retains its traversal node while any child executes.
        if (models.back() == spv::ExecutionModelRayGenerationKHR || models.back() == spv::ExecutionModelClosestHitKHR ||
            models.back() == spv::ExecutionModelMissKHR)
            charge = Add(charge, plan.trace_node_bytes);
        if (models.back() == spv::ExecutionModelRayGenerationKHR) charge = Add(charge, 16);  // null-offset sentinel
        plan.stage_stack_bytes.push_back(charge);
    }
    std::map<uint32_t, uint64_t> maxima;
    auto Charge = [&](uint32_t i) {
        if (i == VK_SHADER_UNUSED_KHR) return;
        Need(i < stages.size(), "group stack stage out of range");
        maxima[models[i]] = std::max(maxima[models[i]], plan.stage_stack_bytes[i]);
    };
    for (const auto &g : groups) {
        Charge(g.generalShader);
        Charge(g.closestHitShader);
        Charge(g.anyHitShader);
        Charge(g.intersectionShader);
    }
    uint64_t hit = maxima[spv::ExecutionModelClosestHitKHR], miss = maxima[spv::ExecutionModelMissKHR];
    uint64_t first = std::max({hit, miss, Add(maxima[spv::ExecutionModelIntersectionKHR], maxima[spv::ExecutionModelAnyHitKHR])});
    plan.default_arena_bytes =
        Align(Add(Add(maxima[spv::ExecutionModelRayGenerationKHR], depth ? first : 0),
                  Add(Mul(depth ? depth - 1 : 0, std::max(hit, miss)), Mul(2, maxima[spv::ExecutionModelCallableKHR]))));
    return plan;
}
uint64_t PlanTypedRuntimeArenaBytes(const std::vector<TypedContinuationProgram> &stages,
                                    const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups, uint32_t depth) {
    return PlanTypedRuntimeArena(stages, groups, depth).default_arena_bytes;
}

TypedRuntimeProgram BuildTypedRuntimeProgram(const std::vector<TypedContinuationProgram> &stages,
                                             const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
                                             const TypedRuntimeBuildOptions &options) {
    Need(!stages.empty() && !groups.empty(), "empty pipeline");
    Need(options.pipeline_identity != 0, "zero pipeline identity");
    Need(stages.size() < UINT32_MAX && groups.size() < UINT32_MAX, "too many stages/groups");
    TypedRuntimeProgram result;
    result.pipeline_identity = options.pipeline_identity;
    result.max_ray_recursion_depth = options.max_ray_recursion_depth;
    std::vector<Types> types;
    std::set<std::string> exports;
    std::map<std::string, uint32_t> canonical;
    std::vector<std::map<uint32_t, uint32_t>> type_ids(stages.size());
    auto Intern = [&](const std::string &key) {
        if (!canonical.count(key)) {
            Need(canonical.size() < UINT32_MAX - 1, "too many canonical interface types");
            canonical[key] = uint32_t(canonical.size() + 1);
        }
        return canonical.at(key);
    };
    // Built-in triangle attributes are two float32 barycentrics, rather than
    // a source-defined interface type. GLSL also permits array/block containers
    // for these values, and unused declarations need not have the vec2 type.
    // Reserve a marker outside the source canonical type namespace.
    constexpr uint32_t triangle_type = UINT32_MAX;
    for (size_t i = 0; i < stages.size(); ++i) {
        const auto &s = stages[i];
        Need(s.transport != TypedFrameTransport::StorageBuffer && s.system_header_bytes == VEL_RT_SYSTEM_BYTES &&
                 !s.step_export_name.empty(),
             "stage is not a typed BDA library");
        Need(s.transport == stages.front().transport && s.private_frame_words == stages.front().private_frame_words,
             "stage frame transports/budgets differ");
        Need(exports.insert(s.step_export_name).second, "duplicate stage export");
        Need(s.frame_stride_bytes && s.frame_stride_bytes % 4 == 0, "invalid stage frame size");
        types.emplace_back(s.source);
        result.max_shader_frame_bytes = std::max(result.max_shader_frame_bytes, s.frame_stride_bytes);
        for (const auto &v : s.invocation_variables) {
            type_ids[i][v.type] = Intern(types.back().Key(v.type));
            if (v.storage_class == spv::StorageClassHitAttributeKHR) {
                Need(v.size <= UINT32_MAX, "hit attribute size");
                result.max_hit_attribute_bytes = std::max(result.max_hit_attribute_bytes, uint32_t(v.size));
            }
        }
    }
    result.max_hit_attribute_bytes = std::max(result.max_hit_attribute_bytes, 8u);
    auto Stage = [&](uint32_t index, std::initializer_list<uint32_t> models) {
        Need(index != VK_SHADER_UNUSED_KHR && index < stages.size(), "invalid group stage index");
        Need(std::find(models.begin(), models.end(), types[index].model) != models.end(), "group execution model mismatch");
    };
    for (const auto &g : groups) {
        Need(
            g.sType == VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR && !g.pNext && !g.pShaderGroupCaptureReplayHandle,
            "unsupported shader group extension/capture replay");
        if (g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
            Stage(g.generalShader,
                  {spv::ExecutionModelRayGenerationKHR, spv::ExecutionModelMissKHR, spv::ExecutionModelCallableKHR});
            Need(g.closestHitShader == VK_SHADER_UNUSED_KHR && g.anyHitShader == VK_SHADER_UNUSED_KHR &&
                     g.intersectionShader == VK_SHADER_UNUSED_KHR,
                 "general group has hit stages");
        } else {
            Need(g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR ||
                     g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
                 "unknown group type");
            Need(g.generalShader == VK_SHADER_UNUSED_KHR, "hit group has general stage");
            if (g.closestHitShader != VK_SHADER_UNUSED_KHR) Stage(g.closestHitShader, {spv::ExecutionModelClosestHitKHR});
            if (g.anyHitShader != VK_SHADER_UNUSED_KHR) Stage(g.anyHitShader, {spv::ExecutionModelAnyHitKHR});
            if (g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR)
                Stage(g.intersectionShader, {spv::ExecutionModelIntersectionKHR});
            else
                Need(g.intersectionShader == VK_SHADER_UNUSED_KHR, "triangle group has intersection stage");
        }
    }
    auto arena = PlanTypedRuntimeArena(stages, groups, options.max_ray_recursion_depth);
    result.recommended_arena_bytes = arena.default_arena_bytes;
    result.stage_stack_bytes = arena.stage_stack_bytes;
    result.private_frame_words = stages.front().private_frame_words;
    std::ostringstream out;
    out << "#version 460\n#extension GL_EXT_buffer_reference : require\n"
           "#extension GL_EXT_buffer_reference2 : require\n#extension GL_EXT_scalar_block_layout : require\n"
           "#extension GL_EXT_shader_explicit_arithmetic_types : require\n";
    out << Filter(kTypedRuntimeAbi) << Filter(kTypedRuntimeSystem) << Filter(kTypedRuntimeBvh4) << Filter(kTypedRuntimeAddress)
        << Filter(kTypedRuntimeTraversal, true);
    if (result.private_frame_words) out << "#define RT_PRIVATE_WORDS " << result.private_frame_words << "\n";
    if (options.context_via_push_constants) out << "#define RT_CONTEXT_PUSH 1\n";
    out << "const uint rt_stage_count=" << stages.size() << "u;\nconst uint rt_group_count=" << groups.size()
        << "u;\nconst uint rt_max_depth=" << options.max_ray_recursion_depth << "u;\nconst uint rt_triangle_type=" << triangle_type
        << "u;\nconst uint64_t rt_pipeline_id=" << options.pipeline_identity
        << "ul;\nconst uint64_t rt_attr_stride=" << Align(result.max_hit_attribute_bytes) << "ul;\n";
    Table(out, "rt_frame_size", stages.size(), [&](size_t i) { return stages[i].frame_stride_bytes; }, "uint64_t");
    Table(out, "rt_control_offset", stages.size(), [&](size_t i) { return stages[i].control_byte_offset; });
    Table(out, "rt_models", stages.size(), [&](size_t i) { return types[i].model; });
    Table(out, "rt_general", groups.size(), [&](size_t i) { return groups[i].generalShader; });
    Table(out, "rt_closest", groups.size(), [&](size_t i) { return groups[i].closestHitShader; });
    Table(out, "rt_any", groups.size(), [&](size_t i) { return groups[i].anyHitShader; });
    Table(out, "rt_intersection", groups.size(), [&](size_t i) { return groups[i].intersectionShader; });
    Table(out, "rt_group_type", groups.size(), [&](size_t i) { return uint32_t(groups[i].type); });
    for (const auto &s : stages) out << "void " << s.step_export_name << "(uint64_t frame){}\n";
    out << "void rt_step(uint stage,uint64_t frame){switch(stage){\n";
    for (size_t i = 0; i < stages.size(); ++i) out << "case " << i << "u:" << stages[i].step_export_name << "(frame);break;\n";
    out << "}}\nuint rt_type(uint stage,uint id){switch(stage){\n";
    for (size_t i = 0; i < stages.size(); ++i) {
        out << "case " << i << "u:switch(id){";
        for (auto [id, key] : type_ids[i]) out << "case " << id << "u:return " << key << "u;";
        out << "}break;\n";
    }
    out << "}return 0u;}\n";
    // One incoming payload/callable/attribute variable is allowed per stage.
    // Metadata is retained even for declarations unused by source instructions.
    out << "uvec4 rt_interface(uint stage,uint storage){switch(stage){\n";
    for (size_t i = 0; i < stages.size(); ++i) {
        out << "case " << i << "u:switch(storage){";
        std::set<uint32_t> seen;
        for (auto &v : stages[i].invocation_variables) {
            if (v.storage_class != spv::StorageClassIncomingRayPayloadKHR &&
                v.storage_class != spv::StorageClassIncomingCallableDataKHR && v.storage_class != spv::StorageClassHitAttributeKHR)
                continue;
            Need(seen.insert(v.storage_class).second, "multiple incoming/attribute interfaces");
            Need(v.offset <= UINT32_MAX && v.size <= UINT32_MAX, "interface exceeds source frame ABI");
            out << "case " << v.storage_class << "u:return uvec4(" << v.offset << "u," << v.size << "u," << type_ids[i].at(v.type)
                << "u,1u);";
        }
        out << "}break;\n";
    }
    out << "}return uvec4(0u);}\n" << kTypedRuntimeBody;
    auto scheduler = CompileGLSL(out.str(), VK_SHADER_STAGE_COMPUTE_BIT);
    result.spirv = LinkTypedPipelineProgram(scheduler, stages);
    // Eliminate aggregate parameter shuttles and trivial codec calls before
    // native compilation. No fast-math or floating-point contraction passes
    // are used; source NoContraction and execution controls remain intact.
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
    std::string cleanup_error;
    optimizer.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
        cleanup_error += std::string(message) + "\n";
    });
    optimizer.RegisterPerformancePasses();
    W cleaned;
    spvtools::ValidatorOptions validation;
    validation.SetScalarBlockLayout(true);
    spvtools::OptimizerOptions cleanup_options;
    cleanup_options.set_validator_options(validation);
    bool optimized = optimizer.Run(result.spirv.data(), result.spirv.size(), &cleaned, cleanup_options);
    Need(optimized, "native runtime cleanup failed: " + cleanup_error);
    result.spirv = std::move(cleaned);
    for (uint32_t i = 0; i < groups.size(); ++i) {
        std::array<uint32_t, 8> words{i,
                                      VEL_RT_SBT_HANDLE_MAGIC,
                                      uint32_t(options.pipeline_identity),
                                      uint32_t(options.pipeline_identity >> 32),
                                      i ^ 0xa511e9b3u,
                                      ~i,
                                      uint32_t(options.pipeline_identity) ^ 0x63d83595u,
                                      uint32_t(options.pipeline_identity >> 32) ^ 0x9e3779b9u};
        std::array<uint8_t, VEL_RT_SBT_HANDLE_BYTES> handle;
        std::memcpy(handle.data(), words.data(), handle.size());
        result.group_handles.push_back(handle);
    }
    return result;
}
}  // namespace vel::rt
