/**
 * @file ppg_config.h
 * @brief Global configuration — pins, constants, version (ESP32-S3 target)
 */

#pragma once

#include <stdint.h>

/* ==================== Firmware Version ==================== */
#include "version.h"
#include "version_ts.h"

/* ==================== System State ==================== */
typedef enum {
    STATE_DEEP_SLEEP = 0,
    STATE_LIGHT_SLEEP,
    STATE_MEASURING,
    STATE_BLE_CONNECTED,
    STATE_BLE_PAIRING,
    STATE_WIFI_STA,
    STATE_OTA,
    STATE_WIFI_SHUTDOWN,
    STATE_STANDALONE,
} system_state_t;

/* ==================== I2C Pins (S3: GPIO4/5) ==================== */
#define PPG_I2C_PORT            I2C_NUM_0
#define PPG_I2C_SDA_PIN         GPIO_NUM_5
#define PPG_I2C_SCL_PIN         GPIO_NUM_4
#define PPG_I2C_FREQ_HZ        400000  /* 400kHz Fast-mode */

/* ==================== MAX30102 ==================== */
#define MAX30102_I2C_ADDR       0x57
#define MAX30102_INT_PIN        GPIO_NUM_12  /* Deep-sleep wake source (RTC GPIO) */
#define PPG_SAMPLE_RATE_HZ      100
#define PPG_SAMPLE_INTERVAL_MS  (1000 / PPG_SAMPLE_RATE_HZ)

/* ==================== ADC Battery (S3: GPIO8 = ADC1_CH7) ==================== */
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_7   /* GPIO8 on ESP32-S3 */
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BATTERY_DIVIDER_RATIO   2       /* 100K + 100K voltage divider */
#define BATTERY_FILTER_CAP_NF   100     /* 100nF filter cap */
#define BATTERY_SAMPLE_COUNT    64
#define BATTERY_LOW_VOLTAGE     330     /* 3.3V low threshold */
#define BATTERY_SHUTDOWN_VOLTAGE 320    /* 3.2V forced shutdown */
#define BATTERY_WIFI_MIN_SOC    20      /* WiFi minimum 20% */
#define BATTERY_PPG_MIN_SOC     10      /* PPG minimum 10% */

/* Battery check switch: 1=enable, 0=disable (debug without battery) */
#define BATTERY_CHECK_ENABLE    0
#define PPG_DEBUG_ENABLE        1   /* PPG algo debug: print perfusion, quality, etc. */

/* ==================== Buttons ==================== */
#define BOOT_BUTTON_GPIO            GPIO_NUM_0   /* BOOT (S3: GPIO0) */
#define BUTTON1_GPIO                GPIO_NUM_18  /* User button (S3: GPIO18) */

/* ==================== Deep-sleep Wake ==================== */
/* ESP32-S3 supports any RTC GPIO (GPIO0-21) for deep-sleep wake */
/* MAX30102_INT_PIN (GPIO12) used for wake */

/* ==================== LED ==================== */
#define PPG_LED_PIN                 GPIO_NUM_11  /* PPG collection status LED */
#define SYS_LED_PIN                 GPIO_NUM_10  /* System status LED (1s blink) */

/* ==================== UART2 (S3: GPIO6-TX/7-RX, serial data capture) ==================== */
#define UART2_TX_PIN            6
#define UART2_RX_PIN            7

/* ==================== TF Card SPI (S3: GPIO14/13/17/16) ==================== */
#define SD_SPI_HOST             SPI2_HOST
#define SD_SPI_CS_PIN           GPIO_NUM_14
#define SD_SPI_CLK_PIN          GPIO_NUM_13
#define SD_SPI_MOSI_PIN         GPIO_NUM_17
#define SD_SPI_MISO_PIN         GPIO_NUM_16
#define SD_MOUNT_POINT          "/sdcard"

/* ==================== BLE ==================== */
#define BLE_DEVICE_NAME         "PPG-Monitor"
#define BLE_SVC_UUID            0xFFF0
#define BLE_CHAR_STATUS_UUID    0xFFF1
#define BLE_CHAR_LIVE_UUID      0xFFF2
#define BLE_CHAR_CMD_UUID       0xFFF3
#define BLE_CHAR_FILELIST_UUID  0xFFF4

/* BLE command definitions */
#define BLE_CMD_START_MEASURE   0x01
#define BLE_CMD_STOP_MEASURE    0x02
#define BLE_CMD_START_WIFI      0x03
#define BLE_CMD_WIFI_ADD        0x10
#define BLE_CMD_WIFI_STATUS     0x11
#define BLE_CMD_WIFI_CLEAR      0x12
#define BLE_CMD_WIFI_DELETE     0x13
#define BLE_CMD_WIFI_LIST       0x14
#define BLE_CMD_WIFI_DETAIL     0x15
#define BLE_CMD_OTA_ENTER       0x20
#define BLE_CMD_FW_VERSION      0x21
#define BLE_CMD_QUERY_STATUS    0x22
#define BLE_CMD_QUERY_SD_CARD   0x23
#define BLE_CMD_QUERY_BATTERY   0x24
#define BLE_CMD_TIME_SYNC       0x40
#define BLE_CMD_STANDALONE      0x41
#define BLE_CMD_UART_RECORD     0x50
#define BLE_CMD_LOG_LEVEL       0x30
#define BLE_CMD_LOG_STATUS      0x31
#define BLE_CMD_FILE_DOWNLOAD   0x32

/* ==================== WiFi ==================== */
#define WIFI_MAX_CREDENTIALS    5
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_HTTP_TIMEOUT_SEC   120

/* ==================== Log System ==================== */
#define LOG_RING_BUFFER_SIZE    (8 * 1024)  /* 8KB ring buffer (S3 has PSRAM) */
#define LOG_FLUSH_THRESHOLD     (LOG_RING_BUFFER_SIZE / 2)
#define LOG_FLUSH_INTERVAL_MS   30000
#define LOG_MAX_FILE_SIZE       (10 * 1024 * 1024)
#define LOG_MAX_FILES           5

#ifndef PPG_LOG_UART_ENABLE
#define PPG_LOG_UART_ENABLE     1
#endif

/* ==================== Power Management ==================== */
#define PM_MAX_FREQ_MHZ         240   /* S3 supports up to 240MHz */
#define PM_MIN_FREQ_MHZ         10
#define PM_LIGHT_SLEEP_ENABLE   false  /* Auto light-sleep disabled (tick conflict on S3) */

/* ==================== OTA ==================== */
#define OTA_BUFFER_SIZE         4096
#define OTA_ROLLBACK_ENABLE     true

/* ==================== Task Stack Sizes ==================== */
#define STACK_PPG_TASK          24576
#define STACK_BLE_CMD           4096
#define STACK_BLE_NOTIFY         4096  /* Dedicated BLE notification task */
#define STACK_SYS_LED           2048  /* Reduced: simple LED toggle + WDT feed */
#define STACK_PPG_LED           2048
#define STACK_BUTTON1           2048
#define STACK_POWER             2048
#define STACK_UART_WR           2048

/* ==================== Timeouts (ms) ==================== */
#define TIMEOUT_BLE_PAIR_WAKEUP     30000
#define TIMEOUT_BLE_PAIR_COLDBOOT   60000
#define TIMEOUT_WIFI_CONNECT        30000
#define TIMEOUT_HTTP_FETCH          5000
#define TIMEOUT_WIFI_IDLE           60000
#define TIMEOUT_STANDBY_AWAKE       30000
#define TIMEOUT_DEEP_SLEEP_NO_INT   300000
#define TIMEOUT_WDT                 10000  /* 10s: enough for 1s LED toggle + margin */
#define TIMEOUT_BUTTON_LONG_PRESS   3000
#define TIMEOUT_BLE_CMD_DELAY       500
#define BAD_SIGNAL_TIMEOUT_SEC      10      /* Invalid data for this long -> sleep */
#define BAD_SIGNAL_LIGHT_SLEEP_SEC  30      /* Light-sleep before deep-sleep */

/* ==================== LED Blink ==================== */
#define PPG_LED_RATE_OFF        30
#define PPG_LED_RATE_SLOW       70
#define PPG_LED_RATE_FAST       100
#define PPG_LED_BLINK_SLOW_MS   600
#define PPG_LED_BLINK_MED_MS    300
#define PPG_LED_BLINK_FAST_MS   150
#define PPG_LED_BLINK_MAX_MS    80

/* ==================== Validation ==================== */
#define MIN_VALID_TIMESTAMP     1700000000
#define BATTERY_MIN_WIFI_PCT    20
#define BATTERY_MIN_PPG_PCT     10
#define BLE_QUALITY_VALID       80
#define BLE_QUALITY_INVALID     20

/* ==================== Application Constants ==================== */
#define IR_AMPLITUDE_NO_FINGER  500     /* IR amplitude below this = no finger */
#define BATTERY_CHECK_INTERVAL  100     /* Check battery every N samples */
#define WIFI_MAINTAIN_SEC       60      /* WiFi maintain timeout (seconds) */
#define LOW_VOLTAGE_THRESHOLD   330     /* Low battery threshold (mV, raw ADC) */
#define LOW_VOLTAGE_COUNT_MAX   3       /* Consecutive low readings before action */
#define HR_VALID_MIN            40      /* Heart rate minimum valid (bpm) */
#define HR_VALID_MAX            200     /* Heart rate maximum valid (bpm) */
#define PEAK_COUNT_MIN          2       /* Minimum peaks for valid HR */
#define PEAK_COUNT_MAX          25      /* Maximum peaks (noise filter) */
#define SPO2_RATIO_MIN          10      /* SpO2 ratio minimum valid */
#define SPO2_RATIO_MAX          180     /* SpO2 ratio maximum valid */

/**
 * @brief PPG raw sample for SD storage (decoupled from max30102 driver)
 *
 * Layout matches max30102_raw_t but defined independently to avoid
 * cross-component type dependency between sd_storage and max30102.
 */
typedef struct {
    uint32_t red;   /**< Red light ADC value */
    uint32_t ir;    /**< IR light ADC value */
} sd_raw_record_t;

/* ==================== System State API ==================== */
void system_set_state(system_state_t new_state);
system_state_t system_get_state(void);
