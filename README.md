# ESP32-S3 PPG 信号采集固件

> **固件版本**: 1.0.5  
> **编译环境**: Ubuntu 24.04 + ESP-IDF v6.0.1  
> **当前状态**: 编译通过，硬件实测中  
> **Flash 配置**: DIO 80MHz，16MB Flash，8MB PSRAM

---

## 项目概述

基于 ESP-IDF v6.0.1 的 PPG（光电容积脉搏波）信号采集固件，运行于 **ESP32-S3** 平台（Xtensa LX7 双核 240MHz）。集成 MAX30102 传感器驱动、心率/血氧算法、BLE 通信、WiFi 数据传输、TF 卡本地存储、OTA 升级等功能。

### 核心特性

| 模块 | 说明 |
|------|------|
| MAX30102 | I2C 驱动，100Hz 采样率，中断驱动 |
| PPG 算法 | 纯定点运算，5 秒滑动窗口，峰值检测 + Hamming 滤波 |
| BLE | NimBLE 协议栈，帧协议通信（0xAA 帧头 + SUM 校验） |
| WiFi | Station 模式，HTTP Server，文件传输 + Web 管理 + OTA |
| TF 卡 | FAT32 + LZ4 压缩，主备缓冲 (16KB+4KB)，SPI 400kHz→20MHz |
| UART 录制 | 双缓冲 DMA (2x32KB)，支持 9600~5Mbps 波特率 |
| DHT11 | 温湿度监测，二进制格式存储 |
| OTA | 双分区 A/B，SHA-256 校验，60 秒启动自检，失败自动回滚 |
| 电源 | DFS 动态调频，Auto Light-sleep，Deep-sleep，电池电量保护（可禁用） |

---

## 硬件规格

| 参数 | 值 |
|------|-----|
| 芯片 | ESP32-S3 (Xtensa LX7 双核) |
| CPU 频率 | 240MHz |
| Flash | 16MB (GD25LQ128CVIG，GPIO30-35) |
| PSRAM | 8MB Octal (片内) |
| Flash 模式 | DIO 80MHz |

---

## 硬件引脚

| 功能 | GPIO | 说明 |
|------|------|------|
| BOOT | GPIO0 | 启动按钮（已配置内部上拉，防止误触发下载模式） |
| I2C_SCL | GPIO4 | MAX30102 时钟线 |
| I2C_SDA | GPIO5 | MAX30102 数据线 |
| UART2_TX | GPIO6 | APP 配置接口（MCU→APP，1M 波特率） |
| UART2_RX | GPIO7 | APP 配置接口（APP→MCU，1M 波特率） |
| BATT_ADC | GPIO8 | 电池电压检测 (ADC1_CH7) |
| SYS_LED | GPIO10 | 系统工作状态灯 |
| PPG_LED | GPIO11 | PPG 采集状态灯 |
| MAX_INT | GPIO12 | MAX30102 中断输出 (低电平有效, Deep-sleep 唤醒) |
| SD_SPI_CLK | GPIO13 | TF 卡 SPI 时钟 |
| SD_SPI_CS | GPIO14 | TF 卡片选 |
| DHT11 | GPIO15 | 温湿度传感器 |
| SD_SPI_MISO | GPIO16 | TF 卡 SPI 主入 |
| SD_SPI_MOSI | GPIO17 | TF 卡 SPI 主出 |
| BUTTON1 | GPIO18 | 用户按钮 (单击=切换模式, 双击=WiFi, 长按=BLE) |
| EXT_FLASH | GPIO30-35 | 外挂 Flash (GD25LQ128CVIG 16MB，硬件占用) |
| UART0_TX | GPIO43 | 调试串口输出（USB 串口芯片，1M 波特率） |
| UART0_RX | GPIO44 | 调试串口输入（USB 串口芯片，1M 波特率） |

### 串口配置

| 串口 | GPIO | 功能 | 波特率 |
|------|------|------|--------|
| UART0 | 43(TX)/44(RX) | 日志输出（USB 串口芯片） | 1000000 |
| UART2 | 6(TX)/7(RX) | APP 配置接口 | 1000000 |

**注意**: Bootloader 阶段使用 115200 波特率，进入 App 后切换到 1000000。

---

## 目录结构

```
esp32s3/
├── CMakeLists.txt              # 项目根配置
├── partitions.csv              # Flash 分区表 (16MB)
├── sdkconfig.defaults          # SDK 默认配置
├── build.sh                    # 编译脚本 (含内存使用摘要)
├── CLAUDE.md                   # AI 协作指南
├── main/
│   └── main.c                  # 入口、状态机、常驻任务
├── components/
│   ├── ppg_config/             # 全局配置 (引脚, BLE命令, 系统状态)
│   ├── ble_svc/                # BLE GATT 服务 + 调试日志
│   ├── max30102/               # MAX30102 I2C 驱动
│   ├── ppg_algo/               # PPG 算法 (定点运算, 无FPU)
│   ├── sd_storage/             # TF 卡存储 (FAT32, 二进制格式)
│   ├── wifi_prov/              # WiFi 配网 (NVS, 自动连接)
│   ├── wifi_transfer/          # HTTP 服务器 (文件下载, OTA上传)
│   ├── battery/                # 电池 ADC 检测 + 电量计算
│   ├── dht11/                  # DHT11 温湿度驱动
│   ├── ota_upgrade/            # OTA 升级 (双分区, SHA-256)
│   ├── power_mgmt/             # 电源管理 (DFS, Deep-sleep)
│   ├── ppg_log/                # 异步日志系统 (环形缓冲, 文件输出)
│   ├── uart_recorder/          # UART 串口录制
│   └── compress/               # LZ4 压缩库
└── output/                     # 编译输出 (bootloader, partition-table, app.bin)
```

---

## 系统状态机

```
冷启动 ──> STATE_BLE_PAIRING (默认启动BLE，方便快速连接)
              │
              ├─ BLE 连接成功 ──> STATE_BLE_CONNECTED
              │                      │
              │                 接收命令 (0x10 添加WiFi, 0x01 开始测量等)
              │                      │
              │                      ▼
              │                 STATE_WIFI_STA (自动连接，60秒超时)
              │                      │
              │                      ▼
              │                 STATE_DEEP_SLEEP
              │
              └─ 超时 ──> STATE_DEEP_SLEEP
                              │
                         GPIO12 唤醒 (MAX30102 中断)
                              │
                              ▼
                         STATE_STANDALONE (独立采集)
                              │
                         30秒活跃后进入 Light-sleep 循环
                              │
                         5分钟无中断 ──> STATE_DEEP_SLEEP
```

**WiFi 连接后自动时间同步**：固件通过 HTTP 获取网络时间戳。

---

## 按钮功能

### BUTTON1 (GPIO18)

| 操作 | 功能 |
|------|------|
| 上电时按住 | 进入 BLE 配对模式（快速连接） |
| 单击 | 切换 STANDALONE / MEASURING 模式 |
| 双击 | 进入 WiFi 模式 |
| 长按 3秒 | 进入 BLE 配对模式 |

### BOOT (GPIO0)

| 操作 | 功能 |
|------|------|
| 按住 BOOT + 按 RST | 进入下载模式（烧录固件） |

**注意**: BOOT 按钮已配置内部上拉，防止浮空导致意外进入下载模式。

---

## BLE 通信协议

### GATT 服务

```
Service UUID: 0000fff0-0000-1000-8000-00805f9b34fb
├── 0xFFF1: Status      (Read/Notify) - 20字节 设备状态
├── 0xFFF2: Live Data   (Notify)      - 5字节 PPG数据
├── 0xFFF3: Command     (Read/Write/Notify) - 帧协议命令
└── 0xFFF4: File List   (Read)         - TF卡文件列表 JSON
```

### 帧协议

```
请求帧: [0xAA][CMD][LEN][DATA...][CHECKSUM]
响应帧: [0xAA][CMD][0x01][STATUS][CHECKSUM]
数据帧: [0xAA][CMD][LEN][DATA...][CHECKSUM]

CHECKSUM = SUM(CMD + LEN + DATA 各字节) & 0xFF
```

**状态码**: 0=OK, 1=取消, 2=校验错误, 3=未知命令, 4=电量不足

### 命令表

| CMD | 名称 | 数据 | 响应 | 说明 |
|-----|------|------|------|------|
| 0x01 | START_MEASURE | - | OK | 开始 PPG 采集 |
| 0x02 | STOP_MEASURE | - | OK | 停止 PPG 采集 |
| 0x03 | START_WIFI | - | OK / 0x04 | 启动 WiFi (电量>=20%) |
| 0x10 | WIFI_ADD | SSID_LEN(2B)+SSID+PWD_LEN(2B)+PWD | OK | 添加 WiFi，自动连接 |
| 0x11 | WIFI_STATUS | - | OK | 查询 WiFi 状态 |
| 0x12 | WIFI_CLEAR | - | OK | 清除所有已保存 WiFi |
| 0x13 | WIFI_DELETE | index(1B) | OK | 按索引删除 WiFi |
| 0x14 | WIFI_LIST | - | JSON via 0xFFF4 | 获取已保存 WiFi 列表 |
| 0x20 | OTA_ENTER | - | OK | 进入 OTA 模式 |
| 0x21 | FW_VERSION | - | version via 0xFFF1 Notify | 获取固件版本 |
| 0x22 | QUERY_STATUS | - | Notify (0xFFF1) | 刷新状态数据（仅通知，不发送响应） |
| 0x23 | QUERY_SD_CARD | - | Data(4B) | 获取 SD 卡空间 |
| 0x24 | QUERY_BATTERY | - | Data(1B) | 获取电池电量百分比 |
| 0x30 | LOG_LEVEL | level(1B) | OK | 设置日志级别 |
| 0x31 | LOG_STATUS | - | OK | 查询日志状态 |
| 0x32 | FILE_DOWNLOAD | - | IP string | BLE 触发 WiFi，返回设备 IP |
| 0x40 | TIME_SYNC | timestamp(4B) | OK | 同步 Unix 时间戳 |
| 0x41 | STANDALONE | - | OK | 进入独立采集模式 |
| 0x50 | UART_RECORD | enable(1)+baud(4B) | OK | 串口记录控制 |

### Status 特征值 (0xFFF1) - 20 字节

```
偏移  长度  字段       类型    说明
0     1     batt_pct   uint8   电池电量百分比 (0-100)
1-2   2     voltage    uint16  电压 (big-endian, ×100 mV)
3     1     reserved   uint8   保留 (0x00)
4     1     connected  uint8   BLE 连接状态 (0=断开, 1=已连接)
5-19  15    version    char[]  固件版本字符串 (UTF-8, 空字符填充)
```

---

## 编译与烧录

### 编译

```bash
cd esp32s3
source ~/esp/esp-idf-v6.0.1/export.sh
idf.py build          # 增量编译
```

或使用脚本：
```bash
bash build.sh         # 增量编译
bash build.sh clean   # 清理后全量编译
```

### 烧录

```bash
idf.py -p /dev/ttyUSB0 flash
```

或使用 esptool：
```bash
esptool --chip esp32s3 --flash_mode dio --flash_size 16MB --flash_freq 80m \
  write_flash 0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x11000 build/ota_data_initial.bin \
  0x20000 build/app.bin
```

### 串口监控

```bash
idf.py -p /dev/ttyUSB0 monitor
```

**波特率说明**:
- Bootloader: 115200
- App: 1000000

### 分区表

| 名称 | 类型 | 子类型 | 偏移 | 大小 |
|------|------|--------|------|------|
| nvs | data | nvs | 0x9000 | 0x8000 (32KB) |
| otadata | data | ota | 0x11000 | 0x2000 (8KB) |
| app0 | app | ota_0 | 0x20000 | 0x1E0000 (1.875MB) |
| app1 | app | ota_1 | 0x200000 | 0x1E0000 (1.875MB) |

---

## HTTP API

WiFi 连接后通过局域网 IP 访问。

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/status` | GET | 设备状态 JSON |
| `/api/files` | GET | TF 卡文件列表 JSON |
| `/api/download?file=xxx` | GET | 下载文件 |
| `/api/ota` | GET | OTA 升级页面 |
| `/api/ota/info` | GET | OTA 分区信息 JSON |
| `/api/ota` | POST | 上传固件执行 OTA |
| `/api/logs` | GET | 日志文件列表 |
| `/api/logs/download?file=xxx` | GET | 下载日志 |
| `/api/shutdown` | POST | 关闭 WiFi |

---

## LED 指示灯

| LED | GPIO | 行为 |
|-----|------|------|
| SYS_LED | GPIO10 | 每 1 秒翻转 (系统运行) |
| PPG_LED | GPIO11 | 闪烁频率随 PPG 数据率变化 |

---

## 电源优化

| 模式 | CPU 频率 | 说明 |
|------|----------|------|
| Active | 240MHz | BLE/WiFi/采集时全速运行 |
| Light-sleep | 10MHz | 独立采集模式，定时器每 1 秒唤醒 |
| Deep-sleep | - | 仅 GPIO12 唤醒，约 5uA |

**WiFi 优化**：
- WiFi 缓冲区配置：RX=6, Dynamic RX=10, TX=6
- 指数退避重连
- PMF/WPA3 兼容

---

## 数据存储

### 文件目录结构

```
/sdcard/
├── raw/                    # 原始 PPG 数据
│   └── 20260705_120000.bin.lz4
├── csv/                    # 心率/血氧结果
│   └── 20260705.csv
├── env/                    # DHT11 温湿度数据
│   └── 20260705.env
└── log/                    # 运行日志
    └── 20260705_123456.log
```

### SPI 时钟策略

| 阶段 | 时钟 | 说明 |
|------|------|------|
| 初始化 | 400kHz | 卡初始化 |
| 正常操作 | 20MHz | 正常读写 |

---

## PPG 算法

全定点运算，无 FPU 依赖。

| 算法 | 方法 | 代码大小 |
|------|------|----------|
| 心率 (HR) | 时域峰值检测（滑动窗口 + 自适应阈值） | 约 5KB |
| 血氧 (SpO2) | R/IR 比值，DC/AC 分量，查找表校准 | 约 3KB |
| 灌注指数 (PI) | AC/DC × 100% | 约 1KB |
| 带通滤波器 | IIR 带通 0.5-4Hz | 约 2KB |

---

## FreeRTOS 任务设计

**常驻任务**（始终运行）：

| 任务 | 优先级 | 栈大小 | 说明 |
|------|--------|--------|------|
| sys_led_task | 1 | 3KB | GPIO10 系统心跳灯 |
| ppg_led_task | 1 | 2KB | GPIO11 PPG 状态灯 |
| button1_task | 2 | 2KB | BUTTON1 按钮检测 |

**采集任务**（按需创建/销毁）：

| 任务 | 优先级 | 栈大小 | 说明 |
|------|--------|--------|------|
| ppg_task | 5 | 4KB | I2C 读取 MAX30102 + 算法处理 |
| power_task | 1 | 2KB | ADC 电池电压监控 |
| dht11_task | 2 | 2KB | DHT11 温湿度采集 |

**BLE 任务**（BLE 初始化时创建）：

| 任务 | 优先级 | 栈大小 | 说明 |
|------|--------|--------|------|
| ble_cmd_task | 3 | 4KB | BLE 命令处理 |

---

## 开发规范

### 模块解耦

- **组件独立**：每个组件独立编译，通过头文件公开接口
- **依赖方向**：上层模块依赖下层模块，禁止反向依赖
- **配置集中管理**：所有引脚、常量、阈值集中在 `ppg_config.h`

### 内存安全

- **malloc/free 配对**：所有动态分配必须检查返回值
- **缓冲区边界检查**：用 `snprintf` 替代 `sprintf`
- **栈溢出防护**：FreeRTOS 任务栈大小留 20% 余量

### 并发安全

- **锁顺序一致**：多把锁场景下，所有任务必须按相同顺序获取锁
- **持锁时间最短**：锁内只做数据拷贝和状态更新
- **超时获取锁**：优先使用 `xSemaphoreTake(mutex, timeout)`

### 命名规范

- 私有函数/变量：`s_` 前缀 (如 `s_initialized`)
- 公共 API 函数：组件名开头 (如 `max30102_init`)

---

## 版本管理

固件版本通过 `version.txt` 文件管理：

```bash
# 查看当前版本
cat version.txt

# 设置新版本
echo "1.0.5" > version.txt
```

## CI/CD

推送到 `master` 分支会自动触发 GitHub Actions：
1. 使用 ESP-IDF v6.0.1 Docker 容器编译
2. 读取 version.txt 生成版本号
3. 打包为 7z 压缩包
4. 创建 GitHub Release

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.0.0 | 2026-06-19 | 初始版本 (ESP32-C3) |
| 1.0.1 | 2026-06-28 | 修复 WDT 核心转储、WiFi 内存泄露、BLE 状态查询超时 |
| 1.0.2 | 2026-07-04 | ESP32-S3 迁移开始 |
| 1.0.3 | 2026-07-05 | 修复 UART 波特率配置（CUSTOM 模式）、GPIO0 上拉 |
| 1.0.4 | 2026-07-05 | 上电挂载 TF 卡并打印占用、修复 BLE cmd 0x22 双重响应 |
| 1.0.5 | 2026-07-05 | 更新文档、Bootloader UART 配置修复 |
