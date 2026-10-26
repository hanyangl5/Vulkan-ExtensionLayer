# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
add_library(vel_omm_core STATIC omm_opacity.cpp omm_storage.cpp)
target_compile_features(vel_omm_core PUBLIC cxx_std_17)
target_include_directories(vel_omm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(vel_omm_metadata STATIC omm_metadata.cpp)
target_link_libraries(vel_omm_metadata PUBLIC vel_omm_core)
add_executable(test_omm_metadata tests/test_omm_metadata.cpp)
target_link_libraries(test_omm_metadata PRIVATE vel_omm_metadata)
add_test(NAME omm_metadata COMMAND test_omm_metadata)
set_tests_properties(omm_metadata PROPERTIES LABELS "offline;omm")
if(MSVC)
    target_compile_options(vel_omm_core PRIVATE /W4 /WX)
    target_compile_options(vel_omm_metadata PRIVATE /W4 /WX)
else()
    target_compile_options(vel_omm_core PRIVATE -Wall -Wextra -Werror)
    target_compile_options(vel_omm_metadata PRIVATE -Wall -Wextra -Werror)
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
    find_library(OMM_SPIRV_LINK_LIBRARY NAMES SPIRV-Tools-link
        HINTS "${Vulkan_ROOT}/Lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    find_library(OMM_SPIRV_OPT_LIBRARY NAMES SPIRV-Tools-opt
        HINTS "${Vulkan_ROOT}/Lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    add_library(vel_omm_shader_compose STATIC omm_shader_compose.cpp)
    target_compile_features(vel_omm_shader_compose PUBLIC cxx_std_17)
    target_include_directories(vel_omm_shader_compose PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vel_omm_shader_compose PUBLIC ${OMM_SPIRV_LINK_LIBRARY} ${OMM_SPIRV_OPT_LIBRARY} SPIRV-Tools-static)
    add_executable(test_omm_shader_compose tests/test_omm_shader_compose.cpp)
    target_link_libraries(test_omm_shader_compose PRIVATE vel_omm_shader_compose)
    add_test(NAME omm_shader_compose COMMAND test_omm_shader_compose)
    set_tests_properties(omm_shader_compose PROPERTIES LABELS "offline;omm")
    if(MSVC)
        target_compile_options(vel_omm_query_rewrite PRIVATE /W4 /WX)
        target_compile_options(vel_omm_shader_compose PRIVATE /W4 /WX)
    else()
        target_compile_options(vel_omm_query_rewrite PRIVATE -Wall -Wextra -Werror)
        target_compile_options(vel_omm_shader_compose PRIVATE -Wall -Wextra -Werror)
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

    set(extra_shaders query_state.comp omm_build.comp omm_copy.comp omm_metadata.comp gpu_build_generate.comp gpu_lookup.comp layer_state.comp
        classifier_library.comp classifier_query.comp composition_workgroup.comp)
    foreach(stage rgen rmiss rahit rchit rint)
        list(APPEND extra_shaders native_pipeline.${stage})
    endforeach()
    set(shader_outputs)
    foreach(shader ${extra_shaders})
        if(shader STREQUAL "omm_build.comp" OR shader STREQUAL "omm_copy.comp" OR shader STREQUAL "omm_metadata.comp")
            set(shader_source ${CMAKE_CURRENT_SOURCE_DIR}/${shader})
        else()
            set(shader_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/${shader})
        endif()
        set(shader_flags)
        if(shader STREQUAL "classifier_library.comp")
            list(APPEND shader_flags --keep-uncalled)
        endif()
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv
            COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2 ${shader_flags} ${shader_source}
                -o ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv
            COMMAND ${OMM_SPIRV_VALIDATOR} --target-env vulkan1.2 ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv
            DEPENDS ${shader_source} tests/native_pipeline.glsl omm_opacity.glsl omm_lookup.glsl omm_classifier.glsl VERBATIM)
        list(APPEND shader_outputs ${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv)
    endforeach()
    add_custom_target(omm_extra_spv DEPENDS ${shader_outputs})
    add_executable(test_omm_metadata_gpu tests/test_omm_metadata_gpu.cpp)
    target_link_libraries(test_omm_metadata_gpu PRIVATE vel_omm_metadata Vulkan::Vulkan)
    add_dependencies(test_omm_metadata_gpu omm_extra_spv)
    add_test(NAME omm_metadata_gpu COMMAND test_omm_metadata_gpu ${CMAKE_CURRENT_BINARY_DIR}/omm_metadata.comp.spv
        ${CMAKE_CURRENT_BINARY_DIR}/omm_build.comp.spv ${CMAKE_CURRENT_BINARY_DIR}/gpu_build_generate.comp.spv)
    set_tests_properties(omm_metadata_gpu PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
    add_executable(test_omm_classifier tests/test_omm_classifier.cpp)
    target_link_libraries(test_omm_classifier PRIVATE vel_omm_metadata vel_omm_query_rewrite vel_omm_shader_compose Vulkan::Vulkan)
    add_dependencies(test_omm_classifier omm_extra_spv)
    add_test(NAME omm_classifier COMMAND test_omm_classifier ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(omm_classifier PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
    add_executable(test_omm_compose_workgroup tests/test_omm_compose_workgroup.cpp)
    target_link_libraries(test_omm_compose_workgroup PRIVATE vel_omm_shader_compose Vulkan::Vulkan)
    add_dependencies(test_omm_compose_workgroup omm_extra_spv)
    add_test(NAME omm_compose_workgroup COMMAND test_omm_compose_workgroup ${CMAKE_CURRENT_BINARY_DIR}/composition_workgroup.comp.spv
        ${CMAKE_CURRENT_BINARY_DIR}/classifier_library.comp.spv)
    set_tests_properties(omm_compose_workgroup PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
    add_library(vel_omm_device STATIC omm_device.cpp)
    target_link_libraries(vel_omm_device PUBLIC vel_omm_core Vulkan::Vulkan)
    add_library(vel_omm_commands STATIC omm_command_state.cpp)
    target_compile_features(vel_omm_commands PUBLIC cxx_std_17)
    target_link_libraries(vel_omm_commands PUBLIC Vulkan::Vulkan)
    target_include_directories(vel_omm_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    add_library(vel_omm_sync STATIC omm_sync.cpp)
    target_compile_features(vel_omm_sync PUBLIC cxx_std_17)
    target_link_libraries(vel_omm_sync PUBLIC Vulkan::Vulkan)
    target_include_directories(vel_omm_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(test_omm_sync tests/test_omm_sync.cpp)
    target_link_libraries(test_omm_sync PRIVATE vel_omm_sync)
    add_test(NAME omm_sync COMMAND test_omm_sync)
    set_tests_properties(omm_sync PROPERTIES LABELS "offline;omm")
    add_executable(test_omm_command_state tests/test_omm_command_state.cpp)
    target_link_libraries(test_omm_command_state PRIVATE vel_omm_commands)
    add_test(NAME omm_command_state COMMAND test_omm_command_state)
    set_tests_properties(omm_command_state PROPERTIES LABELS "offline;omm")
    add_executable(test_omm_device tests/test_omm_device.cpp)
    target_link_libraries(test_omm_device PRIVATE vel_omm_device)
    add_dependencies(test_omm_device omm_extra_spv)
    add_test(NAME omm_device COMMAND test_omm_device ${CMAKE_CURRENT_BINARY_DIR}/omm_build.comp.spv
        ${CMAKE_CURRENT_BINARY_DIR}/omm_copy.comp.spv ${CMAKE_CURRENT_BINARY_DIR}/gpu_build_generate.comp.spv)
    set_tests_properties(omm_device PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
    add_executable(test_omm_layer tests/test_omm_layer.cpp)
    target_compile_definitions(test_omm_layer PRIVATE NOMINMAX)
    target_link_libraries(test_omm_layer PRIVATE vel_omm_core Vulkan::Vulkan)
    add_dependencies(test_omm_layer omm_extra_spv)
    add_test(NAME omm_layer COMMAND test_omm_layer ${CMAKE_CURRENT_BINARY_DIR}/layer_state.comp.spv)
    set_tests_properties(omm_layer PROPERTIES LABELS "gpu;native_rt;omm;experimental;loader" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
    add_executable(test_omm_query_proceed tests/test_omm_query_proceed.cpp)
    target_link_libraries(test_omm_query_proceed PRIVATE vel_omm_query_rewrite vel_omm_core Vulkan::Vulkan)
    add_test(NAME omm_query_proceed COMMAND test_omm_query_proceed)
    set_tests_properties(omm_query_proceed PROPERTIES LABELS "offline;omm")
    add_test(NAME omm_query_proceed_gpu COMMAND test_omm_query_proceed --gpu)
    set_tests_properties(omm_query_proceed_gpu PROPERTIES LABELS "gpu;native_rt;omm;experimental" SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "Validation Error;SYNC-HAZARD")
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/omm_kernels.h
        COMMAND ${CMAKE_COMMAND} -DINPUT_build=${CMAKE_CURRENT_BINARY_DIR}/omm_build.comp.spv
            -DINPUT_copy=${CMAKE_CURRENT_BINARY_DIR}/omm_copy.comp.spv -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/omm_kernels.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_kernels.cmake
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/omm_build.comp.spv ${CMAKE_CURRENT_BINARY_DIR}/omm_copy.comp.spv embed_kernels.cmake)
    find_library(OMM_SAFE_STRUCT_LIBRARY NAMES VulkanSafeStruct
        HINTS "${Vulkan_ROOT}/Lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    add_library(VkLayer_VEL_opacity_micromap MODULE omm_layer.cpp ${CMAKE_CURRENT_BINARY_DIR}/omm_kernels.h)
    if(WIN32)
        target_sources(VkLayer_VEL_opacity_micromap PRIVATE omm_layer.def)
    endif()
    target_include_directories(VkLayer_VEL_opacity_micromap PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(VkLayer_VEL_opacity_micromap PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(VkLayer_VEL_opacity_micromap PRIVATE vel_omm_device vel_omm_commands vel_omm_sync ${OMM_SAFE_STRUCT_LIBRARY})
    set_target_properties(VkLayer_VEL_opacity_micromap PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/layers LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/layers)
    set(omm_layer_library_path "./$<TARGET_FILE_NAME:VkLayer_VEL_opacity_micromap>")
    if(WIN32)
        set(omm_layer_library_path ".\\\\$<TARGET_FILE_NAME:VkLayer_VEL_opacity_micromap>")
    endif()
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/manifest/VkLayer_VEL_opacity_micromap.json CONTENT
        "{\n  \"file_format_version\": \"1.2.0\",\n  \"layer\": {\n    \"name\": \"VK_LAYER_VEL_opacity_micromap\",\n    \"type\": \"GLOBAL\",\n    \"library_path\": \"${omm_layer_library_path}\",\n    \"api_version\": \"1.4.357\",\n    \"implementation_version\": \"1\",\n    \"description\": \"Experimental native RT opacity micromap adapter\"\n  }\n}\n")
    add_custom_command(TARGET VkLayer_VEL_opacity_micromap POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/manifest/VkLayer_VEL_opacity_micromap.json $<TARGET_FILE_DIR:VkLayer_VEL_opacity_micromap>)
    add_custom_target(omm_layer_manifest ALL COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/manifest/VkLayer_VEL_opacity_micromap.json $<TARGET_FILE_DIR:VkLayer_VEL_opacity_micromap>
        DEPENDS VkLayer_VEL_opacity_micromap ${CMAKE_CURRENT_BINARY_DIR}/manifest/VkLayer_VEL_opacity_micromap.json)
    add_dependencies(test_omm_layer omm_layer_manifest)
    add_executable(test_omm_layer_registry tests/test_omm_layer_registry.cpp ${CMAKE_CURRENT_BINARY_DIR}/omm_kernels.h)
    target_compile_definitions(test_omm_layer_registry PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(test_omm_layer_registry PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(test_omm_layer_registry PRIVATE vel_omm_device vel_omm_commands vel_omm_sync ${OMM_SAFE_STRUCT_LIBRARY})
    add_test(NAME omm_layer_registry COMMAND test_omm_layer_registry)
    set_tests_properties(omm_layer_registry PROPERTIES LABELS "offline;omm")
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
