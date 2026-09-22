set (PROJECT_NAME "NoxEditor")

add_executable(${PROJECT_NAME})

file(GLOB_RECURSE NOXEDITOR_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(${PROJECT_NAME} PRIVATE ${NOXEDITOR_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE NoxCore)

set(NOX_SCRIPT_CORE_OUTPUT "${CMAKE_BINARY_DIR}/managed/Nox.ScriptCore")
set(NOX_FACERUN_SCRIPT_OUTPUT "${CMAKE_BINARY_DIR}/managed/Facerun")
add_custom_target(NoxManagedScripts ALL
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
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${NOX_SCRIPT_CORE_OUTPUT}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Core"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
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
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/Streamline/bin/x64/$<IF:$<CONFIG:Debug>,development,>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/development"
    )

    # Copy NRD runtime binaries next to the editor executable
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/NoxCore/vendors/NRD/Lib/$<IF:$<CONFIG:Debug>,Debug,Release>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endif()
