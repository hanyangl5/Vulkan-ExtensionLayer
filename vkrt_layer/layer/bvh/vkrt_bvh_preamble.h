/* SPDX-License-Identifier: MIT */
/* GLSL-facing preamble for the Mesa BVH builder sources. */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_vote : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_spirv_intrinsics : require
#pragma use_vulkan_memory_model

/* vk_bvh_defines.h has a C representation and a GLSL representation. */
#ifndef VULKAN
#define VULKAN
#endif
