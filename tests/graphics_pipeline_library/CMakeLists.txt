# Copyright (c) 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0

# These tests include the implementation to exercise cache keys and a mocked dispatch chain.
# They use the same shared headers as the layer, without a Vulkan loader or GPU dependency.
foreach(test_name dynamic_state compatibility)
    set(test_target graphics_pipeline_library_${test_name}_tests)
    add_executable(${test_target} ${test_name}_tests.cpp)
    lunarg_target_compiler_configurations(${test_target} ${BUILD_WERROR})
    target_include_directories(${test_target} PRIVATE ${PROJECT_SOURCE_DIR}/layers)
    target_link_libraries(${test_target} PRIVATE VkExtLayer_utils Threads::Threads)
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
