cmake_minimum_required (VERSION 3.28)
project (DiscordRPC)

include(GNUInstallDirs)

option(BUILD_EXAMPLES "Build example apps" ON)

# format
file(GLOB_RECURSE ALL_SOURCE_FILES
    examples/*.cpp examples/*.h examples/*.c
    include/*.h
    src/*.cpp src/*.h src/*.c
)

if (PROJECT_IS_TOP_LEVEL)
    # Set CLANG_FORMAT_SUFFIX if you are using custom clang-format, e.g. clang-format-5.0
    find_program(CLANG_FORMAT_CMD clang-format${CLANG_FORMAT_SUFFIX})

    if (CLANG_FORMAT_CMD)
        add_custom_target(
            clangformat
            COMMAND ${CLANG_FORMAT_CMD}
            -i -style=file -fallback-style=none
            ${ALL_SOURCE_FILES}
            DEPENDS
            ${ALL_SOURCE_FILES}
        )
    endif(CLANG_FORMAT_CMD)
endif(PROJECT_IS_TOP_LEVEL)

# thirdparty stuff
execute_process(
    COMMAND mkdir ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ERROR_QUIET
)

set(RAPIDJSON "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rapidjson-1.1.0")

# add subdirs

add_subdirectory(src)
if (BUILD_EXAMPLES)
    add_subdirectory(examples/send-presence)
endif(BUILD_EXAMPLES)
