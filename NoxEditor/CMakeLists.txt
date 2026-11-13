set (PROJECT_NAME "NoxEditor")

add_executable(${PROJECT_NAME})

file(GLOB_RECURSE NOXEDITOR_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# The MCP server (src/Mcp, docs/MCP_Plan_2026-11.md): left out entirely with NOX_WITH_MCP=OFF.
if(NOX_WITH_MCP)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOX_WITH_MCP=1)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/vendors/cpp-httplib")
else()
    list(FILTER NOXEDITOR_FILES EXCLUDE REGEX ".*/src/Mcp/.*")
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOX_WITH_MCP=0)
endif()

target_sources(${PROJECT_NAME} PRIVATE ${NOXEDITOR_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE NoxCore)
# NoxCore's precompiled header (NoxCore/src/NoxPCH.h), built for this target's own compile flags.
target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")

set(NOX_SCRIPT_CORE_OUTPUT "${CMAKE_BINARY_DIR}/managed/Nox.ScriptCore")
set(NOX_FACERUN_SCRIPT_OUTPUT "${CMAKE_BINARY_DIR}/managed/Facerun")
add_custom_target(NoxManagedScripts ALL
    # A fresh/clean build can run this target's commands before NoxEditor's own compile step has created its output
    # directory at all (add_dependencies below only orders this before NoxEditor's LINK, not after its output
    # directory exists) -- the copy_if_different/copy_directory commands further down all target
    # $<TARGET_FILE_DIR:${PROJECT_NAME}>, which then doesn't exist yet ("Target ... is not a directory"). A build
    # where it happens to already exist from an earlier build never showed this. make_directory is a no-op when the
    # directory is already there, so this is safe to run unconditionally, every time.
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND dotnet build "${CMAKE_SOURCE_DIR}/NoxScriptCore/Nox.ScriptCore.csproj"
        --configuration $<CONFIG>
        --output "${NOX_SCRIPT_CORE_OUTPUT}"
    COMMAND dotnet build "${CMAKE_SOURCE_DIR}/Facerun/Assets/Scripts/Facerun.csproj"
        --configuration $<CONFIG>
        --output "${NOX_FACERUN_SCRIPT_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NOX_CORAL_RUNTIME_DIR}/Coral.Managed.dll"
        "${NOX_CORAL_RUNTIME_DIR}/Coral.Managed.deps.json"
        "${NOX_CORAL_RUNTIME_DIR}/Coral.Managed.runtimeconfig.json"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    # copy_directory_if_different (here and below): a NoxRuntime still running from this folder (a server while a client is
    # started from the IDE, which builds first) holds its DLLs open; unchanged files are left alone instead of failing the build.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${NOX_SCRIPT_CORE_OUTPUT}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Core"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${NOX_FACERUN_SCRIPT_OUTPUT}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Facerun"
    DEPENDS CoralManaged
    COMMENT "Building Nox managed scripting API and Facerun scripts"
    VERBATIM
)
add_dependencies(${PROJECT_NAME} NoxManagedScripts)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
# 1. Set Visual Studio Debugger Working Directory to NoxEditor source folder
#    (Fixes relative path resolution during F5 debug in Visual Studio)
# -----------------------------------------------------------------------------
if(WIN32)
    if(MSVC OR ${CMAKE_GENERATOR} MATCHES "Visual Studio.*")
        set_target_properties(${PROJECT_NAME} PROPERTIES 
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    endif()

    # -------------------------------------------------------------------------
    # 2. Copy dynamic libraries (slang.dll) next to NoxEditor.exe
    # -------------------------------------------------------------------------
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/slang/bin/slang.dll"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Templates/CSharpBehaviour.cs.template"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates/CSharpBehaviour.cs.template"
        # The Project Manager's template pictures
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Templates/Basic.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Templates/Blank.png"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates"
    )

    # -------------------------------------------------------------------------
    # 3. Copy assets/ directory to the build output folder
    #    (Ensures NoxEditor.exe can find assets/ when launched standalone or via RenderDoc)
    # -------------------------------------------------------------------------
    #add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    #    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
    #        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    #        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    #)

    # Copy Streamline runtime binaries next to the editor executable
    # (In Debug, copies development binaries; in Release, copies production binaries)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/Streamline/bin/x64/$<IF:$<CONFIG:Debug>,development,>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/development"
    )

    # Steam Audio's runtime (the runtime and packaged games load it from here too)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/NoxCore/vendors/steamaudio/lib/windows-x64/phonon.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )

    # Copy NRD runtime binaries next to the editor executable
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/NoxCore/vendors/NRD/Lib/$<IF:$<CONFIG:Debug>,Debug,Release>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endif()
