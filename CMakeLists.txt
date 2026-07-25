cmake_minimum_required(VERSION 3.31)

project(Noxiouse VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "Use dynamic runtime for msdf-atlas-gen" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "Use dynamic runtime for msdfgen" FORCE)
add_subdirectory(NoxCore/vendors/msdf-atlas-gen EXCLUDE_FROM_ALL)

add_subdirectory(NoxCore)
add_subdirectory(NoxEditor)