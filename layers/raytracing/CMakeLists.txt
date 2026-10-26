# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22.1)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(VelOpacityMicromap LANGUAGES CXX)
    enable_testing()
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# The native OMM adapter deliberately has no software RT dependency.
add_subdirectory(omm)
