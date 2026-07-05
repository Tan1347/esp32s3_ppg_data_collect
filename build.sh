#!/bin/bash
# ESP32-S3 编译脚本
# 用法: ./build.sh [clean]
#   ./build.sh       # 增量编译
#   ./build.sh clean # 清理后全量编译

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="$SCRIPT_DIR/esp32s3_build.log"
OUTPUT_DIR="$SCRIPT_DIR/output"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v6.0.1}"

# 生成版本时间戳（避免 ccache 缓存）
echo "#define PPG_FW_BUILD_TS \"$(date '+%Y-%m-%d %H:%M:%S')\"" > "$SCRIPT_DIR/components/ppg_config/include/version_ts.h"

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  ESP32-S3 编译脚本${NC}"
echo -e "${GREEN}========================================${NC}"
echo "IDF_PATH: $IDF_PATH"
echo "输出目录: $OUTPUT_DIR"
echo "日志文件: $LOG_FILE"
echo ""

# 清理
if [ "$1" = "clean" ]; then
    echo -e "${YELLOW}[CLEAN] 清理构建目录...${NC}"
    rm -rf "$SCRIPT_DIR/build" "$SCRIPT_DIR/sdkconfig"
fi

# 记录开始时间
START_TIME=$(date +%s)
START_TS=$(date '+%Y-%m-%d %H:%M:%S')
echo "开始时间: $START_TS"

# 编译（日志同时输出到终端和文件）
BUILD_EXIT_CODE=0
{
    echo "========================================"
    echo "编译开始: $START_TS"
    echo "IDF_PATH: $IDF_PATH"
    echo "========================================"
    echo ""

    IDF_PATH="$IDF_PATH" bash -c "
        source '$IDF_PATH/export.sh' 2>/dev/null
        cd '$SCRIPT_DIR'
        idf.py build
    "
} 2>&1 | tee "$LOG_FILE" || BUILD_EXIT_CODE=$?

# 记录结束时间
END_TIME=$(date +%s)
END_TS=$(date '+%Y-%m-%d %H:%M:%S')
DURATION=$((END_TIME - START_TIME))
MINUTES=$((DURATION / 60))
SECONDS=$((DURATION % 60))

echo ""
echo "========================================"
echo "开始时间: $START_TS"
echo "结束时间: $END_TS"
echo "编译耗时: ${MINUTES}分${SECONDS}秒"

# 检查编译结果
if [ $BUILD_EXIT_CODE -ne 0 ]; then
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}  编译失败 (退出码: $BUILD_EXIT_CODE)${NC}"
    echo -e "${RED}========================================${NC}"
    echo ""
    echo "错误摘要："
    grep -i "error\|failed" "$LOG_FILE" | tail -5
    echo ""
    echo "日志文件: $LOG_FILE"

    # 写入日志尾部
    {
        echo ""
        echo "========================================"
        echo "编译失败: $END_TS"
        echo "编译耗时: ${MINUTES}分${SECONDS}秒"
        echo "退出码: $BUILD_EXIT_CODE"
        echo "========================================"
    } >> "$LOG_FILE"

    exit $BUILD_EXIT_CODE
fi

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  编译成功${NC}"
echo -e "${GREEN}========================================${NC}"

# 内存使用摘要
if [ -f "$SCRIPT_DIR/build/app.map" ]; then
    echo ""
    echo -e "${YELLOW}[MEMORY] 内存使用摘要:${NC}"
    IDF_PATH="$IDF_PATH" bash -c "
        source '$IDF_PATH/export.sh' 2>/dev/null
        idf_size.py '$SCRIPT_DIR/build/app.map'
    "
fi

# 固件信息
if [ -f "$SCRIPT_DIR/build/app.bin" ]; then
    SIZE=$(stat -c%s "$SCRIPT_DIR/build/app.bin" 2>/dev/null || stat -f%z "$SCRIPT_DIR/build/app.bin" 2>/dev/null)
    SIZE_KB=$((SIZE / 1024))
    echo "固件大小: ${SIZE_KB}KB ($SIZE bytes)"

    # 拷贝固件到 output 目录
    echo ""
    echo -e "${YELLOW}[OUTPUT] 拷贝固件到 output 目录...${NC}"
    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"

    cp "$SCRIPT_DIR/build/bootloader/bootloader.bin" "$OUTPUT_DIR/"
    cp "$SCRIPT_DIR/build/partition_table/partition-table.bin" "$OUTPUT_DIR/"
    cp "$SCRIPT_DIR/build/ota_data_initial.bin" "$OUTPUT_DIR/"
    cp "$SCRIPT_DIR/build/app.bin" "$OUTPUT_DIR/"
    cp "$SCRIPT_DIR/partitions.csv" "$OUTPUT_DIR/"

    # 生成下载说明
    cat > "$OUTPUT_DIR/README.txt" << EOF
PPG Monitor 固件 - ESP32-S3
编译时间: $END_TS
固件大小: ${SIZE_KB}KB ($SIZE bytes)

Flash Download Tool 配置:
- SPI Speed: 80MHz
- SPI Mode: DIO
- Flash Size: 16MB

文件下载地址:
- bootloader.bin        -> 0x0
- partition-table.bin   -> 0x8000
- ota_data_initial.bin  -> 0x11000
- app.bin               -> 0x20000

命令行下载:
esptool --chip esp32s3 --flash_mode dio --flash_size 16MB --flash_freq 80m \\
  write_flash 0x0 bootloader.bin 0x8000 partition-table.bin \\
  0x11000 ota_data_initial.bin 0x20000 app.bin
EOF

    echo -e "${GREEN}固件已拷贝到: $OUTPUT_DIR${NC}"
    ls -lh "$OUTPUT_DIR/"
fi

echo ""
echo "日志文件: $LOG_FILE"

# 写入日志尾部
{
    echo ""
    echo "========================================"
    echo "编译成功: $END_TS"
    echo "编译耗时: ${MINUTES}分${SECONDS}秒"
    if [ -f "$SCRIPT_DIR/build/app.bin" ]; then
        echo "固件大小: ${SIZE_KB}KB ($SIZE bytes)"
    fi
    echo "========================================"
} >> "$LOG_FILE"
