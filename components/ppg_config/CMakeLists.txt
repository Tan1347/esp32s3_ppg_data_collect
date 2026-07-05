# 生成版本时间戳
execute_process(
    COMMAND date "+%Y-%m-%d %H:%M:%S"
    OUTPUT_VARIABLE BUILD_TIMESTAMP
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/version_ts.h"
     "#define PPG_FW_BUILD_TS \"${BUILD_TIMESTAMP}\"\n")

idf_component_register(
    INCLUDE_DIRS "include"
)
