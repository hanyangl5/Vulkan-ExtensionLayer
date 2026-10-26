# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
# Shared by the full layer and VKRT_BVH4_COMPONENTS_ONLY configurations.
include_guard(DIRECTORY)
get_filename_component(_bvh4_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_bvh4_tests "${_bvh4_root}/tests")
set(_bvh4_shaders "${_bvh4_root}/shaders")
set(_bvh4_headers "${_bvh4_root}/src")
set(_bvh4_binary "${CMAKE_CURRENT_BINARY_DIR}/bvh4-tests")

option(VEL_RT_BVH4_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(VKRT_BVH4_COMPONENTS_ONLY)
    set(_bvh4_gpu_default OFF)
else()
    set(_bvh4_gpu_default ON)
endif()
option(VEL_RT_BVH4_GPU_TESTS "Compile and execute the BVH4 compute encoder component" ${_bvh4_gpu_default})
option(VEL_RT_MESA_BUILDER_TESTS "Test the imported Mesa LBVH build followed by GPU BVH4 encoding" ON)

if(NOT TARGET vel_rt_bvh4_reference)
    add_library(vel_rt_bvh4_reference STATIC "${_bvh4_tests}/bvh4_reference.cpp" "${_bvh4_tests}/mesa_ir_adapter.cpp")
    target_include_directories(vel_rt_bvh4_reference PUBLIC "${_bvh4_tests}" "${_bvh4_headers}")
    set_target_properties(vel_rt_bvh4_reference PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
endif()
add_executable(vel_rt_bvh4_tests "${_bvh4_tests}/bvh4_tests.cpp" "${_bvh4_tests}/abi_c_check.c")
target_link_libraries(vel_rt_bvh4_tests PRIVATE vel_rt_bvh4_reference)
set_target_properties(vel_rt_bvh4_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(vel_rt_mesa_adapter_tests "${_bvh4_tests}/mesa_ir_adapter_tests.cpp")
target_link_libraries(vel_rt_mesa_adapter_tests PRIVATE vel_rt_bvh4_reference)
set_target_properties(vel_rt_mesa_adapter_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
find_path(VEL_RT_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
if(VEL_RT_VULKAN_INCLUDE_DIR)
    target_include_directories(vel_rt_mesa_adapter_tests PRIVATE "${VEL_RT_VULKAN_INCLUDE_DIR}")
    target_compile_definitions(vel_rt_mesa_adapter_tests PRIVATE VEL_RT_MESA_CHECK_VULKAN_ABI=1)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    foreach(_bvh4_target vel_rt_bvh4_reference vel_rt_bvh4_tests vel_rt_mesa_adapter_tests)
        target_compile_options(${_bvh4_target} PRIVATE -Wall -Wextra -Wconversion -Werror)
    endforeach()
    if(VEL_RT_BVH4_SANITIZERS)
        target_compile_options(vel_rt_bvh4_reference PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        # The static reference is also consumed by the full layer's CPU oracle.
        target_link_options(vel_rt_bvh4_reference INTERFACE -fsanitize=address,undefined)
    endif()
endif()
function(_bvh4_sanitize target)
    if(VEL_RT_BVH4_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()
_bvh4_sanitize(vel_rt_bvh4_tests)
_bvh4_sanitize(vel_rt_mesa_adapter_tests)
add_test(NAME portable_bvh4_reference COMMAND vel_rt_bvh4_tests)
add_test(NAME portable_bvh4_mesa_adapter COMMAND vel_rt_mesa_adapter_tests)
set_tests_properties(portable_bvh4_reference portable_bvh4_mesa_adapter PROPERTIES LABELS "offline;no_native_rt")

if(NOT VEL_RT_BVH4_GPU_TESTS)
    return()
endif()
find_package(Vulkan REQUIRED)
find_program(VEL_RT_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
find_program(VEL_RT_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(VEL_RT_ENCODE_SPV "${_bvh4_binary}/bvh4_encode.spv")
add_custom_command(OUTPUT "${VEL_RT_ENCODE_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bvh4_binary}"
    COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
        "${_bvh4_tests}/bvh4_encode.comp" -o "${VEL_RT_ENCODE_SPV}"
    COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${VEL_RT_ENCODE_SPV}"
    DEPENDS "${_bvh4_tests}/bvh4_encode.comp" VERBATIM)
add_custom_target(vel_rt_bvh4_encode_spv DEPENDS "${VEL_RT_ENCODE_SPV}")
set(VEL_RT_MESA_ENCODE_SPV "${_bvh4_binary}/bvh4_mesa_encode.spv")
add_custom_command(OUTPUT "${VEL_RT_MESA_ENCODE_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bvh4_binary}"
    COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
        -I "${_bvh4_shaders}" -I "${_bvh4_headers}"
        "${_bvh4_shaders}/bvh4_mesa_encode.comp" -o "${VEL_RT_MESA_ENCODE_SPV}"
    COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${VEL_RT_MESA_ENCODE_SPV}"
    DEPENDS "${_bvh4_shaders}/bvh4_mesa_encode.comp" "${_bvh4_shaders}/as_address.glsl" "${_bvh4_shaders}/bvh4_abi.glsl" VERBATIM)
add_custom_target(vel_rt_bvh4_mesa_encode_spv DEPENDS "${VEL_RT_MESA_ENCODE_SPV}")
add_executable(vel_rt_bvh4_gpu_tests "${_bvh4_tests}/bvh4_gpu_tests.cpp")
target_compile_definitions(vel_rt_bvh4_gpu_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(vel_rt_bvh4_gpu_tests PRIVATE vel_rt_bvh4_reference Vulkan::Vulkan)
set_target_properties(vel_rt_bvh4_gpu_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
_bvh4_sanitize(vel_rt_bvh4_gpu_tests)
add_dependencies(vel_rt_bvh4_gpu_tests vel_rt_bvh4_encode_spv vel_rt_bvh4_mesa_encode_spv)
add_test(NAME portable_bvh4_gpu_encode COMMAND vel_rt_bvh4_gpu_tests "${VEL_RT_ENCODE_SPV}" "${VEL_RT_MESA_ENCODE_SPV}")
set_tests_properties(portable_bvh4_gpu_encode PROPERTIES
    ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)

if(VEL_RT_MESA_BUILDER_TESTS)
    file(GLOB VEL_RT_MESA_BUILD_DEPS CONFIGURE_DEPENDS "${_bvh4_shaders}/*" "${_bvh4_headers}/*.h")
    set(VEL_RT_MESA_BUILD_SPV)
    foreach(pass vkrt_init leaf morton_sort lbvh_main lbvh_generate_ir)
        set(spv "${_bvh4_binary}/mesa-build/${pass}.spv")
        add_custom_command(OUTPUT "${spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bvh4_binary}/mesa-build"
            COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
                -I "${_bvh4_shaders}" -I "${_bvh4_headers}" "${_bvh4_shaders}/${pass}.comp" -o "${spv}"
            COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${spv}"
            DEPENDS ${VEL_RT_MESA_BUILD_DEPS} VERBATIM)
        list(APPEND VEL_RT_MESA_BUILD_SPV "${spv}")
    endforeach()
    add_custom_target(vel_rt_mesa_builder_spv DEPENDS ${VEL_RT_MESA_BUILD_SPV})
    add_dependencies(vel_rt_bvh4_gpu_tests vel_rt_mesa_builder_spv)
    add_test(NAME portable_bvh4_mesa_gpu_build COMMAND vel_rt_bvh4_gpu_tests
        "${VEL_RT_ENCODE_SPV}" "${VEL_RT_MESA_ENCODE_SPV}" --mesa-builder "${_bvh4_binary}/mesa-build")
    set_tests_properties(portable_bvh4_mesa_gpu_build PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

set(VEL_RT_AS_ADDRESS_SPV "${_bvh4_binary}/as_address_tests.spv")
add_custom_command(OUTPUT "${VEL_RT_AS_ADDRESS_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bvh4_binary}"
    COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
        -I "${_bvh4_shaders}" "${_bvh4_tests}/as_address_tests.comp" -o "${VEL_RT_AS_ADDRESS_SPV}"
    COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${VEL_RT_AS_ADDRESS_SPV}"
    DEPENDS "${_bvh4_tests}/as_address_tests.comp" "${_bvh4_shaders}/as_address.glsl" VERBATIM)
add_custom_target(vel_rt_as_address_spv DEPENDS "${VEL_RT_AS_ADDRESS_SPV}")
add_executable(vel_rt_as_address_gpu_tests "${_bvh4_tests}/as_address_gpu_tests.cpp")
target_link_libraries(vel_rt_as_address_gpu_tests PRIVATE Vulkan::Vulkan)
set_target_properties(vel_rt_as_address_gpu_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
_bvh4_sanitize(vel_rt_as_address_gpu_tests)
add_dependencies(vel_rt_as_address_gpu_tests vel_rt_as_address_spv)
add_test(NAME portable_as_address_gpu_resolver COMMAND vel_rt_as_address_gpu_tests "${VEL_RT_AS_ADDRESS_SPV}")
set_tests_properties(portable_as_address_gpu_resolver PROPERTIES
    ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
