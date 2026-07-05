# ESP32-C3 → ESP32-S3 迁移重构计划

## 一、目标平台信息

| 参数 | ESP32-C3 | ESP32-S3 (合宙核心板) |
|------|----------|----------------------|
| CPU | RISC-V 160MHz | Xtensa LX7 双核 240MHz |
| Flash | 4MB | 16MB (GD25LQ128CVIG, GPIO30-35) |
| PSRAM | 无 | 8MB 合封（内部连接，不占用 GPIO） |
| RAM | 400KB | 512KB |
| GPIO 总数 | 22 (GPIO0-21) | 45 (GPIO0-48) |
| Deep-sleep 唤醒 | 仅 GPIO0-5 | 任意 RTC GPIO |
| USB | 无 | USB-OTG (GPIO19-20) |
| SPI Flash | GPIO14-17 | GPIO30-35 |
| 摄像头接口 | 无 | 有 (DVP/CSI) |
| AI 加速器 | 无 | 向量指令 |

## 二、GPIO 引脚映射

根据 `esp32c3/pin_map.csv` 中的映射关系：

| 功能 | ESP32-C3 GPIO | ESP32-S3 GPIO | 备注 |
|------|---------------|---------------|------|
| 32K晶振 XTAL_32K_P | 0 | 1 | 晶振引脚 |
| 32K晶振 XTAL_32K_N | 1 | 2 | 晶振引脚 |
| BUTTON1 用户按钮 | 2 | 18 | 低电平有效 |
| SD_SPI_MOSI | 3 | 17 | TF 卡数据 |
| BATT_ADC 电池电压 | 4 | **8** | ADC1_CH7（WiFi 兼容） |
| MAX_INT 中断/唤醒 | 5 | **12** | RTC GPIO，Deep-sleep 唤醒源 |
| DHT11 温湿度 | 6 | 15 | |
| SD_SPI_CS | 7 | 14 | TF 卡片选 |
| Card_CD 检测 | 8 | **不使用** | C3 时已禁用 |
| BOOT 启动按钮 | 9 | 0 | |
| SD_SPI_MISO | 10 | 16 | TF 卡数据 |
| SD_SPI_CLK | 11 | 13 | TF 卡时钟 |
| PPG_LED 采集灯 | 12 | **不使用** | S3 无此 LED |
| SYS_LED 系统灯 | 13 | **10** | 用户确认 |
| EXT_FLASH | 14-17 | GPIO30-35 | S3 内部 Flash SPI |
| I2C_SCL | 18 | 4 | MAX30102 时钟 |
| I2C_SDA | 19 | 5 | MAX30102 数据 |
| UART0_RX | 20 | 6 | 调试串口 |
| UART0_TX | 21 | 7 | 调试串口 |

### ⚠️ 引脚冲突问题

无冲突。用户已合理分配：
- PPG_LED → 不使用（S3 板无此 LED）
- SYS_LED → GPIO 10（原 PPG_LED 位置）
- BATT_ADC → GPIO 8（空闲 RTC GPIO，ADC1_CH7，WiFi 兼容）
- MAX_INT → GPIO 12（空闲 RTC GPIO，Deep-sleep 唤醒）

### ESP32-S3 已占用 GPIO 汇总

| GPIO | 功能 |
|------|------|
| 0 | BOOT |
| 1 | 32K晶振 P |
| 2 | 32K晶振 N |
| 4 | I2C_SCL |
| 5 | I2C_SDA |
| 6 | UART0_RX |
| 7 | UART0_TX |
| 8 | BATT_ADC |
| 10 | SYS_LED |
| 12 | MAX_INT |
| 13 | SD_SPI_CLK |
| 14 | SD_SPI_CS |
| 15 | DHT11 |
| 16 | SD_SPI_MISO |
| 17 | SD_SPI_MOSI |
| 18 | BUTTON1 |
| 30-35 | Flash (GD25LQ128CVIG) |

## 三、需要修改的文件清单

### 3.1 配置文件

| 文件 | 修改内容 | 优先级 |
|------|----------|--------|
| `sdkconfig.defaults` | TARGET=esp32s3, Flash=16MB, UART 引脚, Flash 引脚, PSRAM 启用, FreeRTOS 栈增大 | P0 |
| `partitions.csv` | Flash 偏移地址调整（16MB Flash），增加 PSRAM 分区（可选） | P0 |
| `CMakeLists.txt` | 无需修改（已通用） | - |

### 3.2 引脚定义

| 文件 | 修改内容 | 优先级 |
|------|----------|--------|
| `components/ppg_config/include/ppg_config.h` | 所有 GPIO_NUM_xxx 引脚号更换为 S3 映射值 | P0 |
| `components/ppg_config/include/ppg_config.h` | BATTERY_ADC_CHANNEL 需确认 S3 ADC 通道映射 | P0 |
| `components/ppg_config/include/ppg_config.h` | Deep-sleep 唤醒注释更新（S3 无 GPIO0-5 限制） | P1 |

### 3.3 代码适配

| 文件 | 修改内容 | 优先级 |
|------|----------|--------|
| `components/battery/` | ADC 通道/atten API 适配（S3 ADC driver 可能有差异） | P0 |
| `components/sd_storage/` | SPI host 可能需要调整（S3 SPI2/SPI3 均可用） | P1 |
| `components/wifi_prov/` | 无需修改（WiFi API 通用） | - |
| `components/ble_svc/` | 无需修改（BLE API 通用） | - |
| `components/max30102/` | 无需修改（I2C API 通用） | - |
| `components/dht11/` | 无需修改（GPIO API 通用） | - |
| `components/uart_recorder/` | UART 引脚可能需要调整 | P1 |
| `components/power_mgmt/` | Deep-sleep 唤醒源配置更新 | P0 |
| `main/main.c` | 唤醒 GPIO 配置更新，LED 引脚更新 | P0 |

### 3.4 构建与 CI

| 文件 | 修改内容 | 优先级 |
|------|----------|--------|
| `build.sh` | 芯片名称、Flash 大小、烧录参数更新 | P0 |
| `.github/workflows/build.yml` | IDF_TARGET, ccache key, Flash 参数, release 文件名 | P0 |
| `CLAUDE.md` | GPIO 分配表、烧录命令、编译命令更新 | P1 |

### 3.5 文档

| 文件 | 修改内容 | 优先级 |
|------|----------|--------|
| `README.md` | 平台信息更新 | P2 |
| `快速开始.md` | 烧录命令更新 | P2 |
| `架构分析.md` | 无需修改 | - |
| `内存占用情况分析.md` | S3 内存更大，需重新分析 | P2 |

## 四、关键技术差异与适配要点

### 4.1 Deep-sleep 唤醒

**ESP32-C3**: 仅 GPIO0-GPIO5 可作为唤醒源
**ESP32-S3**: 任意 RTC GPIO 均可作为唤醒源

当前使用 GPIO5 (MAX30102_INT) 唤醒。迁移到 S3 后使用 GPIO10，需要确认 GPIO10 是否为 RTC GPIO（ESP32-S3 的 GPIO0-21 均为 RTC GPIO，GPIO10 可用）。

### 4.2 ADC 电池检测

ESP32-S3 的 ADC 通道映射与 C3 不同：
- C3: GPIO4 = ADC_CHANNEL_4
- S3: GPIO11 = ADC_CHANNEL_2 (需确认)

需要查看 ESP32-S3 Technical Reference Manual 确认 GPIO11 对应的 ADC 通道。

### 4.3 Flash 与 PSRAM

- **Flash**: 从 4MB 升级到 16MB，分区表偏移地址不变（前 4MB 布局相同），但 OTA 分区可以更大
- **PSRAM**: 8MB 合封 PSRAM 内部连接，不占用 GPIO。可在 sdkconfig 中启用：
  ```
  CONFIG_ESP32S3_SPIRAM_SUPPORT=y
  CONFIG_SPIRAM_MODE_QUAD=y
  ```

### 4.4 电源管理

ESP32-S3 支持更精细的电源管理：
- 可以独立关闭 CPU0/CPU1
- Light-sleep 功耗可能更低
- 需要确认 `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` 在 S3 上的行为

### 4.5 BLE

NimBLE 在 ESP32-S3 上的行为与 C3 一致，无需修改。但 S3 支持 BLE 5.0，可后续利用。

### 4.6 WiFi

ESP32-S3 的 WiFi 功能与 C3 一致，无需修改代码。但 S3 支持 WiFi 6 (802.11ax)，可后续启用。

## 五、执行步骤

### Phase 1: 基础适配（优先级 P0）

1. 复制 esp32c3 工程到 esp32s3 目录
2. 修改 `sdkconfig.defaults`：TARGET=esp32s3, Flash=16MB, UART 引脚(6/7), PSRAM
3. 修改 `ppg_config.h`：所有 GPIO 引脚号（见映射表）
4. 删除 PPG_LED 宏定义（S3 不使用），SYS_LED → GPIO 10
5. BATT_ADC → GPIO 8（ADC1_CH7），MAX_INT → GPIO 12
6. 确认 ADC 通道：GPIO8 = ADC1_CH7，更新 `BATTERY_ADC_CHANNEL`
7. 修改 `partitions.csv`（如需调整 OTA 分区大小）
8. 修改 `build.sh`：芯片名称、Flash 参数
9. 编译测试

### Phase 2: 代码适配（优先级 P1）

9. ADC 驱动 API 适配（如 S3 ADC driver 有变化）
10. Deep-sleep 唤醒配置更新
11. UART 引脚确认
12. SPI TF 卡引脚确认

### Phase 3: CI/CD 与文档（优先级 P2）

13. 修改 `.github/workflows/build.yml`
14. 更新 `CLAUDE.md`
15. 更新 `README.md` 和其他文档
16. 编译烧录测试验证

## 六、风险与注意事项

1. **ADC 通道映射**: ESP32-S3 的 ADC 通道与 GPIO 的对应关系与 C3 不同，必须查阅 datasheet 确认
2. **SPI Flash 引脚**: S3 的 SPI Flash 使用 GPIO30-35，不可用于其他功能
3. **PSRAM 初始化**: 启用 PSRAM 后需要确认内存分配策略
4. **栈大小**: S3 有更多 RAM，但双核调度可能需要调整任务栈大小
5. **编译器**: S3 使用 Xtensa 工具链（非 RISC-V），编译命令不变但底层工具链不同
6. **Deep-sleep 唤醒源**: 虽然 S3 支持任意 RTC GPIO 唤醒，但需要确认唤醒后的 GPIO 状态
