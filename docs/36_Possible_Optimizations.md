# 36_Possible_Optimizations.md

This section documents optimizations that could be applied to this driver — performance, power, memory, or code quality improvements that were not implemented because the current implementation meets all requirements without them. Each entry explains what the optimization is, what it costs, what it buys, and under what conditions it becomes worth doing.

---

## 36.1 Remove the 100 µs Settling Delay

**Current behavior:** A 100 µs busy-wait executes before every burst read inside `bma423_read_accel()`. At 25 Hz this consumes 2.5 ms of CPU time per second in a busy loop — the CPU is running at full speed doing nothing useful.

**Optimization:** Remove the delay after confirming via oscilloscope that data registers are stable at interrupt assertion time.

**What it buys:** 2.5 ms/second of CPU time returned to the scheduler. At 160 MHz this is 400,000 clock cycles per second — negligible for a single-task system, but meaningful if the CPU is trying to sleep between operations.

**What it costs:** Risk of reading partially written data if the interrupt asserts before registers are fully stable. This risk is currently unquantified — the delay exists precisely because the timing relationship was not verified against the datasheet.

**When to do it:** After oscilloscope measurement confirms the INT1 assertion timing. If data is stable at interrupt time, the delay is unnecessary and should be removed. If a delay is required, the measurement gives the minimum value and the delay can be reduced to that value plus a conservative margin.

**How to verify:** Toggle a GPIO at interrupt assertion and at first register byte availability. Measure the interval. If interval is zero or sub-microsecond, the delay is unnecessary.

---

## 36.2 Reduce Stack Allocation After Measurement

**Current behavior:** `bma423_task` is allocated 2048 words (8192 bytes) of stack. Estimated actual usage is 512–1024 bytes — leaving 4–8× headroom.

**Optimization:** Measure actual high-water mark with `uxTaskGetStackHighWaterMark()`. Set allocation to actual usage plus 25% safety margin, rounded up to the nearest 256 words.

**What it buys:** Freed heap memory. If actual usage is 512 bytes (128 words), the stack could be reduced to 192 words (768 bytes) — freeing 7424 bytes of heap. On a system with 300 KB available heap this is minor. On a system running many tasks simultaneously it adds up.

**What it costs:** Nothing if measurement is done first. Risk of stack overflow if reduced without measurement.

**When to do it:** Immediately — this is the lowest-effort optimization in this section. Two lines of code to measure, one constant to adjust.

---

## 36.3 Replace Busy-Wait Inter-Write Delays With Task Yields

**Current behavior:** `bma423_init()` uses `esp_rom_delay_us(1000)` between consecutive register writes — a busy-wait that holds the CPU for 1 ms per delay, with five such delays in the initialization sequence.

**Optimization:** Replace with `vTaskDelay(pdMS_TO_TICKS(1))`.

**What it buys:** During initialization, the CPU yields to other tasks instead of spinning. AXP202 or PCF8563 transactions that are waiting can execute during these windows.

**What it costs:** Timing precision. `vTaskDelay(1ms)` may sleep for 0–10 ms depending on FreeRTOS tick rate (default 100 Hz = 10 ms tick). The BMA423 requires a minimum 1000 µs inter-write delay — `vTaskDelay(1ms)` at 100 Hz tick rate may sleep for up to 10 ms, which exceeds the requirement but wastes more time than necessary.

**When to do it:** If initialization time becomes a concern or if other tasks need I²C access during sensor initialization. Currently initialization takes approximately 57 ms dominated by the 50 ms boot delay — the five 1 ms busy-waits are 9% of initialization time and not a bottleneck.

**Better alternative:** Increase FreeRTOS tick rate to 1000 Hz (`configTICK_RATE_HZ = 1000`), making `vTaskDelay(1ms)` accurate to ±1 ms. This change affects the entire system and requires validation across all tasks.

---

## 36.4 Reduce I²C Transaction Overhead With DMA

**Current behavior:** All I²C transactions use interrupt-driven transfers managed by the ESP-IDF I²C driver. The CPU is involved in transaction setup and teardown for every byte transferred.

**Optimization:** Use DMA-backed I²C transfers for the six-byte burst read. The CPU initiates the transfer and is notified on completion — it does not manage individual bytes.

**What it buys:** Reduced CPU involvement during the burst read. At 400 kHz transferring six bytes, the transaction takes approximately 210 µs. With DMA, the CPU could perform other work during this time rather than waiting in `i2c_master_cmd_begin()`.

**What it costs:** Increased implementation complexity. DMA buffer alignment requirements. Cache coherency considerations on ESP32 (DMA operates on physical memory — cache flush/invalidate required). The new `driver/i2c_master.h` API supports DMA mode; the legacy driver does not.

**When to do it:** At ODRs above 100 Hz where I²C transactions consume a meaningful fraction of CPU time. At 25 Hz the 210 µs burst read represents 0.5% of the sample period — DMA provides no practical benefit at this rate.

---

## 36.5 Adaptive ODR Based on Activity

**Current behavior:** ODR is fixed at 25 Hz regardless of system state. The accelerometer samples at 25 Hz whether the user is actively moving or the watch has been stationary for an hour.

**Optimization:** Reduce ODR when no significant motion is detected for a threshold period. Increase ODR when motion is detected. The BMA423 supports ODR changes at runtime by writing to `ACC_CONF`.

**What it buys:** Power reduction during stationary periods. At 12.5 Hz ODR (one step down from 25 Hz) the sensor consumes approximately half the current. Combined with the power mode averaging filter already in use, low-activity periods could see significant battery life improvement.

**What it costs:** ODR change requires a write to `ACC_CONF` with read-back verification. The inter-write delay applies. The sensor's output data rate changes — the application must account for variable timing between samples. Motion detection threshold tuning adds algorithmic complexity.

**Implementation approach:** Track peak acceleration magnitude over a rolling window. If magnitude stays below a threshold (e.g. 50 mg variation) for N consecutive seconds, reduce ODR to 12.5 Hz. On any sample exceeding the threshold, immediately restore 25 Hz ODR. The ODR change itself takes one I²C write — approximately 72 µs.

**When to do it:** When battery life optimization becomes a priority. Requires the power mode transition infrastructure described in Section 35.8 as a prerequisite.

---

## 36.6 Compile-Time Configurable Parameters

**Current behavior:** `MAX_RETRY_COUNT`, `RETRY_DELAY_MS`, `MAX_REINIT_COUNT`, `REINIT_DELAY_MS`, `ACCEL_QUEUE_DEPTH`, and `BMA423_INT1_GPIO` are all `#define` constants in `bma423_isr.c` and `bma423_regs.h`. Changing them requires modifying source files.

**Optimization:** Move these to a single `bma423_config.h` file that aggregates all tunable parameters with documented defaults. Each parameter has a compile-time override mechanism:

```c
// bma423_config.h
#ifndef BMA423_MAX_RETRY_COUNT
#define BMA423_MAX_RETRY_COUNT    3
#endif

#ifndef BMA423_RETRY_DELAY_MS
#define BMA423_RETRY_DELAY_MS     10
#endif

#ifndef BMA423_INT1_GPIO
#define BMA423_INT1_GPIO          39
#endif
```

A project using this driver on a different board sets `BMA423_INT1_GPIO` in its build system without modifying driver source files.

**What it buys:** Easier board-level customization. Cleaner separation between driver logic and deployment configuration. Parameters are findable in one place.

**What it costs:** One additional header file. Slightly more complex include structure.

**When to do it:** When porting to a second board or when the driver is shared across multiple projects. For a single-board project the current approach is adequate.

---

## 36.7 Structured Logging Over printf

**Current behavior:** All diagnostic output uses `printf()` directly. Log lines have manually maintained prefixes (`[CHIP_ID]`, `[RECOVERY L1]`, etc.) with no severity levels, no filtering, and no ability to disable logging in production without modifying source files.

**Optimization:** Replace `printf()` with ESP-IDF's logging macros (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`, `ESP_LOGD`):

```c
// Current
printf("[CHIP_ID] Read = 0x%02X\n", chip_id);

// Optimized
ESP_LOGI(TAG, "Chip ID read = 0x%02X", chip_id);
```

Where `TAG` is defined per file:

```c
static const char *TAG = "BMA423";
```

**What it buys:** Log level filtering at runtime — debug logs can be disabled in production without recompilation. Automatic timestamp and tag prefixing. Consistent format across all ESP-IDF components. Log level can be set per component via `menuconfig`.

**What it costs:** ESP-IDF dependency in files that currently avoid it. Minor — `bma423.c` already includes ESP-IDF headers indirectly through `i2c.h`.

**When to do it:** Before deploying in any production context. `printf()` is acceptable for development and demonstration. Structured logging is standard practice in any ESP-IDF application that reaches production.

---

## 36.8 Single Atomic Read of All Six Data Bytes Using I²C Burst With Address Auto-Increment Verification

**Current behavior:** The burst read assumes registers 0x12–0x17 are at consecutive addresses and that the BMA423 auto-increments its internal register pointer across them within a single transaction. Both were verified against the datasheet — but the driver does not confirm this assumption at runtime.

**Optimization:** On first initialization, read registers 0x12 and 0x14 individually and compare to the first and third bytes of a burst read. If they match, auto-increment is working correctly. If they do not match, log a warning and fall back to six individual reads.

**What it buys:** Runtime confirmation that the burst read assumption holds. Defense against a hypothetical hardware revision that changes register layout.

**What it costs:** Two additional I²C reads at initialization — approximately 200 µs. Increased code complexity for a failure mode that has not been observed and is unlikely given the BMA423 is a well-documented, widely-used part.

**When to do it:** Never, for this specific use case. The BMA423 register map is fixed by Bosch's specification and will not change between hardware revisions. This optimization is documented for completeness and rejected as over-engineering for a single-sensor driver.

---

## 36.9 Remove I²C Scan From Production Build

**Current behavior:** `i2c_scan()` is called from `power_init()` during every boot. It performs 112 I²C read attempts across the full address space, taking approximately 112 × 97 µs = ~11 ms plus NACK timeout overhead for non-responding addresses.

**Optimization:** Wrap `i2c_scan()` in a compile-time flag:

```c
#ifdef ENABLE_I2C_SCAN
    i2c_scan();
#endif
```

Define `ENABLE_I2C_SCAN` only in debug builds.

**What it buys:** Approximately 11 ms of boot time. Elimination of the flood of `[DEBUG] i2c_master_cmd_begin error: 0xffffffff` messages that appear during the scan — caused by addresses with no responding device returning `ESP_FAIL`. Cleaner serial output in production.

**What it costs:** Loss of automatic bus verification on every boot. Bus configuration problems (wrong address, disconnected device) are no longer caught at startup.

**When to do it:** Before any deployment where clean boot logs matter or where boot time is constrained. The scan is a development tool — it should not run on every production boot.

