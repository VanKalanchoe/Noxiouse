set(PROJECT_NAME "NoxEditor")

add_executable(${PROJECT_NAME})

file(GLOB_RECURSE NOXEDITOR_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

if(NOX_WITH_MCP)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOX_WITH_MCP=1)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Mcp")
    target_link_libraries(${PROJECT_NAME} PRIVATE NoxEngineMcp)
else()
    list(FILTER NOXEDITOR_FILES EXCLUDE REGEX ".*/src/Mcp/.*")
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOX_WITH_MCP=0)
endif()

target_sources(${PROJECT_NAME} PRIVATE
    ${NOXEDITOR_FILES}
)
target_link_libraries(${PROJECT_NAME} PRIVATE NoxCore)
target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")

set(NOX_SCRIPT_CORE_OUTPUT "${CMAKE_BINARY_DIR}/managed/Nox.ScriptCore")
set(NOX_FACERUN_SCRIPT_OUTPUT "${CMAKE_BINARY_DIR}/managed/Facerun")
add_custom_target(NoxManagedScripts ALL
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
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${NOX_SCRIPT_CORE_OUTPUT}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Core"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${NOX_FACERUN_SCRIPT_OUTPUT}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Scripts/Facerun"
    DEPENDS Coral.Managed
    COMMENT "Building Nox managed scripting API and Facerun scripts"
    VERBATIM
)
add_dependencies(${PROJECT_NAME} NoxManagedScripts)

if(WIN32)
    if(MSVC OR ${CMAKE_GENERATOR} MATCHES "Visual Studio.*")
        set_target_properties(${PROJECT_NAME} PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/NoxEditor"
        )
    endif()

    if(DEFINED NETHOST)
        get_filename_component(NOX_NETHOST_DIR "${NETHOST}" DIRECTORY)
        if(EXISTS "${NOX_NETHOST_DIR}/nethost.dll")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NOX_NETHOST_DIR}/nethost.dll"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            )
        endif()
    endif()

    file(GLOB NOX_SLANG_DLLS "${CMAKE_SOURCE_DIR}/NoxCore/vendors/slang/bin/slang*.dll")
    list(FILTER NOX_SLANG_DLLS EXCLUDE REGEX "slang-llvm\\.dll$")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${NOX_SLANG_DLLS}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Templates/CSharpBehaviour.cs.template"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Templates/Basic.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Templates/Blank.png"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/Templates"
    )
    file(GLOB NOX_SLANG_STD_MODULES LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/NoxCore/vendors/slang/bin/slang-standard-module-*")
    foreach(SLANG_MOD_DIR ${NOX_SLANG_STD_MODULES})
        get_filename_component(SLANG_MOD_NAME "${SLANG_MOD_DIR}" NAME)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${SLANG_MOD_DIR}"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${SLANG_MOD_NAME}"
        )
    endforeach()
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(NOX_WINSDK_X64_BIN "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
        if(EXISTS "${NOX_WINSDK_X64_BIN}/dxcompiler.dll" AND EXISTS "${NOX_WINSDK_X64_BIN}/dxil.dll")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NOX_WINSDK_X64_BIN}/dxcompiler.dll"
                    "${NOX_WINSDK_X64_BIN}/dxil.dll"
                    $<TARGET_FILE_DIR:${PROJECT_NAME}>
            )
        endif()
    endif()

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
    if(EXISTS "${CMAKE_SOURCE_DIR}/NoxCore/vendors/AgilitySDK/D3D12Core.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/AgilitySDK"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/NoxCore/vendors/AgilitySDK/D3D12Core.dll"
                "${CMAKE_SOURCE_DIR}/NoxCore/vendors/AgilitySDK/d3d12SDKLayers.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/AgilitySDK"
        )
    endif()
endif()
