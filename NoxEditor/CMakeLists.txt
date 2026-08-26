set (PROJECT_NAME "NoxEditor")

add_executable(${PROJECT_NAME})

file(GLOB_RECURSE NOXEDITOR_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX ""
    FILES ${NOXEDITOR_FILES}
)

target_sources(${PROJECT_NAME} PRIVATE ${NOXEDITOR_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE NoxCore)

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
endif()

if(APPLE)

    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/NoxCore/vendors/slang/lib/libslang.dylib"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )


endif()
