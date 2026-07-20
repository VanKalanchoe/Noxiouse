cmake_minimum_required(VERSION 3.31)

project(Noxiouse VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

add_subdirectory(noxcore)
add_subdirectory(noxeditor)

set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "Use dynamic runtime for msdf-atlas-gen" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "Use dynamic runtime for msdfgen" FORCE)
add_subdirectory(NoxCore/vendors/msdf-atlas-gen EXCLUDE_FROM_ALL)