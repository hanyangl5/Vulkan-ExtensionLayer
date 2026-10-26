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
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_query.comp
            -o ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv
        DEPENDS tests/native_query.comp omm_opacity.glsl VERBATIM)
    add_custom_target(omm_native_query_spv DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv)
    add_executable(test_omm_native_query tests/test_omm_native_query.cpp)
    target_compile_features(test_omm_native_query PRIVATE cxx_std_17)
    target_compile_definitions(test_omm_native_query PRIVATE NOMINMAX)
    target_link_libraries(test_omm_native_query PRIVATE Vulkan::Vulkan vel_omm_core)
    add_dependencies(test_omm_native_query omm_native_query_spv)
    add_test(NAME omm_native_query COMMAND test_omm_native_query ${CMAKE_CURRENT_BINARY_DIR}/native_query.comp.spv)
    set_tests_properties(omm_native_query PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
endif()
