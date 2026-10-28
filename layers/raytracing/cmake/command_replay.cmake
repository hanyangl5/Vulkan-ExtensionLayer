# SPDX-License-Identifier: MIT
# Typed command capture must match the selected SDK header and registry.
set(VKRT_REPLAY_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/vkrt_replay_generated.inc)
set(VKRT_REPLAY_COVERAGE ${CMAKE_CURRENT_BINARY_DIR}/vkrt_replay_coverage.json)
add_custom_command(OUTPUT ${VKRT_REPLAY_GENERATED} ${VKRT_REPLAY_COVERAGE}
   COMMAND ${Python3_EXECUTABLE} -X utf8 ${CMAKE_CURRENT_LIST_DIR}/generate_command_replay.py
      --registry ${VKRT_VULKAN_REGISTRY}
      --header ${VKRT_VULKAN_CORE_INCLUDE_DIR}/vulkan/vulkan_core.h
      --output ${VKRT_REPLAY_GENERATED} --coverage ${VKRT_REPLAY_COVERAGE}
   DEPENDS ${CMAKE_CURRENT_LIST_DIR}/generate_command_replay.py
      ${VKRT_VULKAN_REGISTRY} ${VKRT_VULKAN_CORE_INCLUDE_DIR}/vulkan/vulkan_core.h
   VERBATIM)
add_library(vkrt_command_replay STATIC src/vkrt_replay.c ${VKRT_REPLAY_GENERATED})
target_include_directories(vkrt_command_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${Vulkan_INCLUDE_DIRS}
   PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(vkrt_command_replay PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
