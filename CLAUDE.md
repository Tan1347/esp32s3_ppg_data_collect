# CLAUDE.md — AI 协作指南

## 项目背景

ESP32-S3 PPG 信号采集固件，使用 ESP-IDF v6.0.1 开发。
Xtensa LX7 双核处理器，240MHz，16MB Flash，8MB PSRAM。

## 关键约束

1. **编译环境**: Ubuntu 24.04，ESP-IDF v6.0.1 安装于 `~/esp/esp-idf-v6.0.1`
2. **Deep-sleep 唤醒**: 使用 GPIO12 (MAX30102_INT)，S3 支持任意 RTC GPIO 唤醒
3. **代码必须英文**: 所有日志、注释、变量名必须使用英文
4. **Bootloader UART**: 使用 `CONFIG_ESP_CONSOLE_UART_CUSTOM` 模式，否则波特率配置会被忽略
5. **CodeGraph 优先**: 项目有 `.codegraph/` 索引，查找函数定义、调用关系、源码时优先使用 `codegraph_codegraph_explore`，减少 grep/read 开销

## 栈溢出约束

picolibc `vfprintf` 内部使用约 8KB 栈空间。在 32KB 主任务栈上：

**安全**:
- `puts("message")` — 不经过 vsnprintf
- `PPG_LOGI(TAG, func, line, "msg")` — 静态缓冲

**危险**:
- `printf("format %d", value)` — 调用 vsnprintf
- `esp_log_set_vprintf(handler)` — 所有 ESP_LOG 走 vsnprintf

## 编译命令

```bash
cd esp32s3
bash build.sh
# 或清理后全量编译:
bash build.sh clean
```

**重要**: 运行在 Ubuntu server 上，无法直连物理串口，**禁止尝试烧录固件**。编译成功后固件自动拷贝到 `output/` 目录，用户自行烧录。

## 串口配置

| 串口 | GPIO | 功能 | 波特率 |
|------|------|------|--------|
| UART0 | 43(TX)/44(RX) | 日志输出（USB串口芯片） | 1000000 |
| UART2 | 6(TX)/7(RX) | APP配置接口 | 1000000 |

## GPIO 分配

| GPIO | 功能 | 说明 |
|------|------|------|
| 0 | BOOT | 启动按钮（已配置内部上拉） |
| 4 | I2C_SCL | MAX30102时钟 |
| 5 | I2C_SDA | MAX30102数据 |
| 6 | UART2_TX | APP配置接口（MCU→APP） |
| 7 | UART2_RX | APP配置接口（APP→MCU） |
| 8 | BATT_ADC | 电池电压 (ADC1_CH7) |
| 10 | SYS_LED | 系统状态灯 |
| 11 | PPG_LED | 采集状态灯 |
| 12 | MAX_INT | MAX30102中断/Deep-sleep唤醒 |
| 13 | SD_SPI_CLK | TF卡时钟 |
| 14 | SD_SPI_CS | TF卡片选 |
| 16 | SD_SPI_MISO | TF卡数据 |
| 17 | SD_SPI_MOSI | TF卡数据 |
| 18 | BUTTON1 | 用户按钮（低电平有效） |
| 30-35 | Flash | 外挂Flash (GD25LQ128CVIG 16MB) |
| 43 | UART0_TX | 日志输出（USB串口芯片） |
| 44 | UART0_RX | 日志输入（USB串口芯片） |

## 按钮功能

### BUTTON1 (GPIO18)

| 操作 | 效果 |
|------|------|
| 上电时按住 | 进入 BLE 配对模式（快速连接） |
| 长按 ≥3秒 | 进入 BLE 配对模式 |
| 双击 | 进入 WiFi 模式 |
| 单击 | 切换 STANDALONE ↔ MEASURING |

### BOOT (GPIO0)

- 已配置内部上拉，防止浮空导致进入下载模式
- 仅用于芯片下载模式（按住 BOOT + 按 RST）

## 启动流程

```
1. Bootloader (115200 baud)
2. App 启动 (切换到 1000000 baud)
3. 显示版本信息
4. 系统初始化 (NVS → Netif → Log → Power → Battery → SD → MAX30102 → OTA → UART)
5. 上电时挂载 TF 卡并打印占用情况
6. 检查 BUTTON1 状态决定启动模式
7. 进入对应工作模式
```

## 宏开关

```c
// ppg_config.h
#define BATTERY_CHECK_ENABLE 0  // 电池低电检查 (0=禁用, 1=启用)

// components/ble_svc/ble_svc.c
#define BLE_DEBUG_ENABLE    1   // BLE 调试日志 (0=关闭, 1=开启)
```

## BLE 帧协议

```
[0xAA][CMD][LEN][DATA...][CHECKSUM]
CHECKSUM = SUM(CMD + LEN + DATA) & 0xFF
```

## SPI 时钟策略

- TF 卡初始化：400kHz
- 正常工作：20MHz

## 注意事项

- Bootloader 阶段波特率固定 115200，进入 App 后切换到 1000000
- sdkconfig 使用 `CONFIG_ESP_CONSOLE_UART_CUSTOM` 模式指定 GPIO43/44
- 上电时会自动挂载 TF 卡并打印空间占用
- BLE cmd 0x22 (查询状态) 仅通过 0xFFF1 通知发送，不发送 0xFFF3 响应
