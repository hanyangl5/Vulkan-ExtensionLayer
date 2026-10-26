# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
set(_typed_source_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_typed_common_dir "${_typed_source_dir}/../common/rt_fallback")
set(_typed_traversal_dir "${_typed_source_dir}/../../vkrt_layer/layer/traversal")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_typed_source_dir}/typed_runtime.glsl" "${_typed_common_dir}/bvh4_abi.glsl"
    "${_typed_common_dir}/as_address.glsl" "${_typed_traversal_dir}/sw_ray_query.glsl"
    "${_typed_source_dir}/typed_runtime_abi.h" "${_typed_source_dir}/typed_system_abi.h")
file(READ "${_typed_source_dir}/typed_runtime.glsl" TYPED_RUNTIME_SOURCE)
file(READ "${_typed_common_dir}/bvh4_abi.glsl" TYPED_RUNTIME_BVH4_SOURCE)
file(READ "${_typed_common_dir}/as_address.glsl" TYPED_RUNTIME_ADDRESS_SOURCE)
file(READ "${_typed_traversal_dir}/sw_ray_query.glsl" TYPED_RUNTIME_TRAVERSAL_SOURCE)
file(READ "${_typed_source_dir}/typed_runtime_abi.h" TYPED_RUNTIME_ABI_SOURCE)
file(READ "${_typed_source_dir}/typed_system_abi.h" TYPED_RUNTIME_SYSTEM_SOURCE)
configure_file("${_typed_source_dir}/typed_runtime_sources.h.in" typed_runtime_sources.h @ONLY)
