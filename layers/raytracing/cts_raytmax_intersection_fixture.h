// Copyright (c) 2020 The Khronos Group Inc.
// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Exact generated IntersectionKHR assembly from Vulkan CTS revision
// d18e4c68543103371715845cd914c71e5859963e:
// dEQP-VK.ray_tracing_pipeline.builtin.raytmaxext.sect_aabs.
// Source: external/vulkancts/modules/vulkan/ray_tracing/
// vktRayTracingBuiltinTests.cpp.
// Captured from the unmodified external CTS QPA. Keep numeric IDs and source
// SPIR-V1.4 version when assembling; this fixture exercises the production
// BDA/PrivateArray stage-library path, not just the standalone SSBO wrapper.
inline constexpr char kCtsRayTMaxIntersectionSource[] = R"spv(
; SPIR-V
; Version: 1.4
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 69
; Schema: 0
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
%1 = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %4 "main" %13 %21 %27 %41 %68
OpDecorate %13 BuiltIn LaunchIdKHR
OpDecorate %21 BuiltIn RayTmaxKHR
OpDecorate %21 Volatile
OpDecorate %21 Coherent
OpDecorate %27 Binding 0
OpDecorate %27 DescriptorSet 0
OpDecorate %41 BuiltIn LaunchSizeKHR
%2 = OpTypeVoid
%3 = OpTypeFunction %2
%6 = OpTypeInt 32 1
%7 = OpTypeVector %6 3
%8 = OpTypePointer Function %7
%10 = OpTypeInt 32 0
%11 = OpTypeVector %10 3
%12 = OpTypePointer Input %11
%13 = OpVariable %12 Input
%16 = OpTypePointer Function %6
%18 = OpTypeFloat 32
%19 = OpConstant %18 1048576
%20 = OpTypePointer Input %18
%21 = OpVariable %20 Input
%25 = OpTypeImage %6 3D 0 0 0 2 R32i
%26 = OpTypePointer UniformConstant %25
%27 = OpVariable %26 UniformConstant
%30 = OpConstant %10 0
%31 = OpTypePointer Image %6
%33 = OpConstant %10 1
%35 = OpTypePointer Function %18
%37 = OpTypePointer Input %10
%41 = OpVariable %12 Input
%47 = OpConstant %18 1
%56 = OpConstant %18 0.4375
%57 = OpConstant %18 0.25
%63 = OpConstant %10 126
%64 = OpTypeBool
%66 = OpTypeVector %18 3
%67 = OpTypePointer HitAttributeKHR %66
%68 = OpVariable %67 HitAttributeKHR
%4 = OpFunction %2 None %3
%5 = OpLabel
%9 = OpVariable %8 Function
%17 = OpVariable %16 Function
%36 = OpVariable %35 Function
%46 = OpVariable %35 Function
%14 = OpLoad %11 %13
%15 = OpBitcast %7 %14
OpStore %9 %15
%22 = OpLoad %18 %21
%23 = OpFMul %18 %19 %22
%24 = OpConvertFToS %6 %23
OpStore %17 %24
%28 = OpLoad %7 %9
%29 = OpLoad %6 %17
%32 = OpImageTexelPointer %31 %27 %28 %30
%34 = OpAtomicSMin %6 %32 %33 %30 %29
%38 = OpAccessChain %37 %13 %30
%39 = OpLoad %10 %38
%40 = OpConvertUToF %18 %39
%42 = OpAccessChain %37 %41 %30
%43 = OpLoad %10 %42
%44 = OpConvertUToF %18 %43
%45 = OpFDiv %18 %40 %44
OpStore %36 %45
%48 = OpAccessChain %37 %13 %33
%49 = OpLoad %10 %48
%50 = OpConvertUToF %18 %49
%51 = OpAccessChain %37 %41 %33
%52 = OpLoad %10 %51
%53 = OpConvertUToF %18 %52
%54 = OpFDiv %18 %50 %53
%55 = OpFAdd %18 %47 %54
OpStore %46 %55
%58 = OpLoad %18 %36
%59 = OpFMul %18 %57 %58
%60 = OpLoad %18 %46
%61 = OpFDiv %18 %59 %60
%62 = OpFAdd %18 %56 %61
%65 = OpReportIntersectionKHR %64 %62 %63
OpReturn
OpFunctionEnd
)spv";
