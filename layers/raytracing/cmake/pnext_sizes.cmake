# Generate sizes from the registry matching the selected Vulkan headers.
# No Vulkan-Utility-Libraries checkout is required.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
# Resolve from the selected Vulkan include path on each configure. A separate
# cached find_path can otherwise keep an old SDK header after Vulkan_INCLUDE_DIR
# changes, generating a smaller pNext table than the compiler's actual headers.
unset(_vkrt_selected_vulkan_include)
find_path(_vkrt_selected_vulkan_include vulkan/vulkan_core.h
   PATHS ${Vulkan_INCLUDE_DIRS} NO_DEFAULT_PATH NO_CACHE REQUIRED)
set(VKRT_VULKAN_CORE_INCLUDE_DIR "${_vkrt_selected_vulkan_include}")
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
