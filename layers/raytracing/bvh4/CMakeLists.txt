# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
project(VELRTBvh4Reference LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(VEL_RT_BVH4_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
add_library(vel_rt_bvh4_reference STATIC bvh4_reference.cpp mesa_ir_adapter.cpp)
target_include_directories(vel_rt_bvh4_reference PUBLIC .)
add_executable(vel_rt_bvh4_tests bvh4_tests.cpp abi_c_check.c)
target_link_libraries(vel_rt_bvh4_tests PRIVATE vel_rt_bvh4_reference)
add_executable(vel_rt_mesa_adapter_tests mesa_ir_adapter_tests.cpp)
target_link_libraries(vel_rt_mesa_adapter_tests PRIVATE vel_rt_bvh4_reference)
find_path(VEL_RT_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
if(VEL_RT_VULKAN_INCLUDE_DIR)
    target_include_directories(vel_rt_mesa_adapter_tests PRIVATE "${VEL_RT_VULKAN_INCLUDE_DIR}")
    target_compile_definitions(vel_rt_mesa_adapter_tests PRIVATE VEL_RT_MESA_CHECK_VULKAN_ABI=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vel_rt_bvh4_reference PRIVATE -Wall -Wextra -Wconversion -Werror)
    target_compile_options(vel_rt_bvh4_tests PRIVATE -Wall -Wextra -Wconversion -Werror)
    target_compile_options(vel_rt_mesa_adapter_tests PRIVATE -Wall -Wextra -Wconversion -Werror)
    if(VEL_RT_BVH4_SANITIZERS)
        target_compile_options(vel_rt_bvh4_reference PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_compile_options(vel_rt_bvh4_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(vel_rt_bvh4_tests PRIVATE -fsanitize=address,undefined)
        target_compile_options(vel_rt_mesa_adapter_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(vel_rt_mesa_adapter_tests PRIVATE -fsanitize=address,undefined)
    endif()
endif()
enable_testing()
add_test(NAME portable_bvh4_reference COMMAND vel_rt_bvh4_tests)
add_test(NAME portable_bvh4_mesa_adapter COMMAND vel_rt_mesa_adapter_tests)

option(VEL_RT_BVH4_GPU_TESTS "Compile and execute the BVH4 compute encoder component" OFF)
if(VEL_RT_BVH4_GPU_TESTS)
    find_package(Vulkan REQUIRED)
    find_program(VEL_RT_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    find_program(VEL_RT_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    set(VEL_RT_ENCODE_SPV "${CMAKE_CURRENT_BINARY_DIR}/bvh4_encode.spv")
    add_custom_command(OUTPUT "${VEL_RT_ENCODE_SPV}"
        COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
            "${CMAKE_CURRENT_SOURCE_DIR}/bvh4_encode.comp" -o "${VEL_RT_ENCODE_SPV}"
        COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${VEL_RT_ENCODE_SPV}"
        DEPENDS bvh4_encode.comp VERBATIM)
    add_custom_target(vel_rt_bvh4_encode_spv DEPENDS "${VEL_RT_ENCODE_SPV}")
    set(VEL_RT_MESA_ENCODE_SPV "${CMAKE_CURRENT_BINARY_DIR}/bvh4_mesa_encode.spv")
    add_custom_command(OUTPUT "${VEL_RT_MESA_ENCODE_SPV}"
        COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
            "${CMAKE_CURRENT_SOURCE_DIR}/bvh4_mesa_encode.comp" -o "${VEL_RT_MESA_ENCODE_SPV}"
        COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${VEL_RT_MESA_ENCODE_SPV}"
        DEPENDS bvh4_mesa_encode.comp as_address.glsl bvh4_abi.glsl VERBATIM)
    add_custom_target(vel_rt_bvh4_mesa_encode_spv DEPENDS "${VEL_RT_MESA_ENCODE_SPV}")
    add_executable(vel_rt_bvh4_gpu_tests bvh4_gpu_tests.cpp)
    target_compile_definitions(vel_rt_bvh4_gpu_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(vel_rt_bvh4_gpu_tests PRIVATE vel_rt_bvh4_reference Vulkan::Vulkan)
    if(VEL_RT_BVH4_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(vel_rt_bvh4_gpu_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(vel_rt_bvh4_gpu_tests PRIVATE -fsanitize=address,undefined)
    endif()
    add_dependencies(vel_rt_bvh4_gpu_tests vel_rt_bvh4_encode_spv)
    add_dependencies(vel_rt_bvh4_gpu_tests vel_rt_bvh4_mesa_encode_spv)
    add_test(NAME portable_bvh4_gpu_encode COMMAND vel_rt_bvh4_gpu_tests "${VEL_RT_ENCODE_SPV}" "${VEL_RT_MESA_ENCODE_SPV}")
    set_tests_properties(portable_bvh4_gpu_encode PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
    option(VEL_RT_MESA_BUILDER_TESTS "Test the imported Mesa LBVH build followed by GPU BVH4 encoding" ON)
    if(VEL_RT_MESA_BUILDER_TESTS)
        set(VEL_RT_MESA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bvh")
        file(GLOB VEL_RT_MESA_BUILD_DEPS CONFIGURE_DEPENDS "${VEL_RT_MESA_SOURCE_DIR}/*")
        set(VEL_RT_MESA_BUILD_SPV)
        foreach(pass vkrt_init leaf morton_sort lbvh_main lbvh_generate_ir)
            set(spv "${CMAKE_CURRENT_BINARY_DIR}/mesa-build/${pass}.spv")
            add_custom_command(OUTPUT "${spv}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/mesa-build"
                COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
                    -I "${VEL_RT_MESA_SOURCE_DIR}" "${VEL_RT_MESA_SOURCE_DIR}/${pass}.comp" -o "${spv}"
                COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${spv}"
                DEPENDS ${VEL_RT_MESA_BUILD_DEPS} VERBATIM)
            list(APPEND VEL_RT_MESA_BUILD_SPV "${spv}")
        endforeach()
        add_custom_target(vel_rt_mesa_builder_spv DEPENDS ${VEL_RT_MESA_BUILD_SPV})
        add_dependencies(vel_rt_bvh4_gpu_tests vel_rt_mesa_builder_spv)
        add_test(NAME portable_bvh4_mesa_gpu_build COMMAND vel_rt_bvh4_gpu_tests
            "${VEL_RT_ENCODE_SPV}" "${VEL_RT_MESA_ENCODE_SPV}" --mesa-builder "${CMAKE_CURRENT_BINARY_DIR}/mesa-build")
        set_tests_properties(portable_bvh4_mesa_gpu_build PROPERTIES
            ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
endif()

if(VEL_RT_BVH4_GPU_TESTS)
    set(VEL_RT_AS_ADDRESS_SPV "${CMAKE_CURRENT_BINARY_DIR}/as_address_tests.spv")
    add_custom_command(OUTPUT "${VEL_RT_AS_ADDRESS_SPV}"
        COMMAND "${VEL_RT_GLSLC}" --target-env=vulkan1.2 -fshader-stage=compute
            -I "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/as_address_tests.comp" -o "${VEL_RT_AS_ADDRESS_SPV}"
        COMMAND "${VEL_RT_SPIRV_VAL}" --target-env vulkan1.2 --scalar-block-layout "${VEL_RT_AS_ADDRESS_SPV}"
        DEPENDS as_address_tests.comp as_address.glsl VERBATIM)
    add_custom_target(vel_rt_as_address_spv DEPENDS "${VEL_RT_AS_ADDRESS_SPV}")
    add_executable(vel_rt_as_address_gpu_tests as_address_gpu_tests.cpp)
    target_link_libraries(vel_rt_as_address_gpu_tests PRIVATE Vulkan::Vulkan)
    if(VEL_RT_BVH4_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(vel_rt_as_address_gpu_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(vel_rt_as_address_gpu_tests PRIVATE -fsanitize=address,undefined)
    endif()
    add_dependencies(vel_rt_as_address_gpu_tests vel_rt_as_address_spv)
    add_test(NAME portable_as_address_gpu_resolver COMMAND vel_rt_as_address_gpu_tests "${VEL_RT_AS_ADDRESS_SPV}")
    set_tests_properties(portable_as_address_gpu_resolver PROPERTIES
        ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
