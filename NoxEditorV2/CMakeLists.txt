set(PROJECT_NAME "NoxEditorV2")

add_executable(${PROJECT_NAME})

file(GLOB_RECURSE NOXEDITORV2_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(${PROJECT_NAME} PRIVATE
    ${NOXEDITORV2_FILES}
)
target_link_libraries(${PROJECT_NAME} PRIVATE NoxCore)
target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(NOX_WITH_MCP)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOX_WITH_MCP=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOX_WITH_MCP=0)
endif()

if(TARGET NoxManagedScripts)
    add_dependencies(${PROJECT_NAME} NoxManagedScripts)
endif()

if(WIN32)
    if(MSVC OR ${CMAKE_GENERATOR} MATCHES "Visual Studio.*")
        set_target_properties(${PROJECT_NAME} PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/NoxEditor"
        )
    endif()

    set(NOX_V2_SCRIPT_CORE_OUTPUT "${CMAKE_BINARY_DIR}/managed/Nox.ScriptCore")
    set(NOX_V2_FACERUN_SCRIPT_OUTPUT "${CMAKE_BINARY_DIR}/managed/Facerun")

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/slang/bin/slang.dll"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NOX_CORAL_RUNTIME_DIR}/Coral.Managed.dll"
            "${NOX_CORAL_RUNTIME_DIR}/Coral.Managed.deps.json"
            "${NOX_CORAL_RUNTIME_DIR}/Coral.Managed.runtimeconfig.json"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${NOX_V2_SCRIPT_CORE_OUTPUT}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Core"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${NOX_V2_FACERUN_SCRIPT_OUTPUT}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Facerun"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/NoxEditor/assets/Templates/CSharpBehaviour.cs.template"
            "${CMAKE_SOURCE_DIR}/NoxEditor/assets/Templates/Basic.png"
            "${CMAKE_SOURCE_DIR}/NoxEditor/assets/Templates/Blank.png"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates"
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/Streamline/bin/x64/$<IF:$<CONFIG:Debug>,development,>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/development"
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/steamaudio/lib/windows-x64/phonon.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/NRD/Lib/$<IF:$<CONFIG:Debug>,Debug,Release>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endif()
