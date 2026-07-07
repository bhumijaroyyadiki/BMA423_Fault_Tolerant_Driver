# 35_Future_Improvements.md

This section documents improvements that were identified during development but not implemented — either because they were outside the current project scope, because the time investment was not justified by the current use case, or because they require prerequisite work that was not done. Each entry includes what the improvement is, why it was not done now, and what would be required to implement it.

---

## 35.1 Migrate to New ESP-IDF I²C Driver

**What:** Replace the legacy `driver/i2c.h` implementation in `i2c.c` with the new `driver/i2c_master.h` API recommended by ESP-IDF 5.x.

**Why not done:** The new API is ESP32-specific. Since the next development phase targets STM32, time invested in learning `i2c_master.h` deeply has low transferability. The retry workaround handles the legacy driver's periodic failure completely.

**What it would fix:** Eliminates the periodic `ESP_FAIL (0x107)` failure entirely. Removes the boot-time deprecation warning. Uses the supported, maintained API path going forward on ESP32.

**What it requires:** Rewrite `i2c.c` only. The function signatures in `i2c.h` remain identical — `i2c_init()`, `i2c_read()`, `i2c_write()` with the same parameters and return types. All other files are untouched. The new API uses bus handles (`i2c_master_bus_handle_t`) and device handles (`i2c_master_dev_handle_t`) as static module-level variables replacing the port number constant. Transaction model changes from command-link-based to direct `i2c_master_transmit_receive()` calls.

**Estimated effort:** One focused session. The layered architecture makes this a contained change.

---

## 35.2 Measure and Validate Stack Usage

**What:** Call `uxTaskGetStackHighWaterMark(accel_task_handle)` after the system has been running under load for several minutes. Use the result to right-size the stack allocation.

**Why not done:** Estimated usage (512–1024 bytes) is well within the 8192 byte allocation. No stack overflow was observed. The measurement was deprioritized.

**What it would confirm:** Whether the 2048 word stack allocation is appropriate, excessive, or insufficient. If excessive, the allocation can be reduced — freeing heap for other tasks. If insufficient, it catches a latent bug before it manifests as a corruption.

**What it requires:** Two lines of code and one serial log check. Lowest effort improvement in this list.

```c
// Add after system runs for 30+ seconds
printf("[PERF] Stack remaining: %u words\n",
       uxTaskGetStackHighWaterMark(accel_task_handle));
```

---

## 35.3 Check `xQueueSendFromISR` Return Value

**What:** Check whether `xQueueSendFromISR()` returns `pdTRUE` or `errQUEUE_FULL` in the ISR handler. Increment a dropped-event counter when the queue is full.

**Why not done:** At 25 Hz with a queue depth of 5, overflow was never observed during testing. The fix was identified as a known gap (Section 34.2) but not prioritized within the project timeline.

**What it would provide:** Visibility into queue saturation events. A non-zero dropped-event counter would indicate that the task is not keeping up with the interrupt rate — an early warning of a scheduling problem before it becomes a data quality problem.

**What it requires:**

```c
static volatile uint32_t g_dropped_events = 0;

void IRAM_ATTR bma423_isr_handler(void *arg) {
    BaseType_t woken = pdFALSE;
    uint8_t event = 1;
    if (xQueueSendFromISR(accel_event_queue, &event, &woken) != pdTRUE) {
        g_dropped_events++;
    }
    portYIELD_FROM_ISR(woken);
}
```

The task can log `g_dropped_events` periodically. The variable must be `volatile` — it is written from ISR context and read from task context.

---

## 35.4 Validate Physical Disconnect Recovery

**What:** Physically disconnect the BMA423 SDA line during active operation and observe the system's behavior. Confirm the recovery ladder reaches Level 3 correctly and that the I²C bus remains functional for AXP202 and PCF8563 after disconnect.

**Why not done:** Hardware modification risk during the project window. Software fault injection was sufficient to validate recovery ladder logic. Physical disconnect tests a different and more destructive failure mode.

**What it would reveal:** Whether bus-stuck conditions (SDA held low by a device that lost power mid-transaction) are handled correctly. Whether the I²C driver requires additional recovery steps (SCL toggling to release a stuck slave) that the current recovery ladder does not implement.

**What it requires:** A test fixture or careful physical manipulation. Monitoring both the BMA423 recovery behavior and AXP202/PCF8563 continued operation simultaneously. A logic analyzer would significantly aid interpretation of bus behavior during disconnect.

---

## 35.5 Configurable Fault Injection via Compile Flag

**What:** Wrap all fault injection code in `#ifdef ENABLE_FAULT_INJECTION` preprocessor guards so it is completely absent from production builds.

**Why not done:** With `g_inject_fault = false` there is no runtime impact. The fix is low priority since the flag is initialized to false and requires explicit modification to activate.

**What it would provide:** Guarantee that fault injection code cannot accidentally affect production behavior. Smaller binary size in production builds. Cleaner separation between test and production code.

**What it requires:**

```c
// bma423.h
#ifdef ENABLE_FAULT_INJECTION
extern bool g_inject_fault;
#endif

// bma423.c
#ifdef ENABLE_FAULT_INJECTION
bool g_inject_fault = false;
static int g_fault_count = 0;
#endif

bma423_status_t bma423_read_accel(int16_t *x, int16_t *y, int16_t *z) {
#ifdef ENABLE_FAULT_INJECTION
    if (g_inject_fault) {
        printf("[FAULT INJECT] Forced failure #%d\n", ++g_fault_count);
        return BMA423_ERR_BUS;
    }
#endif
    // ... rest of function
}
```

Define `ENABLE_FAULT_INJECTION` in `CMakeLists.txt` or `sdkconfig` for debug builds only.

---

## 35.6 Migrate I²C Layer to STM32 HAL

**What:** Write an STM32-specific `i2c.c` implementing the same `i2c_init()`, `i2c_read()`, `i2c_write()` interface using STM32 HAL I²C functions or direct register access.

**Why not done:** The current project targets ESP32. STM32 development begins after this project concludes.

**What it would demonstrate:** The portability of the layered architecture. `bma423.c`, `bma423_regs.h`, and the recovery logic in `bma423_task()` would be identical on STM32 and ESP32. Only `i2c.c`, `bma423_isr.c`, and `bma423_platform.h` require platform-specific rewrites.

**What it requires:** STM32 HAL I²C init (`HAL_I2C_Init()`), memory-to-register write (`HAL_I2C_Mem_Write()`), and register-to-memory read (`HAL_I2C_Mem_Read()`). These map directly to the `i2c_write()` and `i2c_read()` contracts. The mutex implementation would use CMSIS-RTOS primitives if FreeRTOS is used on STM32, or a simple critical section if running bare-metal.

**Estimated effort:** One session to write `i2c.c` for STM32. The sensor driver layer requires no changes — this is the concrete validation of the portability claim made throughout this documentation.

---

## 35.7 FIFO-Based Burst Reading

**What:** Enable the BMA423's internal FIFO buffer and read multiple samples per I²C transaction instead of one sample per interrupt.

**Why not done:** Outside Phase 1 scope. The interrupt-driven single-sample architecture meets all current requirements. FIFO adds significant complexity — frame parsing, watermark interrupt configuration, overflow handling — for a benefit that is not needed at 25 Hz.

**What it would provide:** Reduced I²C transaction overhead at high ODRs. At 200 Hz, reading 8 samples per transaction reduces transaction count by 8× — significant at bus-contention levels. FIFO also provides resilience against missed interrupts — even if the MCU misses several interrupts, samples accumulate in the FIFO rather than being lost.

**What it requires:** Configure FIFO mode in BMA423 registers. Set watermark level. Change interrupt source from data-ready to FIFO watermark. Write a frame parser that extracts individual XYZ samples from the FIFO packet format. Handle FIFO overflow detection and recovery. This is a substantial addition — estimated at two to three sessions of development and testing.

---

## 35.8 Power Mode Transitions

**What:** Implement transitions between BMA423 active mode and suspend mode, coordinated with ESP32 light sleep or deep sleep.

**Why not done:** Phase 1 scope explicitly excluded power mode transitions. The current driver runs the accelerometer continuously at 25 Hz regardless of system power state.

**What it would provide:** Significant battery life improvement. At 25 Hz in active mode, the BMA423 consumes approximately 180 µA. In suspend mode it consumes approximately 2 µA — a 90× reduction. For a smartwatch that spends most of its time with the wrist stationary, duty-cycling the accelerometer based on motion detection (using the BMA423's built-in wrist tilt or any-motion interrupt) could extend battery life substantially.

**What it requires:** Two new driver functions — `bma423_suspend()` and `bma423_resume()` — writing to `PWR_CTRL` and `PWR_CONF`. A wake-on-motion interrupt configuration using the BMA423's feature engine (which requires loading the feature configuration file — the 450 µs disable, load, 140 ms enable sequence documented in the datasheet). Integration with the ESP32 sleep architecture. This interacts with the AXP202 power management and requires careful sequencing to avoid power rail conflicts.

---

## 35.9 Watchdog Integration

**What:** Feed a software watchdog from within `bma423_task()`'s main loop. If the task suspends or hangs, the watchdog expires and triggers a controlled system reboot.

**Why not done:** Requires a system-level watchdog architecture that spans all tasks — a design decision that belongs at the application level, not within an individual driver. Implementing a per-driver watchdog without a system-wide watchdog framework creates fragmented, inconsistent watchdog behavior.

**What it would provide:** Recovery from Level 3 task suspension or any unexpected hang in the BMA423 task — scenarios the current recovery ladder does not address. The system would reboot cleanly rather than running indefinitely without accelerometer data.

**What it requires:** A system-level watchdog manager that each task registers with and periodically feeds. The BMA423 task feeds the watchdog at the top of its main loop. If Level 3 suspends the task, the watchdog feed stops, the watchdog expires, and a controlled reboot occurs. This is a system architecture decision — the BMA423 driver would consume the watchdog API but not define it.

---

## 35.10 Formal Unit Testing

**What:** Write a suite of unit tests for `bma423.c` and `i2c.c` that run on the host (PC) rather than on the target hardware.

**Why not done:** Unit testing embedded drivers on host requires a hardware abstraction layer (HAL) that can be mocked — substituting fake `i2c_read()` and `i2c_write()` implementations that return controlled responses. Building this mock infrastructure was outside the project timeline.

**What it would provide:** Regression testing for the driver logic — sign extension, register construction, error code mapping, recovery ladder state transitions — without requiring hardware. Fast iteration during development. Confidence that refactoring does not break existing behavior.

**What it requires:** A mock `i2c.c` that returns programmer-controlled responses. A test runner (Unity, CppUTest, or similar). Test cases covering at minimum: correct chip ID passes, wrong chip ID returns `BMA423_ERR_CHIP_ID`, correct sign extension for positive and negative values, register construction produces correct bytes, recovery ladder transitions through all three levels correctly under sustained fault injection. The layered architecture makes mocking straightforward — `bma423.c` calls only `i2c_read()` and `i2c_write()`, which can be replaced with mock implementations at link time.

