# SPDX-License-Identifier: MIT
#
# VkLayer_VKRT_SOFTWARE_RAY_QUERY
#
# Standalone Vulkan layer; also available through VEL's opt-in build.

cmake_minimum_required(VERSION 3.22.1)

project(vkrt_layer LANGUAGES C CXX)

# The SDK's SPIRV-Tools import libs are release-CRT. Building without a
# CMAKE_BUILD_TYPE mixes CRTs and the resulting binary dies before main
# with no diagnostic, so pin one.
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_path(VKRT_SPIRV_HEADERS_INCLUDE_DIR spirv/unified1/spirv.hpp
   HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include" REQUIRED)

# Prefer package targets; SDK import libraries remain a supported fallback.
foreach(_vkrt_component IN ITEMS SPIRV-Tools-link SPIRV-Tools-opt SPIRV-Tools)
   find_package(${_vkrt_component} CONFIG QUIET HINTS "$ENV{VULKAN_SDK}")
   if(TARGET ${_vkrt_component})
      list(APPEND VKRT_SPIRV_LIBRARIES ${_vkrt_component})
   elseif(TARGET ${_vkrt_component}-static)
      list(APPEND VKRT_SPIRV_LIBRARIES ${_vkrt_component}-static)
   else()
      find_library(VKRT_${_vkrt_component}_LIBRARY NAMES ${_vkrt_component}
         HINTS "$ENV{VULKAN_SDK}/Lib" "$ENV{VULKAN_SDK}/lib" REQUIRED)
      list(APPEND VKRT_SPIRV_LIBRARIES "${VKRT_${_vkrt_component}_LIBRARY}")
   endif()
endforeach()
include(${CMAKE_CURRENT_SOURCE_DIR}/pnext_sizes.cmake)

set(VKRT_LAYER_NAME VkLayer_VKRT_SOFTWARE_RAY_QUERY)
set(VKRT_COMMON_BVH4 ${CMAKE_CURRENT_SOURCE_DIR}/../../layers/common/rt_fallback)

add_library(${VKRT_LAYER_NAME} SHARED
   src/vkrt_layer.c
   src/vkrt_spirv.c
   src/vkrt_spirv_lower.cpp
   src/vkrt_spirv_ids.cpp
   src/vkrt_virtual_as.c
   src/vkrt_build.c
   src/vkrt_features.c
   src/vkrt_discovery.c
   src/vkrt_translate.c
   src/vkrt_commands.c
   src/vkrt_sync.c
)

# Traversal function library: compiled from GLSL to SPIR-V and embedded
# into shader modules at vkCreateShaderModule time.
find_program(VKRT_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "${Vulkan_INCLUDE_DIRS}/../Bin")
if(NOT VKRT_GLSLC)
   find_program(VKRT_GLSLC glslc REQUIRED)
endif()

set(VKRT_TRAVERSAL_SPV ${CMAKE_CURRENT_BINARY_DIR}/sw_ray_query.spv)
find_program(VKRT_SPIRV_VAL spirv-val HINTS "${Vulkan_INCLUDE_DIRS}/../Bin" REQUIRED)
add_custom_command(
   OUTPUT ${VKRT_TRAVERSAL_SPV} ${VKRT_TRAVERSAL_SPV}.h
   COMMAND ${VKRT_GLSLC}
           -fshader-stage=compute
           --target-env=vulkan1.2
           -I ${CMAKE_CURRENT_SOURCE_DIR}/bvh
           -I ${CMAKE_CURRENT_SOURCE_DIR}/traversal -I ${VKRT_COMMON_BVH4}
           ${CMAKE_CURRENT_SOURCE_DIR}/traversal/sw_ray_query_library.comp
           -o ${VKRT_TRAVERSAL_SPV}
   COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 --scalar-block-layout ${VKRT_TRAVERSAL_SPV}
   COMMAND ${CMAKE_COMMAND} -DINPUT=${VKRT_TRAVERSAL_SPV}
           -DOUTPUT=${VKRT_TRAVERSAL_SPV}.h -DNAME=traversal
           -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake
   DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/traversal/sw_ray_query_library.comp
      ${CMAKE_CURRENT_SOURCE_DIR}/traversal/sw_ray_query.glsl
      ${VKRT_COMMON_BVH4}/bvh4_abi.glsl
      ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vk_bvh_defines.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_contract.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_builder.h
      ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_build_layout.h
      ${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake
   COMMENT "Compiling sw_ray_query traversal library to SPIR-V"
   VERBATIM
)
# NOTE: deliberately NOT passing -O to glslc. spirv-opt strips OpName,
# and vkrt_spirv_lower.cpp resolves the sw_* entry points by name after
# spvtools::Link. Do not re-add -O without switching the lookup to
# LinkageAttributes or signature matching.
add_custom_target(vkrt_traversal_spv ALL DEPENDS ${VKRT_TRAVERSAL_SPV} ${VKRT_TRAVERSAL_SPV}.h)
add_dependencies(${VKRT_LAYER_NAME} vkrt_traversal_spv)

# GPU BVH builder.  These are the standalone-compatible subset of Mesa's
# builder used by the initial LBVH path.  Keep the shader names stable: M2's
# runtime pipeline cache loads the staged files by these names.
set(VKRT_BVH_SHADER_SOURCES
   bvh/vkrt_init.comp
   bvh/leaf.comp
   bvh/leaf_bvh4.comp
   bvh/vkrt_bvh4_prepare.comp
   ../../layers/common/rt_fallback/bvh4_mesa_encode.comp
   bvh/id_prefix_sum.comp
   bvh/morton_sort.comp
   bvh/lbvh_main.comp
   bvh/lbvh_generate_ir.comp
   bvh/pair_triangles.comp
   bvh/ploc_internal.comp
)
set(VKRT_BVH_SPV)
find_program(VKRT_SPIRV_VAL spirv-val HINTS "${Vulkan_INCLUDE_DIRS}/../Bin" REQUIRED)
target_include_directories(${VKRT_LAYER_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/bvh)
foreach(_vkrt_bvh_src IN LISTS VKRT_BVH_SHADER_SOURCES)
   get_filename_component(_vkrt_bvh_name "${_vkrt_bvh_src}" NAME_WE)
   set(_vkrt_bvh_spv "${CMAKE_CURRENT_BINARY_DIR}/bvh/${_vkrt_bvh_name}.spv")
   add_custom_command(
      OUTPUT ${_vkrt_bvh_spv} ${_vkrt_bvh_spv}.h
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bvh
      COMMAND ${VKRT_GLSLC}
              -fshader-stage=compute
              --target-env=vulkan1.2
              -I ${CMAKE_CURRENT_SOURCE_DIR}/bvh
              ${CMAKE_CURRENT_SOURCE_DIR}/${_vkrt_bvh_src}
              -o ${_vkrt_bvh_spv}
      COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 --scalar-block-layout ${_vkrt_bvh_spv}
      COMMAND ${CMAKE_COMMAND} -DINPUT=${_vkrt_bvh_spv}
              -DOUTPUT=${_vkrt_bvh_spv}.h -DNAME=${_vkrt_bvh_name}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake
      DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/${_vkrt_bvh_src}
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vkrt_bvh_preamble.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vk_bvh_defines.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_contract.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_builder.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_build_layout.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vk_bvh_helpers.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/leaf.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/pair_triangles.h
         ${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake
      COMMENT "Compiling GPU BVH builder ${_vkrt_bvh_name}"
      VERBATIM
   )
   list(APPEND VKRT_BVH_SPV ${_vkrt_bvh_spv})
   list(APPEND VKRT_BVH_HEADERS ${_vkrt_bvh_spv}.h)
endforeach()
add_custom_target(vkrt_bvh_spv ALL DEPENDS ${VKRT_BVH_SPV} ${VKRT_BVH_HEADERS})
add_dependencies(${VKRT_LAYER_NAME} vkrt_bvh_spv)

# Ship the SPIR-V next to the DLL so the layer can load it at runtime.
add_custom_command(TARGET ${VKRT_LAYER_NAME} POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${VKRT_TRAVERSAL_SPV} $<TARGET_FILE_DIR:${VKRT_LAYER_NAME}>/sw_ray_query.spv
   COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${VKRT_LAYER_NAME}>/bvh
   COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${VKRT_BVH_SPV} $<TARGET_FILE_DIR:${VKRT_LAYER_NAME}>/bvh/
   COMMENT "Staging sw_ray_query.spv beside the layer DLL"
)

target_compile_definitions(${VKRT_LAYER_NAME} PRIVATE
   VKRT_TRAVERSAL_SPV_NAME="sw_ray_query.spv"
)

target_link_libraries(${VKRT_LAYER_NAME} PRIVATE
   ${VKRT_SPIRV_LIBRARIES} Threads::Threads
)

if(WIN32)
   # VKAPI_ATTR is empty on Windows, so the negotiate entry point is not
   # auto-exported. A .def file is the clean way to export exactly that
   # one symbol without the dllexport-redeclaration warning. CMake passes
   # a .def in target_sources straight to the linker (MSVC and lld-link).
   target_sources(${VKRT_LAYER_NAME} PRIVATE vkrt_layer.def)
endif()

target_include_directories(${VKRT_LAYER_NAME}
   PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${VKRT_COMMON_BVH4}
      ${CMAKE_CURRENT_BINARY_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/bvh
      ${Vulkan_INCLUDE_DIRS}
      ${Vulkan_INCLUDE_DIRS}/spirv-tools
      ${VKRT_SPIRV_HEADERS_INCLUDE_DIR}
)

target_compile_definitions(${VKRT_LAYER_NAME}
   PRIVATE
      VKRT_DEBUG
      _CRT_SECURE_NO_WARNINGS
)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# Export only the negotiate entry point (via vkrt_layer.def on Windows).
if(MSVC)
   target_compile_options(${VKRT_LAYER_NAME} PRIVATE /W3 /wd4100)
else()
   # -Wno-unused-parameter: Vulkan entry-point stubs must carry full
   # signatures even when they only log and return.
   target_compile_options(${VKRT_LAYER_NAME} PRIVATE
      -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
   # GNU ld only; lld-link on Windows rejects it.
   if(NOT WIN32 AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_link_options(${VKRT_LAYER_NAME} PRIVATE -Wl,-Bsymbolic-functions)
   endif()
endif()

target_link_libraries(${VKRT_LAYER_NAME} PRIVATE ${Vulkan_LIBRARIES})

set_target_properties(${VKRT_LAYER_NAME} PROPERTIES
   C_STANDARD 11
   C_STANDARD_REQUIRED ON
   CXX_STANDARD 17
   CXX_STANDARD_REQUIRED ON
   # Keep the DLL name exactly what the manifest names.
   OUTPUT_NAME ${VKRT_LAYER_NAME}
   PREFIX ""
   WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

# The lowerer is C++ (spvtools::Link); let CMake enable the language when
# it sees the .cpp rather than forcing the whole project to CXX.
set_source_files_properties(src/vkrt_spirv_lower.cpp PROPERTIES LANGUAGE CXX)

# ------------------------------------------------------------------
# Manifests
# ------------------------------------------------------------------
# Use the actual target path, including the platform filename and build
# configuration, rather than guessing a library prefix or output directory.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_VKRT_SOFTWARE_RAY_QUERY.json.in _vkrt_manifest_template)
set(library_path "$<TARGET_FILE:${VKRT_LAYER_NAME}>")
string(CONFIGURE "${_vkrt_manifest_template}" _vkrt_manifest @ONLY)
file(GENERATE
   OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VkLayer_VKRT_SOFTWARE_RAY_QUERY.json"
   CONTENT "${_vkrt_manifest}")

# Stage the DLL + bare-name manifest into <build>/layers so the enablement
# story is a single VK_LAYER_PATH.
add_custom_command(TARGET ${VKRT_LAYER_NAME} POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/layers
   COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:${VKRT_LAYER_NAME}> ${CMAKE_BINARY_DIR}/layers/
   COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VkLayer_VKRT_SOFTWARE_RAY_QUERY.json
      ${CMAKE_BINARY_DIR}/layers/VkLayer_VKRT_SOFTWARE_RAY_QUERY.json
   COMMENT "Staging VKRT layer into ${CMAKE_BINARY_DIR}/layers"
)

message(STATUS "VKRT software ray-query layer: ${VKRT_LAYER_NAME}")
message(STATUS "  enable with: VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY")
message(STATUS "               VK_LAYER_PATH=<build>/layers")

# ------------------------------------------------------------------
# CPU debug BVHIR builder (used by the tests, not by the layer)
# ------------------------------------------------------------------
if(NOT TARGET vel_rt_bvh4_reference)
   add_library(vel_rt_bvh4_reference STATIC ${VKRT_COMMON_BVH4}/bvh4_reference.cpp ${VKRT_COMMON_BVH4}/mesa_ir_adapter.cpp)
   target_include_directories(vel_rt_bvh4_reference PUBLIC ${VKRT_COMMON_BVH4})
   set_target_properties(vel_rt_bvh4_reference PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
endif()
add_library(vkrt_cpu_bvh STATIC src/vkrt_cpu_bvh.c src/vkrt_cpu_bvh_bridge.cpp)
target_link_libraries(vkrt_cpu_bvh PUBLIC vel_rt_bvh4_reference)
set_target_properties(vkrt_cpu_bvh PROPERTIES CXX_STANDARD 17)
target_include_directories(vkrt_cpu_bvh PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/src
   ${Vulkan_INCLUDE_DIRS}
)
set_target_properties(vkrt_cpu_bvh PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------
# Host-side test of the SPIR-V lowerer (no GPU needed)
# ------------------------------------------------------------------
add_executable(test_spirv_lower
   ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_spirv_lower.cpp
   src/vkrt_spirv_lower.cpp
   src/vkrt_spirv_ids.cpp
)
target_include_directories(test_spirv_lower PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/src
   ${Vulkan_INCLUDE_DIRS}
   ${Vulkan_INCLUDE_DIRS}/spirv-tools
   ${VKRT_SPIRV_HEADERS_INCLUDE_DIR}
)
target_link_libraries(test_spirv_lower PRIVATE
   ${VKRT_SPIRV_LIBRARIES}
)
set_target_properties(test_spirv_lower PROPERTIES CXX_STANDARD 17)


# ------------------------------------------------------------------
# V0: single-triangle ray-query differential test (needs a GPU)
# ------------------------------------------------------------------
add_executable(test_single_tri
   ${CMAKE_CURRENT_SOURCE_DIR}/../tests/00_single_tri/main.c
)
target_link_libraries(test_single_tri PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
target_include_directories(test_single_tri PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/src
   ${Vulkan_INCLUDE_DIRS}
)
set_target_properties(test_single_tri PROPERTIES C_STANDARD 11)

# Compile the test shader as a real build output. A POST_BUILD command only
# runs when the executable relinks, so an incremental build could otherwise
# leave a test binary without rq_test.comp.spv.
set(VKRT_TEST_RQ_SPV ${CMAKE_CURRENT_BINARY_DIR}/rq_test.comp.spv)
add_custom_command(
   OUTPUT ${VKRT_TEST_RQ_SPV}
   COMMAND ${VKRT_GLSLC}
           -fshader-stage=compute --target-env=vulkan1.2
           ${CMAKE_CURRENT_SOURCE_DIR}/../tests/00_single_tri/rq_test.comp
           -o ${VKRT_TEST_RQ_SPV}
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tests/00_single_tri/rq_test.comp
   COMMENT "Compiling rq_test.comp"
   VERBATIM
)
add_custom_target(vkrt_test_rq_spv DEPENDS ${VKRT_TEST_RQ_SPV})
add_dependencies(test_single_tri vkrt_test_rq_spv)

add_executable(test_gpu_build ../tests/test_gpu_build.c)
target_link_libraries(test_gpu_build PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
set_target_properties(test_gpu_build PROPERTIES C_STANDARD 11)
add_dependencies(test_gpu_build vkrt_test_rq_spv)

set(VKRT_TEST_GETTER_SPV ${CMAKE_CURRENT_BINARY_DIR}/rq_sbt_offset.spv)
add_custom_command(
   OUTPUT ${VKRT_TEST_GETTER_SPV}
   COMMAND ${VKRT_GLSLC} -fshader-stage=compute --target-env=vulkan1.2
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/rq_sbt_offset.comp -o ${VKRT_TEST_GETTER_SPV}
   COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${VKRT_TEST_GETTER_SPV}
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tests/rq_sbt_offset.comp
   COMMENT "Compiling baseline ray-query SBT-offset getter fixture"
   VERBATIM
)
add_custom_target(vkrt_test_getter_spv DEPENDS ${VKRT_TEST_GETTER_SPV})
add_dependencies(test_spirv_lower vkrt_traversal_spv vkrt_test_rq_spv vkrt_test_getter_spv)

include(CTest)
if(BUILD_TESTING)
   # Manual capability probe; its observed address/alignment is device-specific.
   add_executable(probe_bda_alignment ../tests/probe_bda_alignment.c)
   target_include_directories(probe_bda_alignment PRIVATE ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(probe_bda_alignment PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(probe_bda_alignment PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   set(VKRT_CLASSIFICATION_SPV ${CMAKE_CURRENT_BINARY_DIR}/traversal_classification.spv)
   add_custom_command(
      OUTPUT ${VKRT_CLASSIFICATION_SPV}
      COMMAND ${VKRT_GLSLC} -fshader-stage=compute --target-env=vulkan1.2
         -I ${CMAKE_CURRENT_SOURCE_DIR}/bvh -I ${CMAKE_CURRENT_SOURCE_DIR}/traversal -I ${VKRT_COMMON_BVH4}
         ${CMAKE_CURRENT_SOURCE_DIR}/../tests/traversal_classification.comp -o ${VKRT_CLASSIFICATION_SPV}
      COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 --scalar-block-layout ${VKRT_CLASSIFICATION_SPV}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tests/traversal_classification.comp
         ${CMAKE_CURRENT_SOURCE_DIR}/traversal/sw_ray_query.glsl
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vk_bvh_defines.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_contract.h
      COMMENT "Compiling production traversal helper semantics test"
      VERBATIM
   )
   add_custom_target(vkrt_classification_spv DEPENDS ${VKRT_CLASSIFICATION_SPV})
   add_executable(test_traversal_classification ../tests/test_traversal_classification.c)
   target_include_directories(test_traversal_classification PRIVATE ${Vulkan_INCLUDE_DIRS} ${VKRT_COMMON_BVH4})
   target_link_libraries(test_traversal_classification PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(test_traversal_classification PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_traversal_classification vkrt_classification_spv)
   add_executable(test_host_translation ../tests/test_host_translation.c src/vkrt_translate.c src/vkrt_sync.c)
   target_include_directories(test_host_translation PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_host_translation PROPERTIES C_STANDARD 11)
   add_executable(test_device_features ../tests/test_device_features.c src/vkrt_features.c)
   target_include_directories(test_device_features PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_device_features PROPERTIES C_STANDARD 11)
   add_test(NAME vkrt.device_features COMMAND test_device_features)
   set_tests_properties(vkrt.device_features PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_discovery ../tests/test_discovery.c src/vkrt_discovery.c)
   target_include_directories(test_discovery PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_discovery PROPERTIES C_STANDARD 11)
   add_test(NAME vkrt.discovery COMMAND test_discovery)
   set_tests_properties(vkrt.discovery PROPERTIES LABELS "offline;no_native_rt")
   # Build the same fixture as two translation units to catch per-TU once state.
   add_library(test_registry_worker OBJECT ../tests/test_registry_concurrency.c)
   target_compile_definitions(test_registry_worker PRIVATE VKRT_REGISTRY_WORKER_TU=1)
   target_include_directories(test_registry_worker PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_registry_worker PRIVATE Threads::Threads)
   set_target_properties(test_registry_worker PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_executable(test_registry_concurrency ../tests/test_registry_concurrency.c $<TARGET_OBJECTS:test_registry_worker>)
   target_include_directories(test_registry_concurrency PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_registry_concurrency PRIVATE Threads::Threads)
   set_target_properties(test_registry_concurrency PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.registry_concurrency COMMAND test_registry_concurrency)
   set_tests_properties(vkrt.registry_concurrency PROPERTIES LABELS "offline;concurrency" TIMEOUT 60)
   add_executable(test_spirv_ids ../tests/test_spirv_ids.cpp src/vkrt_spirv_ids.cpp)
   target_include_directories(test_spirv_ids PRIVATE src ${Vulkan_INCLUDE_DIRS} ${VKRT_SPIRV_HEADERS_INCLUDE_DIR})
   target_link_libraries(test_spirv_ids PRIVATE ${VKRT_SPIRV_LIBRARIES})
   set_target_properties(test_spirv_ids PROPERTIES CXX_STANDARD 17)
   add_executable(test_ir_builder_contract ../tests/test_ir_builder_contract.c)
   target_include_directories(test_ir_builder_contract PRIVATE bvh ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_ir_builder_contract PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.ir_builder_contract COMMAND test_ir_builder_contract)
   set_tests_properties(vkrt.ir_builder_contract PROPERTIES LABELS "offline")
   add_executable(test_cpu_bvh4 ../tests/test_cpu_bvh4.c)
   target_link_libraries(test_cpu_bvh4 PRIVATE vkrt_cpu_bvh)
   set_target_properties(test_cpu_bvh4 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.cpu_bvh4 COMMAND test_cpu_bvh4)
   set_tests_properties(vkrt.cpu_bvh4 PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_bvh4_build_layout ../tests/test_bvh4_build_layout.cpp)
   target_include_directories(test_bvh4_build_layout PRIVATE src bvh ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_bvh4_build_layout PRIVATE vel_rt_bvh4_reference)
   set_target_properties(test_bvh4_build_layout PROPERTIES CXX_STANDARD 17)
   add_test(NAME vkrt.bvh4_build_layout COMMAND test_bvh4_build_layout)
   set_tests_properties(vkrt.bvh4_build_layout PROPERTIES LABELS "offline;no_native_rt")
   add_test(NAME vkrt.host_translation COMMAND test_host_translation)
   add_executable(test_as_address_cells ../tests/test_as_address_cells.c src/vkrt_virtual_as.c src/vkrt_translate.c)
   target_include_directories(test_as_address_cells PRIVATE src bvh ${VKRT_COMMON_BVH4} ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_as_address_cells PRIVATE Threads::Threads)
   set_target_properties(test_as_address_cells PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.as_address_cells COMMAND test_as_address_cells)
   set_tests_properties(vkrt.as_address_cells PROPERTIES LABELS "offline;concurrency" TIMEOUT 60)
   add_test(NAME vkrt.spirv_ids COMMAND test_spirv_ids)
   set_tests_properties(vkrt.host_translation vkrt.spirv_ids PROPERTIES LABELS "offline")
   add_test(NAME vkrt.spirv_lower COMMAND test_spirv_lower
      ${VKRT_TEST_RQ_SPV} ${VKRT_TRAVERSAL_SPV} ${CMAKE_CURRENT_BINARY_DIR}/ctest_lowered.spv)
   add_test(NAME vkrt.spirv_lower.sbt_offset COMMAND test_spirv_lower
      ${VKRT_TEST_GETTER_SPV} ${VKRT_TRAVERSAL_SPV} ${CMAKE_CURRENT_BINARY_DIR}/ctest_sbt_offset_lowered.spv
      --require-sbt-offset)
   set_tests_properties(vkrt.spirv_lower.sbt_offset PROPERTIES LABELS "offline")
   add_test(NAME vkrt.traversal_classification COMMAND test_traversal_classification ${VKRT_CLASSIFICATION_SPV})
   set_tests_properties(vkrt.traversal_classification PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;software_compute;no_native_rt" TIMEOUT 120)
   add_test(NAME vkrt.cpu_fixture COMMAND test_single_tri)
   add_test(NAME vkrt.gpu_build.software COMMAND test_gpu_build)
   add_test(NAME vkrt.gpu_build.native COMMAND test_gpu_build)
   set_tests_properties(vkrt.cpu_fixture vkrt.gpu_build.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu" TIMEOUT 600)
   set_tests_properties(vkrt.gpu_build.native PROPERTIES ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" TIMEOUT 600)
   set_tests_properties(vkrt.spirv_lower PROPERTIES LABELS "offline")
endif()
