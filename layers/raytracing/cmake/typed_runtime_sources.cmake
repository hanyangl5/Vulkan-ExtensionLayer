# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
get_filename_component(_typed_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_typed_root}/shaders/typed_runtime.glsl" "${_typed_root}/shaders/bvh4_abi.glsl"
    "${_typed_root}/shaders/as_address.glsl" "${_typed_root}/shaders/sw_ray_query.glsl"
    "${_typed_root}/src/typed_runtime_abi.h" "${_typed_root}/src/typed_system_abi.h")
file(READ "${_typed_root}/shaders/typed_runtime.glsl" TYPED_RUNTIME_SOURCE)
file(READ "${_typed_root}/shaders/bvh4_abi.glsl" TYPED_RUNTIME_BVH4_SOURCE)
file(READ "${_typed_root}/shaders/as_address.glsl" TYPED_RUNTIME_ADDRESS_SOURCE)
file(READ "${_typed_root}/shaders/sw_ray_query.glsl" TYPED_RUNTIME_TRAVERSAL_SOURCE)
file(READ "${_typed_root}/src/typed_runtime_abi.h" TYPED_RUNTIME_ABI_SOURCE)
file(READ "${_typed_root}/src/typed_system_abi.h" TYPED_RUNTIME_SYSTEM_SOURCE)
configure_file("${CMAKE_CURRENT_LIST_DIR}/typed_runtime_sources.h.in" typed_runtime_sources.h @ONLY)
