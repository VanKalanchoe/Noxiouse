add_executable(NoxEditor)

file(GLOB_RECURSE NOXEDITOR_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(NoxEditor PRIVATE ${NOXEDITOR_FILES})

target_link_libraries(NoxEditor PRIVATE NoxCore)

target_include_directories(NoxEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")