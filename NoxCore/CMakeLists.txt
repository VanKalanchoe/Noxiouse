set (PROJECT_NAME "NoxCore")

# Add option to enable/disable C++ 20 module
option(ENABLE_CPP20_MODULE "Enable C++ 20 module support for Vulkan" OFF)

# Enable C++ module dependency scanning only if C++ 20 module is enabled
if(ENABLE_CPP20_MODULE)
  set(CMAKE_CXX_SCAN_FOR_MODULES ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")

find_package (sdl3 REQUIRED)
find_package (glm REQUIRED)
find_package (Vulkan 1.4.350 REQUIRED)
find_package (entt REQUIRED)
find_package (Stb REQUIRED)
set(STB_INCLUDEDIR ${stb_INCLUDE_DIRS})
find_package (KTX REQUIRED)
find_package (tinyobjloader REQUIRED)
find_package (xxhash REQUIRED)
find_package (yaml-cpp REQUIRED)
find_package (spdlog CONFIG REQUIRED)
find_package (box2d REQUIRED)

# Define Slang as an imported library
add_library(Slang::slang IMPORTED UNKNOWN)

# Set the include directory
set(SLANG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/include")
target_include_directories(Slang::slang INTERFACE "${SLANG_INCLUDE_DIR}")

# Set the library path
if(WIN32)
    set(SLANG_LIB "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/lib/slang.lib")
    
    # CRITICAL: This sets the actual file location for the linker
    set_target_properties(Slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_LIB}"
    )
else()
    set(SLANG_LIB "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/lib/libslang.dylib")
    set_target_properties(Slang::slang PROPERTIES
        IMPORTED_LOCATION "${SLANG_LIB}"
    )
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    # Add the specific backends matching your engine tech!
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp

    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)

set(IMGUIZMO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/ImGuizmo")
set(IMGUIZMO_SOURCES
    ${IMGUIZMO_DIR}/src/ImGuizmo.cpp
)

# Set up Vulkan C++ module only if enabled
if(ENABLE_CPP20_MODULE)
  add_library(VulkanCppModule)
  add_library(Vulkan::cppm ALIAS VulkanCppModule)

  target_compile_definitions(VulkanCppModule PUBLIC VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1)
  target_include_directories(VulkanCppModule PUBLIC "${Vulkan_INCLUDE_DIR}")
  target_link_libraries(VulkanCppModule PUBLIC Vulkan::Vulkan)

  set_target_properties(VulkanCppModule PROPERTIES CXX_STANDARD 20)

  # Add MSVC-specific compiler options for proper C++ module support
  if(MSVC)
    target_compile_options(VulkanCppModule PRIVATE
      /std:c++latest      # Use latest C++ standard for better module support
      /permissive-        # Standards conformance mode
      /Zc:__cplusplus     # Enable correct __cplusplus macro
      /EHsc               # Enable C++ exception handling
      /Zc:preprocessor    # Use conforming preprocessor
      /translateInclude   # Automatically translate #include to import for standard library
    )
  endif()

  target_sources(VulkanCppModule
    PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES
    BASE_DIRS
    "${Vulkan_INCLUDE_DIR}"
    FILES
    "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.cppm"
  )


  # Add the vulkan.cppm file directly as a source file
  target_sources(VulkanCppModule
    PRIVATE
    "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.cppm"
  )
else()
  # Create a dummy interface library when C++ 20 module is disabled
  add_library(VulkanCppModule INTERFACE)
  add_library(Vulkan::cppm ALIAS VulkanCppModule)
  target_link_libraries(VulkanCppModule INTERFACE Vulkan::Vulkan KTX::ktx)
  target_compile_definitions(VulkanCppModule
    INTERFACE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1
  )
endif()

file(GLOB_RECURSE NOXCORE_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_library(${PROJECT_NAME} STATIC ${NOXCORE_FILES} ${IMGUI_SOURCES} ${IMGUIZMO_SOURCES}) #main file here
set_target_properties (${PROJECT_NAME} PROPERTIES CXX_STANDARD 20)
target_link_libraries (${PROJECT_NAME} PUBLIC Vulkan::cppm SDL3::SDL3 glm::glm spdlog::spdlog Slang::slang EnTT::EnTT xxHash::xxhash yaml-cpp::yaml-cpp box2d::box2d msdf-atlas-gen::msdf-atlas-gen tinyobjloader::tinyobjloader)
# Add here vendors
target_include_directories (${PROJECT_NAME} SYSTEM PRIVATE ${STB_INCLUDEDIR} ${CMAKE_CURRENT_SOURCE_DIR}/vendors/taskflow ${CMAKE_CURRENT_SOURCE_DIR}/vendors/VulkanMemoryAllocator-Hpp/include ${CMAKE_CURRENT_SOURCE_DIR}/vendors/VulkanMemoryAllocator-Hpp/VulkanMemoryAllocator/include ${IMGUI_DIR}/backends)

#vendors visible to other projects like editor
target_include_directories (${PROJECT_NAME} SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUIZMO_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/vendors/filewatch)

# Sysmtem means supress warnings from the include directories, so we don't get warnings from external libraries.
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/shaders)

# Add compile definition if C++ 20 module is enabled
if(ENABLE_CPP20_MODULE)
  target_compile_definitions(${PROJECT_NAME} PRIVATE USE_CPP20_MODULES=1)
endif()

# Define VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS to treat VK_ERROR_OUT_OF_DATE_KHR as a success code
target_compile_definitions(${PROJECT_NAME} PRIVATE "VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS" )

target_compile_options(${PROJECT_NAME} PRIVATE /std:c++latest)