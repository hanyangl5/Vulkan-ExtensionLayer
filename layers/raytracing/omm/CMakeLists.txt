# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
add_library(vel_omm_core STATIC omm_opacity.cpp omm_storage.cpp)
target_compile_features(vel_omm_core PUBLIC cxx_std_17)
target_include_directories(vel_omm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(vel_omm_core PRIVATE /W4 /WX)
else()
    target_compile_options(vel_omm_core PRIVATE -Wall -Wextra -Werror)
endif()

foreach(component opacity storage)
    add_executable(test_omm_${component} tests/test_omm_${component}.cpp)
    target_link_libraries(test_omm_${component} PRIVATE vel_omm_core)
    add_test(NAME omm_${component} COMMAND test_omm_${component})
    set_tests_properties(omm_${component} PROPERTIES LABELS "offline;omm")
endforeach()

option(OMM_BUILD_NATIVE_PROOFS "Build experimental native RT semantic proofs" ON)
if(OMM_BUILD_NATIVE_PROOFS)
    find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
    find_program(OMM_SPIRV_VALIDATOR NAMES spirv-val HINTS "${Vulkan_ROOT}/Bin" "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    if(NOT TARGET SPIRV-Tools-static)
        # Some SDK packages relocate their CMake files without fixing the
        # imported prefix. Locate the public headers and library directly.
        find_path(OMM_SPIRV_TOOLS_INCLUDE_DIR spirv-tools/libspirv.hpp HINTS ${Vulkan_INCLUDE_DIRS} REQUIRED)
        find_library(OMM_SPIRV_TOOLS_LIBRARY NAMES SPIRV-Tools
            HINTS "${Vulkan_ROOT}/Lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
        add_library(SPIRV-Tools-static STATIC IMPORTED)
        set_target_properties(SPIRV-Tools-static PROPERTIES IMPORTED_LOCATION "${OMM_SPIRV_TOOLS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OMM_SPIRV_TOOLS_INCLUDE_DIR}")
    endif()
    add_library(vel_omm_query_rewrite STATIC omm_query_rewrite.cpp)
    target_compile_features(vel_omm_query_rewrite PUBLIC cxx_std_17)
    target_include_directories(vel_omm_query_rewrite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vel_omm_query_rewrite PUBLIC SPIRV-Tools-static)
    if(MSVC)
        target_compile_options(vel_omm_query_rewrite PRIVATE /W4 /WX)
    else()
        target_compile_options(vel_omm_query_rewrite PRIVATE -Wall -Wextra -Werror)
    endif()
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_query.comp
            -o ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv
        COMMAND ${OMM_SPIRV_VALIDATOR} --target-env vulkan1.2 ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv
        DEPENDS tests/native_query.comp omm_opacity.glsl omm_lookup.glsl VERBATIM)
    add_custom_target(omm_native_query_spv DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv)
    add_executable(test_omm_native_query tests/test_omm_native_query.cpp)
    target_compile_features(test_omm_native_query PRIVATE cxx_std_17)
    target_compile_definitions(test_omm_native_query PRIVATE NOMINMAX)
    target_link_libraries(test_omm_native_query PRIVATE Vulkan::Vulkan vel_omm_core)
    add_dependencies(test_omm_native_query omm_native_query_spv)
    add_test(NAME omm_native_query COMMAND test_omm_native_query ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv)
    set_tests_properties(omm_native_query PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")

    set(extra_shaders query_state.comp omm_build.comp gpu_build_generate.comp gpu_lookup.comp)
    foreach(stage rgen rmiss rahit rchit rint)
        list(APPEND extra_shaders native_pipeline.${stage})
    endforeach()
    set(shader_outputs)
    foreach(shader ${extra_shaders})
        if(shader STREQUAL "omm_build.comp")
            set(shader_source ${CMAKE_CURRENT_SOURCE_DIR}/${shader})
        else()
            set(shader_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/${shader})
        endif()
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv
            COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2 ${shader_source}
                -o ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv
            COMMAND ${OMM_SPIRV_VALIDATOR} --target-env vulkan1.2 ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv
            DEPENDS ${shader_source} tests/native_pipeline.glsl omm_opacity.glsl omm_lookup.glsl VERBATIM)
        list(APPEND shader_outputs ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv)
    endforeach()
    add_custom_target(omm_extra_spv DEPENDS ${shader_outputs})
    foreach(proof native_pipeline gpu_build gpu_lookup query_rewrite)
        add_executable(test_omm_${proof} tests/test_omm_${proof}.cpp)
        target_link_libraries(test_omm_${proof} PRIVATE Vulkan::Vulkan vel_omm_core)
        target_compile_definitions(test_omm_${proof} PRIVATE NOMINMAX)
        add_dependencies(test_omm_${proof} omm_extra_spv)
    endforeach()
    target_link_libraries(test_omm_query_rewrite PRIVATE vel_omm_query_rewrite)
    add_executable(test_omm_query_graph tests/test_omm_query_graph.cpp)
    target_link_libraries(test_omm_query_graph PRIVATE vel_omm_query_rewrite)
    add_test(NAME omm_query_graph COMMAND test_omm_query_graph)
    set_tests_properties(omm_query_graph PROPERTIES LABELS "offline;omm")
    add_test(NAME omm_native_pipeline COMMAND test_omm_native_pipeline ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME omm_gpu_build COMMAND test_omm_gpu_build ${CMAKE_CURRENT_BINARY_DIR}/omm_build.comp.spv
        ${CMAKE_CURRENT_BINARY_DIR}/gpu_build_generate.comp.spv)
    add_test(NAME omm_gpu_lookup COMMAND test_omm_gpu_lookup ${CMAKE_CURRENT_BINARY_DIR}/gpu_lookup.comp.spv
        ${CMAKE_CURRENT_BINARY_DIR}/omm_build.comp.spv)
    add_test(NAME omm_query_rewrite COMMAND test_omm_query_rewrite ${CMAKE_CURRENT_BINARY_DIR}/query_state.comp.spv)
    add_test(NAME omm_query_rewrite_gpu COMMAND test_omm_query_rewrite ${CMAKE_CURRENT_BINARY_DIR}/query_state.comp.spv --gpu)
    set_tests_properties(omm_query_rewrite PROPERTIES LABELS "offline;omm")
    set_tests_properties(omm_native_pipeline omm_gpu_build omm_gpu_lookup omm_query_rewrite_gpu PROPERTIES
        LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
endif()
