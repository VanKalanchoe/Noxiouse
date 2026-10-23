# NoxRuntime: the standalone player (docs/Runtime_And_Packaging_Plan_2026-10.md, R1). Built with -DNOX_BUILD_RUNTIME=ON.
set (PROJECT_NAME "NoxRuntime")

add_executable(${PROJECT_NAME})

file(GLOB_RECURSE NOXRUNTIME_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(${PROJECT_NAME} PRIVATE ${NOXRUNTIME_FILES})
target_link_libraries(${PROJECT_NAME} PRIVATE NoxCore)
# NoxCore's precompiled header (NoxCore/src/NoxPCH.h), built for this target's own compile flags.
target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Next to NoxEditor.exe: the editor's build puts the managed scripts (Scripts/Core, Scripts/Facerun), Coral, slang, Streamline
# and NRD binaries there, and the runtime loads the same ones. Its own output folder with everything copied is R7 (Build Game).
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:NoxEditor>")
add_dependencies(${PROJECT_NAME} NoxEditor)

# Shipping: a windowed application (no console next to the game); SDL_main.h supplies WinMain.
if(NOX_DIST AND WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE ON)
endif()

if(WIN32 AND (MSVC OR ${CMAKE_GENERATOR} MATCHES "Visual Studio.*"))
    # Engine assets (shaders, fonts) are found relative to NoxEditor/ like for the editor; the sample project is played by default.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/NoxEditor"
        VS_DEBUGGER_COMMAND_ARGUMENTS "${CMAKE_SOURCE_DIR}/Facerun/Facerun.nproj"
    )
endif()
