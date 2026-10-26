# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/continuation_runtime.glsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/continuation_runtime.glsl" CONTINUATION_RUNTIME_SOURCE)
configure_file("${CMAKE_CURRENT_LIST_DIR}/continuation_runtime.h.in" continuation_runtime.h @ONLY)

add_executable(vel_rt_continuation_tests tests/continuation_tests.cpp)
target_link_libraries(vel_rt_continuation_tests PRIVATE vkrt_typed_compiler)
target_include_directories(vel_rt_continuation_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME ray_tracing_fallback_continuation_analysis COMMAND vel_rt_continuation_tests)
set_tests_properties(ray_tracing_fallback_continuation_analysis PROPERTIES LABELS "offline;compiler")

foreach(component frame_emitter state_compiler rt_request typed_pipeline typed_bda typed_runtime)
    add_executable(vel_rt_${component}_tests tests/${component}_tests.cpp)
    target_link_libraries(vel_rt_${component}_tests PRIVATE vkrt_typed_compiler Vulkan::Vulkan)
    target_include_directories(vel_rt_${component}_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
foreach(pair
    "frame_emitter;typed_frame_execution"
    "state_compiler;typed_source_cfg"
    "rt_request;typed_rt_requests"
    "typed_pipeline;typed_pipeline"
    "typed_bda;typed_bda"
    "typed_runtime;typed_runtime")
    list(GET pair 0 component)
    list(GET pair 1 test)
    add_test(NAME ray_tracing_fallback_${test} COMMAND vel_rt_${component}_tests)
    set_tests_properties(ray_tracing_fallback_${test} PROPERTIES
        LABELS "gpu;compiler;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
add_test(NAME ray_tracing_fallback_pinned_sources
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/docs/upstream/verify.py")
set_tests_properties(ray_tracing_fallback_pinned_sources PROPERTIES LABELS "offline;compiler")
