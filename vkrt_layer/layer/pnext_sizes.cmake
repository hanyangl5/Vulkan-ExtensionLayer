# Generate sizes from the registry matching the selected Vulkan headers.
# No Vulkan-Utility-Libraries checkout is required.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_path(VKRT_VULKAN_CORE_INCLUDE_DIR vulkan/vulkan_core.h
   HINTS ${Vulkan_INCLUDE_DIRS} REQUIRED)
find_file(VKRT_VULKAN_REGISTRY vk.xml
   HINTS "$ENV{VULKAN_SDK}/share/vulkan/registry"
         "${VKRT_VULKAN_CORE_INCLUDE_DIR}/../share/vulkan/registry"
   PATHS /usr/share/vulkan/registry
   DOC "Vulkan registry matching the selected Vulkan headers")
if(NOT VKRT_VULKAN_REGISTRY)
   message(FATAL_ERROR "vk.xml not found. Set VKRT_VULKAN_REGISTRY to the registry from your Vulkan SDK/Headers installation.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
   "${VKRT_VULKAN_REGISTRY}"
   "${VKRT_VULKAN_CORE_INCLUDE_DIR}/vulkan/vulkan_core.h"
   "${CMAKE_CURRENT_LIST_DIR}/generate_pnext_sizes.py")
execute_process(
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_pnext_sizes.py"
           "${VKRT_VULKAN_REGISTRY}"
           "${VKRT_VULKAN_CORE_INCLUDE_DIR}/vulkan/vulkan_core.h"
           "${CMAKE_CURRENT_BINARY_DIR}/vkrt_pnext_sizes.h"
   RESULT_VARIABLE _vkrt_pnext_result
   ERROR_VARIABLE _vkrt_pnext_error)
if(NOT _vkrt_pnext_result EQUAL 0)
   message(FATAL_ERROR "Cannot generate pNext sizes: ${_vkrt_pnext_error}")
endif()
