# Architecture Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use compose:subagent (recommended) or compose:execute to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate 4 remaining direct cross-component dependencies in the ESP32-S3 PPG firmware to complete the callback-based decoupling architecture.

**Architecture:** Extend the existing `ble_callbacks_t` / `http_callbacks_t` callback interfaces to cover the 2 remaining direct dependencies (uart_recorder, ota_upgrade). Remove a redundant `esp_wifi_stop()` call. Decouple `sd_storage` from `max30102` by inlining the simple 2-field struct.

**Tech Stack:** ESP-IDF v6.0.1, C, FreeRTOS, I2C, SPI

## Global Constraints

- All firmware code/comments/logs must be in English
- Do NOT compile or flash unless explicitly asked by user
- `idf.py build` must pass after each task
- Follow existing callback pattern: declare in `ble_callbacks.h`/`http_callbacks.h`, register in `main.c` s_ble_cbs/s_http_cbs, use via `s_cbs->xxx()` in components
- `picolibc vfprintf` uses ~8KB stack; use `puts()`/`PPG_LOGI()` on small-stack tasks

---

## File Structure

| File | Change |
|------|--------|
| `components/ppg_config/include/ble_callbacks.h` | Add `uart_record_start`, `uart_record_stop` callbacks |
| `components/ppg_config/include/http_callbacks.h` | Add `ota_upgrade_from_http` callback + `http_read_func` typedef |
| `main/main.c` | Register new callbacks in s_ble_cbs/s_http_cbs, remove redundant `esp_wifi_stop()` |
| `components/ble_svc/ble_svc.c` | Replace direct uart_recorder calls with `s_cbs->xxx()` |
| `components/wifi_transfer/wifi_transfer.c` | Replace direct ota_upgrade call with `s_cbs->xxx()` |
| `components/sd_storage/include/sd_storage.h` | Replace `#include "max30102.h"` with inline struct |
| `components/ppg_config/include/ppg_config.h` | Add `sd_raw_record_t` struct definition |

---

## Task 1: Add uart_recorder callbacks to ble_callbacks_t

**Covers:** Issue 1 — ble_svc → uart_recorder direct dependency

**Files:**
- Modify: `components/ppg_config/include/ble_callbacks.h:21-47`
- Modify: `components/uart_recorder/include/uart_recorder.h:40-52`

**Interfaces:**
- Produces: `ble_callbacks_t.uart_record_start(config)`, `ble_callbacks_t.uart_record_stop()`

- [ ] **Step 1: Add forward declaration and callback typedef to ble_callbacks.h**

Add after line 15 (`#include <stddef.h>`):

```c
/* UART recorder config forward declaration */
typedef struct {
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
} uart_recorder_config_t;

typedef esp_err_t (*uart_record_start_fn)(const uart_recorder_config_t *config);
typedef void (*uart_record_stop_fn)(void);
```

Add to `ble_callbacks_t` struct before the closing brace (after `log_get_buffer_count`):

```c
    /* UART recorder */
    uart_record_start_fn uart_record_start;
    uart_record_stop_fn uart_record_stop;
```

- [ ] **Step 2: Register callbacks in main.c s_ble_cbs**

In `main/main.c` at line 107 (before closing brace of s_ble_cbs), add:

```c
    .uart_record_start = uart_recorder_start,
    .uart_record_stop = uart_recorder_stop,
```

Add `#include "uart_recorder.h"` to main.c includes if not present.

- [ ] **Step 3: Replace direct calls in ble_svc.c**

In `components/ble_svc/ble_svc.c`:
- Line 24: Remove `#include "uart_recorder.h"`
- Line 563: Replace `esp_err_t ret = uart_recorder_start(&cfg);` with `esp_err_t ret = s_cbs->uart_record_start(&cfg);`
- Line 574: Replace `uart_recorder_stop();` with `s_cbs->uart_record_stop();`

- [ ] **Step 4: Build verification**

```bash
cd esp32s3 && source ~/esp/esp-idf-v6.0.1/export.sh > /dev/null 2>&1 && idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
cd esp32s3 && git add -A && git commit -m "refactor: decouple ble_svc from uart_recorder via callbacks"
```

---

## Task 2: Add ota_upgrade callback to http_callbacks_t

**Covers:** Issue 2 — wifi_transfer → ota_upgrade direct dependency

**Files:**
- Modify: `components/ppg_config/include/http_callbacks.h:39-42`
- Modify: `components/wifi_transfer/wifi_transfer.c:18,349-360`
- Modify: `main/main.c:110-121`

**Interfaces:**
- Produces: `http_callbacks_t.ota_upgrade(content_len, read_func, read_ctx)`

- [ ] **Step 1: Add callback to http_callbacks.h**

Add after line 41 (`const char *(*ota_get_build_time)(void);`):

```c
    /* OTA upgrade */
    esp_err_t (*ota_upgrade)(size_t content_len, int (*read_func)(void *ctx, void *buf, size_t len), void *read_ctx);
```

- [ ] **Step 2: Register callback in main.c s_http_cbs**

In `main/main.c` after line 121 (`.ota_get_build_time`), add:

```c
    .ota_upgrade = ota_upgrade_from_http,
```

Ensure `#include "ota_upgrade.h"` is in main.c includes.

- [ ] **Step 3: Replace direct call in wifi_transfer.c**

In `components/wifi_transfer/wifi_transfer.c`:
- Line 18: Remove `#include "ota_upgrade.h"`
- Line 360: Replace `esp_err_t ret = ota_upgrade_from_http(req->content_len, http_read_func, req);` with `esp_err_t ret = s_cbs->ota_upgrade(req->content_len, http_read_func, req);`

Note: `http_read_func` is a local static in wifi_transfer.c (line 349), so it's passed as the callback's read function — this is fine, the callback just wraps the OTA call.

- [ ] **Step 4: Build verification**

```bash
cd esp32s3 && source ~/esp/esp-idf-v6.0.1/export.sh > /dev/null 2>&1 && idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
cd esp32s3 && git add -A && git commit -m "refactor: decouple wifi_transfer from ota_upgrade via callback"
```

---

## Task 3: Remove redundant esp_wifi_stop() in enter_deep_sleep

**Covers:** Issue 3 — enter_deep_sleep redundant WiFi stop

**Files:**
- Modify: `main/main.c:768-769`

**Interfaces:** None — removes a redundant call, no API change.

- [ ] **Step 1: Remove redundant line**

In `main/main.c`, remove line 769 (`esp_wifi_stop();`). The `wifi_transfer_stop()` on line 768 already calls `esp_wifi_stop()` via the callback chain (`wifi_transfer_stop` → `s_cbs->wifi_disconnect()` → `wifi_prov_disconnect()` → `esp_wifi_stop()`).

Before:
```c
    if (s_wifi_initialized) {
        wifi_transfer_stop();
        esp_wifi_stop();
        puts("[SLEEP] WiFi stopped");
    }
```

After:
```c
    if (s_wifi_initialized) {
        wifi_transfer_stop();
        puts("[SLEEP] WiFi stopped");
    }
```

- [ ] **Step 2: Build verification**

```bash
cd esp32s3 && source ~/esp/esp-idf-v6.0.1/export.sh > /dev/null 2>&1 && idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 3: Commit**

```bash
cd esp32s3 && git add -A && git commit -m "fix: remove redundant esp_wifi_stop() in enter_deep_sleep"
```

---

## Task 4: Decouple sd_storage from max30102 type

**Covers:** Issue 5 — sd_storage max30102_sample_t dependency

**Files:**
- Modify: `components/ppg_config/include/ppg_config.h` (add `sd_raw_record_t`)
- Modify: `components/sd_storage/include/sd_storage.h:14,65-66,108`
- Modify: `components/sd_storage/sd_storage.c:406`

**Interfaces:**
- Produces: `sd_raw_record_t` struct (red:u32 + ir:u32) in ppg_config.h
- Changes: `sd_storage_write_raw()` signature from `const max30102_sample_t *` to `const sd_raw_record_t *`

- [ ] **Step 1: Add sd_raw_record_t to ppg_config.h**

Add before the `#ifdef __cplusplus` at the end of ppg_config.h (or after existing struct definitions):

```c
/**
 * @brief PPG raw sample for SD storage (decoupled from max30102 driver)
 *
 * Layout matches max30102_raw_t but defined independently to avoid
 * cross-component type dependency.
 */
typedef struct {
    uint32_t red;   /**< Red light ADC value */
    uint32_t ir;    /**< IR light ADC value */
} sd_raw_record_t;
```

- [ ] **Step 2: Update sd_storage.h to use sd_raw_record_t**

In `components/sd_storage/include/sd_storage.h`:
- Line 14: Remove `#include "max30102.h"` (keep `#include "ppg_algo.h"` if still needed, or remove if ppg_algo_result_t is also replaced)
- Line 66: Replace `typedef max30102_raw_t max30102_sample_t;` with `typedef sd_raw_record_t max30102_sample_t;` (keep as alias for backward compatibility during transition)
- Line 108: Replace `const max30102_sample_t *sample` with `const sd_raw_record_t *sample`

Actually, simpler approach — just replace the typedef and remove the include:

Before:
```c
#include "max30102.h"
...
typedef max30102_raw_t max30102_sample_t;
```

After:
```c
/* sd_raw_record_t is defined in ppg_config.h, included transitively via ppg_algo.h */
typedef sd_raw_record_t max30102_sample_t;
```

Note: `ppg_algo.h` already includes `ppg_config.h` which defines `sd_raw_record_t`. Verify this chain exists. If not, add `#include "ppg_config.h"` explicitly.

- [ ] **Step 3: Update sd_storage.c function signature**

In `components/sd_storage/sd_storage.c` line 406:
Replace `esp_err_t sd_storage_write_raw(const max30102_sample_t *sample)` with `esp_err_t sd_storage_write_raw(const sd_raw_record_t *sample)`.

- [ ] **Step 4: Update caller in main.c**

In `main/main.c` line 459, `batch_buf` is `max30102_raw_t batch_buf[32]`. The function `sd_storage_write_raw()` now takes `const sd_raw_record_t *`. Both structs have identical layout (`{uint32_t red; uint32_t ir;}`).

Line 483: Replace `sd_storage_write_raw(&batch_buf[i]);` with:
```c
sd_storage_write_raw((const sd_raw_record_t *)&batch_buf[i]);
```

This cast is safe due to identical struct layout.

- [ ] **Step 5: Build verification**

```bash
cd esp32s3 && source ~/esp/esp-idf-v6.0.1/export.sh > /dev/null 2>&1 && idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 6: Commit**

```bash
cd esp32s3 && git add -A && git commit -m "refactor: decouple sd_storage from max30102 type dependency"
```

---

## Task 5: Update 架构分析.md

**Files:**
- Modify: `架构分析.md`

- [ ] **Step 1: Update architecture doc**

Remove the 4 fixed issues from the remaining issues section. Update the dependency graph to show fully decoupled state. Add the newly completed items to the doc.

- [ ] **Step 2: Commit**

```bash
cd esp32s3 && git add -A && git commit -m "docs: update architecture analysis after cleanup"
```
