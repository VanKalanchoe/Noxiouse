set (PROJECT_NAME "NoxCore")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package (sdl3 REQUIRED)
find_package (glm REQUIRED)
find_package (entt REQUIRED)
find_package (Stb REQUIRED)
set(STB_INCLUDEDIR ${stb_INCLUDE_DIRS})
find_package (KTX REQUIRED)
find_package (tinyobjloader REQUIRED)
find_package (xxhash REQUIRED)
find_package (yaml-cpp REQUIRED)
find_package (Freetype REQUIRED)
find_package (spdlog CONFIG REQUIRED)
find_package (box2d REQUIRED)
find_package (OpenVDB CONFIG QUIET)

set(RTXDI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/RTXDI")
set(RTXDI_INCLUDE_DIR "${RTXDI_DIR}/Include" "${RTXDI_DIR}/Include/Rtxdi" "${RTXDI_DIR}/include")
set(RTXDI_SHADERS_DIR "${RTXDI_DIR}/Include/Rtxdi" "${RTXDI_DIR}/shaders")
file(GLOB RTXDI_SOURCES "${RTXDI_DIR}/Source/*.cpp")

set(TINYGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/tinygltf")
#set(TINYGLTF_SOURCE
#    ${TINYGLTF_DIR}/tiny_gltf_v3.c
#)

set(MESHOPTIMIZER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/meshoptimizer/src")
file(GLOB MESHOPTIMIZER_SOURCES "${MESHOPTIMIZER_DIR}/*.cpp")
# meshoptimizer's cluster LOD builder (header-only, implemented in Asset/MeshImporter.cpp)
set(CLUSTERLOD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/meshoptimizer/demo")

# Range allocator for the unified geometry buffers (its tests are not part of the build)
set(OFFSETALLOCATOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/OffsetAllocator")
set(OFFSETALLOCATOR_SOURCES "${OFFSETALLOCATOR_DIR}/offsetAllocator.cpp")

# BC7 / BC5 / BC4 encoders for cooking PNG/JPG textures (the CPU encoders only: the command line tool, RDO and ISPC paths are not used)
set(BC7ENC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/bc7enc_rdo")
set(BC7ENC_SOURCES "${BC7ENC_DIR}/bc7enc.cpp" "${BC7ENC_DIR}/rgbcx.cpp") # rgbcx: BC4 / BC5 for grayscale and normal maps

# HLOD building (docs/Engine_Architecture_Plan_2026.md §5.13.2): xatlas lays out the HLOD's UV atlas, bcdec decodes BC1-BC7 textures
# on the CPU to bake the source materials (header-only, implemented in Scene/HLODBuilder.cpp).
set(XATLAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/xatlas/source/xatlas")
set(XATLAS_SOURCES "${XATLAS_DIR}/xatlas.cpp")
set(BCDEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/bcdec")

file(GLOB_RECURSE NOXCORE_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_library(${PROJECT_NAME} STATIC ${NOXCORE_FILES} ${TINYGLTF_SOURCE} ${MESHOPTIMIZER_SOURCES} ${OFFSETALLOCATOR_SOURCES} ${BC7ENC_SOURCES} ${XATLAS_SOURCES} ${RTXDI_SOURCES}) #main file here

# Precompiled header (src/NoxPCH.h): the standard library and the third-party headers most files include, parsed once instead of in
# every source file. The vendors' own sources compiled into this library build without it (their own configuration macros).
target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/NoxPCH.h")
set_source_files_properties(${MESHOPTIMIZER_SOURCES}
                            ${OFFSETALLOCATOR_SOURCES} ${BC7ENC_SOURCES} ${XATLAS_SOURCES} ${RTXDI_SOURCES}
                            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_link_libraries (${PROJECT_NAME} PUBLIC NoxTasks::NoxTasks NoxNet::NoxNet NoxAudio::NoxAudio NoxRHI::RenderGraph NoxUI::Core NoxUI::SDL3 NoxUI::NoxRHI Coral.Native KTX::ktx SDL3::SDL3 glm::glm spdlog::spdlog EnTT::EnTT xxHash::xxhash yaml-cpp::yaml-cpp box2d::box2d msdf-atlas-gen::msdf-atlas-gen Freetype::Freetype tinyobjloader::tinyobjloader Jolt)
# Add here vendors
target_include_directories (${PROJECT_NAME} SYSTEM PRIVATE ${STB_INCLUDEDIR})

#vendors visible to other projects like editor
target_include_directories (${PROJECT_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendors/filewatch ${CMAKE_CURRENT_SOURCE_DIR}/vendors/JoltPhysics)

# Sysmtem means supress warnings from the include directories, so we don't get warnings from external libraries.
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/NoxEditor/assets/shaders ${TINYGLTF_DIR} ${MESHOPTIMIZER_DIR} ${OFFSETALLOCATOR_DIR} ${BC7ENC_DIR} ${XATLAS_DIR} ${BCDEC_DIR} ${RTXDI_INCLUDE_DIR} ${RTXDI_SHADERS_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${CLUSTERLOD_DIR})
# Voxels (VX1): FastNoise2 and zstd only inside Voxel/*.cpp; the chunk index's map type is in Voxel/VoxelWorld.h.
target_link_libraries(${PROJECT_NAME} PRIVATE FastNoise2 libzstd_static)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendors/zstd/lib)
target_link_libraries(${PROJECT_NAME} PUBLIC unordered_dense::unordered_dense)

# -----------------------------------------------------------------------------
# Profiling (docs/Engine_Architecture_Plan_2026.md 5.1)
# -----------------------------------------------------------------------------
option(NOX_PROFILE_STATS "Build the in-engine Nox Stats profiler (F3 overlay)" ON)
option(NOX_ENABLE_TRACY "Build with the Tracy profiler client (connect with Tracy GUI v0.14.1)" OFF)

if(NOX_ENABLE_TRACY)
    # Tracy's own CMake integration: options are Tracy cache variables, the client is Tracy::TracyClient.
    set(TRACY_ENABLE ON CACHE BOOL "Enable profiling" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "On-demand profiling" FORCE) # nothing is collected until the GUI connects
    add_subdirectory(vendors/tracy EXCLUDE_FROM_ALL)
    # PRIVATE: TRACY_* definitions and includes only reach NoxCore (TracyBackend.cpp is the only user).
    target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
endif()

if(OpenVDB_FOUND)
    message(STATUS "NoxCore: OpenVDB found -- enabling .vdb -> .nvdb volume cooker (NOX_HAS_OPENVDB=1)")
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenVDB::openvdb)
    target_compile_definitions(${PROJECT_NAME} PUBLIC NOX_HAS_OPENVDB=1)
else()
    message(STATUS "NoxCore: OpenVDB not found -- .nvdb runtime loader active; install 'openvdb:x64-windows' via vcpkg to cook .vdb files")
    target_compile_definitions(${PROJECT_NAME} PUBLIC NOX_HAS_OPENVDB=0)
endif()

# PUBLIC so NoxCore/Debug/Profiler.h expands the same way in NoxCore and in every application.
target_compile_definitions(${PROJECT_NAME} PUBLIC
    NOX_PROFILE_STATS=$<BOOL:${NOX_PROFILE_STATS}>
    NOX_ENABLE_TRACY=$<BOOL:${NOX_ENABLE_TRACY}>
    NOX_BUILD_CONFIG="$<CONFIG>"
    NOX_DIST=$<BOOL:${NOX_DIST}>
)

# Large world coordinates (docs/Engine_Architecture_Plan_2026.md §5.14a): world positions are double precision, and glm's
# conversions between double and float (and between vector / matrix sizes) are explicit, so no world position loses its
# precision silently -- every narrowing is written out (Unreal's FVector3f(FVector) is explicit for the same reason).
target_compile_definitions(${PROJECT_NAME} PUBLIC GLM_FORCE_EXPLICIT_CTOR)

# Debug builds turn on NOX_ASSERT / NOX_CORE_ASSERT (NoxCore/Core/core.h). PUBLIC so the editor, the runtime and
# every header they include from NoxCore agree on it.
target_compile_definitions(${PROJECT_NAME} PUBLIC $<$<CONFIG:Debug>:NOX_DEBUG>)
