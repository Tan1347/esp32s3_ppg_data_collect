# Auto-generate version header from version.txt (single source of truth)
if(NOT PROJECT_VER)
    if(EXISTS "${CMAKE_SOURCE_DIR}/version.txt")
        file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
        string(STRIP "${PROJECT_VER}" PROJECT_VER)
    else()
        set(PROJECT_VER "0.0.0")
    endif()
endif()
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/version.h"
     "#pragma once\n#define PPG_FW_VERSION \"${PROJECT_VER}\"\n")

# Auto-generate build timestamp header
execute_process(
    COMMAND date "+%Y-%m-%d %H:%M:%S"
    OUTPUT_VARIABLE BUILD_TIMESTAMP
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/version_ts.h"
     "#pragma once\n#define PPG_FW_BUILD_TS \"${BUILD_TIMESTAMP}\"\n")

idf_component_register(
    INCLUDE_DIRS "include"
)
