# SPDX-License-Identifier: MIT
#
# Unified software acceleration structures, Ray Query and RT pipelines.
#
# Standalone Vulkan layer; also available through VEL's opt-in build.

cmake_minimum_required(VERSION 3.22.1)

project(VELRayTracingFallback LANGUAGES C CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
   message(FATAL_ERROR "The experimental RT fallback currently requires a 64-bit build")
endif()
if(WIN32)
   # The parent VEL build enables Win32 Vulkan declarations for every compiler.
   # Keep Windows macros out of the typed compiler's standard C++ interfaces.
   add_compile_definitions(NOMINMAX)
endif()

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
set(VKRT_COMMON_BVH4 ${CMAKE_CURRENT_SOURCE_DIR}/bvh4)

add_library(${VKRT_LAYER_NAME} SHARED
   src/vkrt_layer.c
   src/vkrt_spirv.c
   src/vkrt_spirv_lower.cpp
   src/vkrt_spirv_ids.cpp
   src/vkrt_virtual_as.c
   src/vkrt_as_registry.c
   src/vkrt_as_copy.c
   src/vkrt_as_serialization.c
   src/vkrt_rtp.cpp
   src/vkrt_resource_profile.c
   src/vkrt_descriptor_layout.c
   src/vkrt_memory_queries.c
   src/vkrt_query_pool.c
   src/vkrt_shader_objects.c
   src/vkrt_deferred.c
   src/vkrt_build.c
   src/vkrt_features.c
   src/vkrt_discovery.c
   src/vkrt_translate.c
   src/vkrt_commands.c
   src/vkrt_sync.c
)

# The canonical AS/RQ/RTP layer uses one typed SPIR-V
# compiler. This target has no SPIRV-Cross/GLSL regex or legacy builder runtime.
set(VKRT_TYPED_SOURCE ${CMAKE_CURRENT_SOURCE_DIR})
add_library(vkrt_typed_compiler STATIC
   ${VKRT_TYPED_SOURCE}/continuations.cpp ${VKRT_TYPED_SOURCE}/frame_emitter.cpp
   ${VKRT_TYPED_SOURCE}/state_compiler.cpp ${VKRT_TYPED_SOURCE}/typed_pipeline.cpp
   ${VKRT_TYPED_SOURCE}/typed_runtime.cpp ${VKRT_TYPED_SOURCE}/glsl_compiler.cpp)
include(${VKRT_TYPED_SOURCE}/typed_runtime_sources.cmake)
target_include_directories(vkrt_typed_compiler PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
   find_path(VKRT_GLSLANG_INCLUDE_DIR glslang/Public/ShaderLang.h HINTS ${Vulkan_INCLUDE_DIRS} REQUIRED)
   foreach(_vkrt_glslang IN ITEMS SPIRV glslang glslang-default-resource-limits)
      find_library(VKRT_${_vkrt_glslang}_LIBRARY NAMES ${_vkrt_glslang}
         HINTS "$ENV{VULKAN_SDK}/Lib" "${Vulkan_INCLUDE_DIRS}/../Lib" REQUIRED)
      list(APPEND VKRT_GLSLANG_LIBRARIES ${VKRT_${_vkrt_glslang}_LIBRARY})
   endforeach()
   target_include_directories(vkrt_typed_compiler PRIVATE ${VKRT_GLSLANG_INCLUDE_DIR})
   target_link_libraries(vkrt_typed_compiler PUBLIC ${VKRT_GLSLANG_LIBRARIES})
else()
   find_package(glslang CONFIG REQUIRED)
   target_link_libraries(vkrt_typed_compiler PUBLIC glslang::SPIRV glslang::glslang glslang::glslang-default-resource-limits)
endif()
target_include_directories(vkrt_typed_compiler PUBLIC ${VKRT_TYPED_SOURCE} ${Vulkan_INCLUDE_DIRS} ${VKRT_SPIRV_HEADERS_INCLUDE_DIR})
target_link_libraries(vkrt_typed_compiler PUBLIC ${VKRT_SPIRV_LIBRARIES})
target_compile_features(vkrt_typed_compiler PUBLIC cxx_std_17)
set_target_properties(vkrt_typed_compiler PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(${VKRT_LAYER_NAME} PRIVATE vkrt_typed_compiler)

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
      ${VKRT_COMMON_BVH4}/as_address.glsl
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
   bvh/vkrt_bvh4_copy.comp
   bvh/vkrt_bvh4_serialization.comp
   bvh/vkrt_as_query.comp
   bvh/vkrt_query_format.comp
   bvh/vkrt_query_publish.comp
   bvh4/bvh4_mesa_encode.comp
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
              -I ${VKRT_COMMON_BVH4}
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
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vkrt_as_serialization.glsl
         ${VKRT_COMMON_BVH4}/as_address.glsl
         ${VKRT_COMMON_BVH4}/bvh4_abi.glsl
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
   ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spirv_lower.cpp
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
   ${CMAKE_CURRENT_SOURCE_DIR}/tests/00_single_tri/main.c
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
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/00_single_tri/rq_test.comp
           -o ${VKRT_TEST_RQ_SPV}
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/00_single_tri/rq_test.comp
   COMMENT "Compiling rq_test.comp"
   VERBATIM
)
add_custom_target(vkrt_test_rq_spv DEPENDS ${VKRT_TEST_RQ_SPV})
add_dependencies(test_single_tri vkrt_test_rq_spv)

add_executable(test_gpu_build tests/test_gpu_build.c)
target_link_libraries(test_gpu_build PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
set_target_properties(test_gpu_build PROPERTIES C_STANDARD 11)
add_dependencies(test_gpu_build vkrt_test_rq_spv)
add_executable(test_query_pool_gpu tests/test_query_pool_gpu.c)
target_link_libraries(test_query_pool_gpu PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES} Threads::Threads)
set_target_properties(test_query_pool_gpu PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
set(VKRT_SHADER_OBJECT_RQ_SPV ${CMAKE_CURRENT_BINARY_DIR}/shader_object_rq.comp.spv)
add_custom_command(OUTPUT ${VKRT_SHADER_OBJECT_RQ_SPV}
   COMMAND ${VKRT_GLSLC} -fshader-stage=compute --target-env=vulkan1.2
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_object_rq.comp -o ${VKRT_SHADER_OBJECT_RQ_SPV}
   COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${VKRT_SHADER_OBJECT_RQ_SPV}
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_object_rq.comp VERBATIM)
add_custom_target(vkrt_shader_object_rq_spv DEPENDS ${VKRT_SHADER_OBJECT_RQ_SPV})
add_executable(test_shader_object_rq tests/test_shader_object_rq.c)
target_link_libraries(test_shader_object_rq PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
set_target_properties(test_shader_object_rq PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_shader_object_rq vkrt_shader_object_rq_spv)
set(VKRT_SHADER_BINARY_PROFILE_SPV)
foreach(profile rq plain)
   set(output ${CMAKE_CURRENT_BINARY_DIR}/shader_binary_profile_${profile}.comp.spv)
   add_custom_command(OUTPUT ${output}
      COMMAND ${VKRT_GLSLC} --target-env=vulkan1.2 ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_binary_profile_${profile}.comp -o ${output}
      COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${output}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_binary_profile_${profile}.comp VERBATIM)
   list(APPEND VKRT_SHADER_BINARY_PROFILE_SPV ${output})
endforeach()
foreach(stage vert frag)
   set(output ${CMAKE_CURRENT_BINARY_DIR}/shader_binary_linked.${stage}.spv)
   add_custom_command(OUTPUT ${output}
      COMMAND ${VKRT_GLSLC} --target-env=vulkan1.2 ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_binary_linked.${stage} -o ${output}
      COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${output}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_binary_linked.${stage} VERBATIM)
   list(APPEND VKRT_SHADER_BINARY_PROFILE_SPV ${output})
endforeach()
add_custom_target(vkrt_shader_binary_profile_spv DEPENDS ${VKRT_SHADER_BINARY_PROFILE_SPV})
add_executable(test_shader_binary_profiles tests/test_shader_binary_profiles.c)
target_link_libraries(test_shader_binary_profiles PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
set_target_properties(test_shader_binary_profiles PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_shader_binary_profiles vkrt_shader_binary_profile_spv)

add_executable(test_as_copy tests/test_as_copy.c)
target_link_libraries(test_as_copy PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
set_target_properties(test_as_copy PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_as_copy vkrt_test_rq_spv)
add_executable(test_as_update tests/test_as_update.c)
target_link_libraries(test_as_update PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
target_include_directories(test_as_update PRIVATE src ${VKRT_COMMON_BVH4})
set_target_properties(test_as_update PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_as_update vkrt_test_rq_spv)
add_executable(test_as_serialization tests/test_as_serialization.c)
target_link_libraries(test_as_serialization PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
target_include_directories(test_as_serialization PRIVATE src)
set_target_properties(test_as_serialization PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_as_serialization vkrt_test_rq_spv)
add_executable(test_query_properties tests/test_query_properties.c)
target_link_libraries(test_query_properties PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
target_include_directories(test_query_properties PRIVATE src bvh ${VKRT_COMMON_BVH4})
set_target_properties(test_query_properties PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(test_query_properties vkrt_test_rq_spv vkrt_bvh_spv)

set(VKRT_TEST_GETTER_SPV ${CMAKE_CURRENT_BINARY_DIR}/rq_sbt_offset.spv)
add_custom_command(
   OUTPUT ${VKRT_TEST_GETTER_SPV}
   COMMAND ${VKRT_GLSLC} -fshader-stage=compute --target-env=vulkan1.2
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/rq_sbt_offset.comp -o ${VKRT_TEST_GETTER_SPV}
   COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${VKRT_TEST_GETTER_SPV}
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/rq_sbt_offset.comp
   COMMENT "Compiling baseline ray-query SBT-offset getter fixture"
   VERBATIM
)
add_custom_target(vkrt_test_getter_spv DEPENDS ${VKRT_TEST_GETTER_SPV})
add_dependencies(test_spirv_lower vkrt_traversal_spv vkrt_test_rq_spv vkrt_test_getter_spv)

include(CTest)
if(BUILD_TESTING)
   set(VKRT_RTP_API_SPV)
   foreach(stage rgen rmiss rchit comp)
      set(output ${CMAKE_CURRENT_BINARY_DIR}/rtp_api.${stage}.spv)
      add_custom_command(
         OUTPUT ${output}
         COMMAND ${VKRT_GLSLC} --target-env=vulkan1.2
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rtp_api.${stage} -o ${output}
         COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${output}
         DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/rtp_api.${stage}
         COMMENT "Compiling public RTP API ${stage} fixture"
         VERBATIM)
      list(APPEND VKRT_RTP_API_SPV ${output})
   endforeach()
   add_custom_target(vkrt_rtp_api_spv DEPENDS ${VKRT_RTP_API_SPV})
   add_executable(test_rtp_api tests/test_rtp_api.c)
   target_include_directories(test_rtp_api PRIVATE src ${VKRT_COMMON_BVH4} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_rtp_api PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
   set_target_properties(test_rtp_api PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_rtp_api vkrt_rtp_api_spv)
   add_test(NAME vkrt.rtp_api.software COMMAND test_rtp_api ${VKRT_RTP_API_SPV})
   set_tests_properties(vkrt.rtp_api.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;typed_rtp" SKIP_RETURN_CODE 77 TIMEOUT 600)
   # Manual capability probe; its observed address/alignment is device-specific.
   add_executable(probe_bda_alignment tests/probe_bda_alignment.c)
   target_include_directories(probe_bda_alignment PRIVATE ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(probe_bda_alignment PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(probe_bda_alignment PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_executable(test_deferred_device tests/test_deferred_device.c)
   target_include_directories(test_deferred_device PRIVATE ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_deferred_device PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(test_deferred_device PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.deferred_device COMMAND test_deferred_device)
   set_tests_properties(vkrt.deferred_device PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=" LABELS "gpu;no_native_rt" TIMEOUT 60)
   set(VKRT_CLASSIFICATION_SPV ${CMAKE_CURRENT_BINARY_DIR}/traversal_classification.spv)
   add_custom_command(
      OUTPUT ${VKRT_CLASSIFICATION_SPV}
      COMMAND ${VKRT_GLSLC} -fshader-stage=compute --target-env=vulkan1.2
         -I ${CMAKE_CURRENT_SOURCE_DIR}/bvh -I ${CMAKE_CURRENT_SOURCE_DIR}/traversal -I ${VKRT_COMMON_BVH4}
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/traversal_classification.comp -o ${VKRT_CLASSIFICATION_SPV}
      COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 --scalar-block-layout ${VKRT_CLASSIFICATION_SPV}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/traversal_classification.comp
         ${CMAKE_CURRENT_SOURCE_DIR}/traversal/sw_ray_query.glsl
         ${VKRT_COMMON_BVH4}/bvh4_abi.glsl
         ${VKRT_COMMON_BVH4}/as_address.glsl
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/vk_bvh_defines.h
         ${CMAKE_CURRENT_SOURCE_DIR}/bvh/bvh_ir_contract.h
      COMMENT "Compiling production traversal helper semantics test"
      VERBATIM
   )
   add_custom_target(vkrt_classification_spv DEPENDS ${VKRT_CLASSIFICATION_SPV})
   add_executable(test_traversal_classification tests/test_traversal_classification.c)
   target_include_directories(test_traversal_classification PRIVATE ${Vulkan_INCLUDE_DIRS} ${VKRT_COMMON_BVH4})
   target_link_libraries(test_traversal_classification PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(test_traversal_classification PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_traversal_classification vkrt_classification_spv)
   add_executable(test_host_translation tests/test_host_translation.c src/vkrt_translate.c src/vkrt_sync.c)
   target_include_directories(test_host_translation PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_host_translation PROPERTIES C_STANDARD 11)
   add_executable(test_command_state tests/test_command_state.c src/vkrt_commands.c)
   target_include_directories(test_command_state PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_command_state PRIVATE Threads::Threads)
   set_target_properties(test_command_state PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.command_state COMMAND test_command_state)
   set_tests_properties(vkrt.command_state PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_memory_queries tests/test_memory_queries.c)
   target_include_directories(test_memory_queries PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_memory_queries PRIVATE Threads::Threads)
   set_target_properties(test_memory_queries PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.memory_queries COMMAND test_memory_queries)
   add_executable(test_query_pool_publication tests/test_query_pool_publication.c)
   target_include_directories(test_query_pool_publication PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/bvh ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_query_pool_publication PRIVATE Threads::Threads)
   set_target_properties(test_query_pool_publication PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_query_pool_publication vkrt_bvh_spv)
   add_test(NAME vkrt.query_pool_publication COMMAND test_query_pool_publication)
   set_tests_properties(vkrt.query_pool_publication PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_shader_objects tests/test_shader_objects.c src/vkrt_shader_objects.c)
   target_include_directories(test_shader_objects PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_shader_objects PRIVATE Threads::Threads)
   set_target_properties(test_shader_objects PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.shader_objects COMMAND test_shader_objects)
   set_tests_properties(vkrt.shader_objects PROPERTIES LABELS "offline;no_native_rt")
   add_test(NAME vkrt.query_pool_gpu.cross_queue.software COMMAND test_query_pool_gpu --cross-queue)
   set_tests_properties(vkrt.query_pool_gpu.cross_queue.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;query_publication" TIMEOUT 600 SKIP_RETURN_CODE 77)
   add_test(NAME vkrt.query_pool_gpu.cross_queue.native COMMAND test_query_pool_gpu --cross-queue)
   set_tests_properties(vkrt.query_pool_gpu.cross_queue.native PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS="
      LABELS "gpu;native_rt;query_publication" TIMEOUT 600 SKIP_RETURN_CODE 77)
   add_test(NAME vkrt.query_pool_gpu COMMAND test_query_pool_gpu)
   set_tests_properties(vkrt.query_pool_gpu PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;no_native_rt" TIMEOUT 120)
   add_test(NAME vkrt.shader_object_rq.software COMMAND test_shader_object_rq ${VKRT_SHADER_OBJECT_RQ_SPV})
   set_tests_properties(vkrt.shader_object_rq.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.shader_object_rq.native COMMAND test_shader_object_rq ${VKRT_SHADER_OBJECT_RQ_SPV})
   set_tests_properties(vkrt.shader_object_rq.native PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.shader_binary_profiles.software COMMAND test_shader_binary_profiles ${VKRT_SHADER_BINARY_PROFILE_SPV})
   set_tests_properties(vkrt.shader_binary_profiles.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.shader_binary_profiles.native COMMAND test_shader_binary_profiles ${VKRT_SHADER_BINARY_PROFILE_SPV})
   set_tests_properties(vkrt.shader_binary_profiles.native PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   set_tests_properties(vkrt.memory_queries PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_memory_queries_gpu tests/test_memory_queries_gpu.c)
   target_include_directories(test_memory_queries_gpu PRIVATE ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_memory_queries_gpu PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(test_memory_queries_gpu PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.memory_queries.gpu COMMAND test_memory_queries_gpu)
   set_tests_properties(vkrt.memory_queries.gpu PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu" SKIP_RETURN_CODE 77 TIMEOUT 120)
   add_executable(test_resource_profile tests/test_resource_profile.c src/vkrt_resource_profile.c)
   target_include_directories(test_resource_profile PRIVATE src ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_resource_profile PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.resource_profile COMMAND test_resource_profile)
   set_tests_properties(vkrt.resource_profile PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_descriptor_layout tests/test_descriptor_layout.c src/vkrt_descriptor_layout.c)
   target_include_directories(test_descriptor_layout PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_descriptor_layout PRIVATE Threads::Threads)
   set_target_properties(test_descriptor_layout PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.descriptor_layout COMMAND test_descriptor_layout)
   set_tests_properties(vkrt.descriptor_layout PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_resource_profile_gpu tests/test_resource_profile_gpu.c src/vkrt_resource_profile.c src/vkrt_descriptor_layout.c)
   target_include_directories(test_resource_profile_gpu PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_resource_profile_gpu PRIVATE Threads::Threads ${Vulkan_LIBRARIES})
   set_target_properties(test_resource_profile_gpu PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.resource_profile.gpu COMMAND test_resource_profile_gpu)
   set_tests_properties(vkrt.resource_profile.gpu PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
   set(VKRT_RESOURCE_BOUNDARY_SPV)
   foreach(stage rgen rmiss rchit)
      set(output ${CMAKE_CURRENT_BINARY_DIR}/resource_boundary.${stage}.spv)
      add_custom_command(OUTPUT ${output}
         COMMAND ${VKRT_GLSLC} --target-env=vulkan1.2
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_boundary.${stage} -o ${output}
         COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${output}
         DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_boundary.${stage}
         COMMENT "Compiling full resource boundary ${stage} fixture" VERBATIM)
      list(APPEND VKRT_RESOURCE_BOUNDARY_SPV ${output})
   endforeach()
   add_custom_target(vkrt_resource_boundary_spv DEPENDS ${VKRT_RESOURCE_BOUNDARY_SPV})
   add_executable(test_resource_boundary tests/test_resource_boundary.c src/vkrt_resource_profile.c)
   target_include_directories(test_resource_boundary PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_resource_boundary PRIVATE vkrt_cpu_bvh ${Vulkan_LIBRARIES})
   set_target_properties(test_resource_boundary PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_resource_boundary vkrt_resource_boundary_spv)
   add_test(NAME vkrt.resource_boundary.software COMMAND test_resource_boundary ${VKRT_RESOURCE_BOUNDARY_SPV})
   set_tests_properties(vkrt.resource_boundary.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;typed_rtp" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.resource_boundary.native COMMAND test_resource_boundary ${VKRT_RESOURCE_BOUNDARY_SPV})
   set_tests_properties(vkrt.resource_boundary.native PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.resource_boundary.dsl_lifetime.software COMMAND test_resource_boundary
      ${VKRT_RESOURCE_BOUNDARY_SPV} --destroy-dsl-before-pipeline)
   set_tests_properties(vkrt.resource_boundary.dsl_lifetime.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;typed_rtp" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.resource_boundary.dsl_lifetime.native COMMAND test_resource_boundary
      ${VKRT_RESOURCE_BOUNDARY_SPV} --destroy-dsl-before-pipeline)
   set_tests_properties(vkrt.resource_boundary.dsl_lifetime.native PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   set(VKRT_DSL_LIFETIME_SPV ${CMAKE_CURRENT_BINARY_DIR}/dsl_lifetime.comp.spv)
   add_custom_command(OUTPUT ${VKRT_DSL_LIFETIME_SPV}
      COMMAND ${VKRT_GLSLC} --target-env=vulkan1.2 ${CMAKE_CURRENT_SOURCE_DIR}/tests/dsl_lifetime.comp
         -o ${VKRT_DSL_LIFETIME_SPV}
      COMMAND ${VKRT_SPIRV_VAL} --target-env vulkan1.2 ${VKRT_DSL_LIFETIME_SPV}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/dsl_lifetime.comp VERBATIM)
   add_custom_target(vkrt_dsl_lifetime_spv DEPENDS ${VKRT_DSL_LIFETIME_SPV})
   add_executable(test_dsl_lifetime tests/test_dsl_lifetime.c)
   target_include_directories(test_dsl_lifetime PRIVATE ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_dsl_lifetime PRIVATE ${Vulkan_LIBRARIES})
   set_target_properties(test_dsl_lifetime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_dsl_lifetime vkrt_dsl_lifetime_spv)
   add_test(NAME vkrt.dsl_lifetime.native COMMAND test_dsl_lifetime ${VKRT_DSL_LIFETIME_SPV})
   add_test(NAME vkrt.dsl_lifetime.retained.native COMMAND test_dsl_lifetime ${VKRT_DSL_LIFETIME_SPV} --retain-dsl)
   set_tests_properties(vkrt.dsl_lifetime.native vkrt.dsl_lifetime.retained.native PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;no_native_rt" SKIP_RETURN_CODE 77 TIMEOUT 120)
   add_executable(test_rtp_bindings tests/test_rtp_bindings.cpp src/vkrt_rtp.cpp)
   target_include_directories(test_rtp_bindings PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS} ${VKRT_SPIRV_HEADERS_INCLUDE_DIR})
   target_link_libraries(test_rtp_bindings PRIVATE vkrt_typed_compiler Threads::Threads)
   target_compile_definitions(test_rtp_bindings PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
   set_target_properties(test_rtp_bindings PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.rtp_bindings COMMAND test_rtp_bindings)
   set_tests_properties(vkrt.rtp_bindings PROPERTIES LABELS "offline;no_native_rt")
   add_test(NAME vkrt.rtp_api.maintenance6 COMMAND test_rtp_api ${VKRT_RTP_API_SPV} --maintenance6)
   set_tests_properties(vkrt.rtp_api.maintenance6 PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;typed_rtp" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.rtp_api.shader_object COMMAND test_rtp_api ${VKRT_RTP_API_SPV} --shader-object)
   set_tests_properties(vkrt.rtp_api.shader_object PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;typed_rtp" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.rtp_api.derivatives.software COMMAND test_rtp_api ${VKRT_RTP_API_SPV} --derivatives)
   set_tests_properties(vkrt.rtp_api.derivatives.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu;typed_rtp" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_test(NAME vkrt.rtp_api.derivatives.native COMMAND test_rtp_api ${VKRT_RTP_API_SPV} --derivatives)
   set_tests_properties(vkrt.rtp_api.derivatives.native PROPERTIES
      ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;typed_rtp;native_rt" SKIP_RETURN_CODE 77 TIMEOUT 600)
   add_executable(test_device_features tests/test_device_features.c src/vkrt_features.c)
   target_include_directories(test_device_features PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_device_features PROPERTIES C_STANDARD 11)
   add_test(NAME vkrt.device_features COMMAND test_device_features)
   set_tests_properties(vkrt.device_features PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_discovery tests/test_discovery.c src/vkrt_discovery.c)
   target_include_directories(test_discovery PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_discovery PROPERTIES C_STANDARD 11)
   add_test(NAME vkrt.discovery COMMAND test_discovery)
   set_tests_properties(vkrt.discovery PROPERTIES LABELS "offline;no_native_rt")
   # Build the same fixture as two translation units to catch per-TU once state.
   add_library(test_registry_worker OBJECT tests/test_registry_concurrency.c)
   target_compile_definitions(test_registry_worker PRIVATE VKRT_REGISTRY_WORKER_TU=1)
   target_include_directories(test_registry_worker PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_registry_worker PRIVATE Threads::Threads)
   set_target_properties(test_registry_worker PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_executable(test_registry_concurrency tests/test_registry_concurrency.c $<TARGET_OBJECTS:test_registry_worker>)
   target_include_directories(test_registry_concurrency PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_registry_concurrency PRIVATE Threads::Threads)
   set_target_properties(test_registry_concurrency PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.registry_concurrency COMMAND test_registry_concurrency)
   set_tests_properties(vkrt.registry_concurrency PROPERTIES LABELS "offline;concurrency" TIMEOUT 60)
   add_executable(test_spirv_ids tests/test_spirv_ids.cpp src/vkrt_spirv_ids.cpp)
   target_include_directories(test_spirv_ids PRIVATE src ${Vulkan_INCLUDE_DIRS} ${VKRT_SPIRV_HEADERS_INCLUDE_DIR})
   target_link_libraries(test_spirv_ids PRIVATE ${VKRT_SPIRV_LIBRARIES})
   set_target_properties(test_spirv_ids PROPERTIES CXX_STANDARD 17)
   add_executable(test_ir_builder_contract tests/test_ir_builder_contract.c)
   target_include_directories(test_ir_builder_contract PRIVATE bvh ${Vulkan_INCLUDE_DIRS})
   set_target_properties(test_ir_builder_contract PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.ir_builder_contract COMMAND test_ir_builder_contract)
   set_tests_properties(vkrt.ir_builder_contract PROPERTIES LABELS "offline")
   add_executable(test_cpu_bvh4 tests/test_cpu_bvh4.c)
   target_link_libraries(test_cpu_bvh4 PRIVATE vkrt_cpu_bvh)
   set_target_properties(test_cpu_bvh4 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.cpu_bvh4 COMMAND test_cpu_bvh4)
   set_tests_properties(vkrt.cpu_bvh4 PROPERTIES LABELS "offline;no_native_rt")
   add_executable(test_bvh4_build_layout tests/test_bvh4_build_layout.cpp)
   target_include_directories(test_bvh4_build_layout PRIVATE src bvh ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_bvh4_build_layout PRIVATE vel_rt_bvh4_reference)
   set_target_properties(test_bvh4_build_layout PROPERTIES CXX_STANDARD 17)
   add_test(NAME vkrt.bvh4_build_layout COMMAND test_bvh4_build_layout)
   set_tests_properties(vkrt.bvh4_build_layout PROPERTIES LABELS "offline;no_native_rt")
   add_test(NAME vkrt.host_translation COMMAND test_host_translation)
   add_executable(test_as_address_cells tests/test_as_address_cells.c src/vkrt_virtual_as.c src/vkrt_as_registry.c src/vkrt_translate.c)
   target_include_directories(test_as_address_cells PRIVATE src bvh ${VKRT_COMMON_BVH4} ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_as_address_cells PRIVATE Threads::Threads)
   set_target_properties(test_as_address_cells PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.as_address_cells COMMAND test_as_address_cells)
   set_tests_properties(vkrt.as_address_cells PROPERTIES LABELS "offline;concurrency" TIMEOUT 60)
   add_executable(test_as_registry tests/test_as_registry.c src/vkrt_as_registry.c)
   target_include_directories(test_as_registry PRIVATE src ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_as_registry PRIVATE Threads::Threads)
   set_target_properties(test_as_registry PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.as_registry COMMAND test_as_registry)
   set_tests_properties(vkrt.as_registry PROPERTIES LABELS "offline;no_native_rt;concurrency" TIMEOUT 60)
   add_executable(test_deferred tests/test_deferred.c src/vkrt_deferred.c)
   target_include_directories(test_deferred PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_deferred PRIVATE Threads::Threads)
   set_target_properties(test_deferred PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_test(NAME vkrt.deferred COMMAND test_deferred)
   set_tests_properties(vkrt.deferred PROPERTIES LABELS "offline;no_native_rt;concurrency" TIMEOUT 60)
   add_executable(test_as_copy_host tests/test_as_copy_host.c src/vkrt_as_copy.c)
   target_include_directories(test_as_copy_host PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/bvh ${VKRT_COMMON_BVH4} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_as_copy_host PRIVATE Threads::Threads)
   set_target_properties(test_as_copy_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_as_copy_host vkrt_bvh_spv)
   add_test(NAME vkrt.as_copy_host COMMAND test_as_copy_host)
   add_executable(test_as_serialization_host tests/test_as_serialization_host.c src/vkrt_as_serialization.c)
   target_include_directories(test_as_serialization_host PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/bvh ${VKRT_COMMON_BVH4} ${Vulkan_INCLUDE_DIRS})
   target_link_libraries(test_as_serialization_host PRIVATE Threads::Threads)
   set_target_properties(test_as_serialization_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
   add_dependencies(test_as_serialization_host vkrt_bvh_spv)
   add_test(NAME vkrt.as_serialization_host COMMAND test_as_serialization_host)
   set_tests_properties(vkrt.as_serialization_host PROPERTIES LABELS "offline;no_native_rt")
   set_tests_properties(vkrt.as_copy_host PROPERTIES LABELS "offline;no_native_rt")
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
   add_test(NAME vkrt.gpu_build.aligned_values COMMAND test_gpu_build --aligned-values)
   add_test(NAME vkrt.gpu_build.aligned_values.native COMMAND test_gpu_build --aligned-values)
   add_test(NAME vkrt.as_copy.software COMMAND test_as_copy)
   add_test(NAME vkrt.as_update.software COMMAND test_as_update)
   add_test(NAME vkrt.as_serialization.software COMMAND test_as_serialization)
   add_test(NAME vkrt.query_properties.software COMMAND test_query_properties
      ${CMAKE_CURRENT_BINARY_DIR}/bvh/vkrt_as_query.spv ${CMAKE_CURRENT_BINARY_DIR}/bvh/vkrt_query_format.spv)
   set_tests_properties(vkrt.cpu_fixture vkrt.gpu_build.software vkrt.gpu_build.aligned_values vkrt.as_copy.software
      vkrt.as_update.software vkrt.as_serialization.software vkrt.query_properties.software PROPERTIES
      ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layers;VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
      LABELS "gpu" TIMEOUT 600)
   set_tests_properties(vkrt.gpu_build.native PROPERTIES ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" TIMEOUT 600)
   set_tests_properties(vkrt.gpu_build.aligned_values.native PROPERTIES ENVIRONMENT "VK_INSTANCE_LAYERS=" LABELS "gpu;native_rt" TIMEOUT 120)
   set_tests_properties(vkrt.spirv_lower PROPERTIES LABELS "offline")
endif()

# Compiler component tests share the production typed compiler.
if(BUILD_TESTING)
   include(${CMAKE_CURRENT_SOURCE_DIR}/compiler_tests.cmake)
endif()
