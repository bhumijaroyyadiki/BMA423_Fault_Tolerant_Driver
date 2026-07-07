# 34_Limitations.md

This section documents the known limitations of the driver honestly and completely. A limitation is not a failure — it is a boundary condition that is understood, accepted, and documented. A system with documented limitations is safer than one that claims to handle everything, because its actual behavior under boundary conditions is known rather than assumed.

---

## 34.1 ESP-IDF Legacy I²C Driver — Periodic ESP_FAIL

**Description:** The driver uses ESP-IDF's legacy I²C driver (`driver/i2c.h`), which produces periodic `ESP_FAIL (0x107)` errors at approximately 1.5 second intervals. Each failure affects one or two consecutive I²C transactions depending on ODR. The failure window is approximately 20–30 ms wide.

**Root cause:** Internal timing behavior of the legacy driver — consistent with an internal semaphore or timer reset cycle. Corroborated by the ESP-IDF boot-time warning recommending migration to `driver/i2c_master.h`.

**Impact:** One or two acceleration samples are delayed by approximately 10 ms per 1.5 second cycle. No data is lost — Level 1 retry recovers every instance. No crash, no hang, no data corruption.

**Mitigation in place:** Level 1 retry with 10 ms delay. Retry 1 has succeeded 100% of the time in all observed instances.

**Permanent fix:** Migrate `i2c.c` to the new `driver/i2c_master.h` API. Only `i2c.c` requires changes — the layered architecture protects all other files. Migration was assessed and deferred in favor of STM32 development during the available project time. The migration surface is well-defined and documented.

**Severity:** Low. The failure is handled completely by the retry mechanism. No user-visible impact at 25 Hz ODR.

---

## 34.2 Queue Overflow Not Detected

**Description:** `xQueueSendFromISR()` returns `errQUEUE_FULL` when the queue is full. This return value is not currently checked in `bma423_isr_handler()`.

```c
void IRAM_ATTR bma423_isr_handler(void *arg) {
    BaseType_t woken = pdFALSE;
    uint8_t event = 1;
    xQueueSendFromISR(accel_event_queue, &event, &woken);  // return value ignored
    portYIELD_FROM_ISR(woken);
}
```

**Impact:** If `bma423_task()` falls behind the interrupt rate — for example, during Level 2 re-initialization which blocks for approximately 107 ms per attempt — the queue can fill and subsequent interrupts are silently dropped. There is no counter, no log, and no error indication when this occurs.

**When this occurs:** During Level 2 recovery, `bma423_task()` is blocked in `bma423_init()` for approximately 57 ms per attempt. At 25 Hz, approximately 1–2 interrupts fire during this window. With a queue depth of 5, this is handled without overflow in a single re-init attempt. If three re-init attempts run consecutively (~171 ms total), approximately 4 queue slots are consumed — within the depth-5 limit but close to the boundary.

**Mitigation:** Queue depth of 5 provides sufficient buffer for all observed recovery scenarios. The limitation becomes relevant only at higher ODRs or with deeper recovery nesting.

**Recommended fix:** Check the return value of `xQueueSendFromISR()` and increment a dropped-event counter (stored in a `volatile uint32_t` accessible to the task):

```c
if (xQueueSendFromISR(accel_event_queue, &event, &woken) != pdTRUE) {
    dropped_events++;  // volatile, task can log this periodically
}
```

---

## 34.3 No Stack Overflow Detection Configured

**Description:** The FreeRTOS stack overflow detection feature (`configCHECK_FOR_STACK_OVERFLOW`) was not verified as enabled in the project's `sdkconfig`. The `bma423_task` stack was allocated at 2048 words based on estimated usage — actual usage was not measured with `uxTaskGetStackHighWaterMark()`.

**Impact:** If the actual stack usage exceeds 2048 words — due to deep call chains, large local variables, or unexpected recursion — the overflow will corrupt adjacent heap or stack memory. On ESP32 this typically manifests as a hard fault or watchdog reset with a cryptic backtrace rather than a clean error message.

**Estimated actual usage:** Based on the call chain (`bma423_task` → `bma423_read_accel` → `i2c_read` → ESP-IDF internals), estimated stack usage is 512–1024 bytes (128–256 words). The 2048 word allocation provides approximately 4–8× headroom — unlikely to overflow in practice.

**Recommended fix:** Add this immediately after the system has been running for several seconds:

```c
UBaseType_t watermark = uxTaskGetStackHighWaterMark(accel_task_handle);
printf("[PERF] BMA423 task stack remaining: %u words\n", watermark);
```

If remaining words are above 512, the allocation is sufficient. If below 128, increase the allocation.

---

## 34.4 Physical Disconnect Not Tested

**Description:** The recovery ladder (Levels 1–3) was validated using software fault injection — a flag that forces `bma423_read_accel()` to return `BMA423_ERR_BUS` unconditionally. The sensor was not physically disconnected from the I²C bus during operation.

**Impact:** Physical disconnect produces different failure signatures than software injection. A disconnected SDA or SCL line may produce bus-stuck conditions where the line is held low by a device that lost power mid-transaction. This manifests differently from `ESP_FAIL` — it may produce `ESP_ERR_TIMEOUT` rather than `ESP_FAIL`, or it may leave the I²C bus in a state requiring a physical bus reset (toggling SCL to release a stuck slave).

The recovery ladder as implemented handles `BMA423_ERR_BUS` regardless of which underlying error code caused it. Level 2 re-initialization would likely fail on a physically disconnected sensor (chip ID read would fail). Level 3 would trigger correctly and suspend the task. However, a bus-stuck condition caused by a half-completed transaction at disconnect time might not be resolved by `bma423_init()` alone — it might require the I²C bus reset that was explicitly excluded from the recovery ladder.

**Recommended test:** Disconnect the BMA423 SDA line during active operation. Observe which error code appears, whether the recovery ladder progresses to Level 3 correctly, and whether the I²C bus remains functional for AXP202 and PCF8563 after the disconnect.

---

## 34.5 No Watchdog Integration

**Description:** No hardware or software watchdog timer is integrated with the BMA423 driver. If `bma423_task()` hangs in a state not covered by the recovery ladder — for example, inside `i2c_master_cmd_begin()` waiting on an internal semaphore that never releases — nothing external will detect or recover from the hang.

**When this matters:** The 50 ms mutex timeout and 10 ms I²C transaction timeout bound most wait conditions. However, if the ESP-IDF I²C driver's internal state machine enters an unrecoverable state that holds its internal semaphore indefinitely, the mutex timeout would eventually trigger — but the I²C driver would remain in a bad state. Subsequent calls would continue to timeout, Level 1 would exhaust, Level 2 re-initialization would fail (also using I²C), and Level 3 would suspend the task. This is the correct behavior — but the system would have lost the accelerometer permanently without a reboot.

**Mitigation in place:** The graduated recovery ladder terminates in task suspension rather than an infinite retry loop. The system continues running without the accelerometer. This is preferable to a watchdog reboot that disrupts all system state.

**Recommended addition for production:** Feed a software watchdog from within `bma423_task()`'s main loop. If the task suspends or hangs, the watchdog expires and triggers a controlled system reboot. This requires a system-level watchdog architecture that is outside the scope of this driver.

---

## 34.6 100 µs Settling Delay Not Datasheet-Verified

**Description:** A 100 µs busy-wait before the acceleration burst read was added as a conservative stabilization margin. The BMA423 datasheet describes the data-ready interrupt as asserting when a new data set is available — implying data is stable at interrupt time — but provides no explicit timing parameter specifying the relationship between interrupt assertion and register stability.

**Impact:** The 100 µs delay costs 2.5 ms of CPU busy-wait per second at 25 Hz. More significantly, if the delay is insufficient for some hardware configurations (longer PCB traces, different power supply characteristics, temperature extremes), reads could return partially written data with no indication of incorrectness.

**Mitigation:** In all testing performed, data consistency was maintained. The delay has not been observed to be insufficient.

**Recommended verification:** Oscilloscope measurement of the INT1 assertion timing relative to register write completion. If data is stable at interrupt assertion, the delay can be removed. If a delay is required, the measurement gives the minimum required value and a margin can be added with justification.

---

## 34.7 Single-Instance Driver

**Description:** The driver is designed for exactly one BMA423 sensor on one I²C bus. The I²C address is a compile-time constant (`BMA423_ADDR = 0x19`). The queue handle, task handle, and mutex are all static module-level variables. There is no instance handle or context pointer.

**Impact:** Instantiating a second BMA423 (at address 0x18 via SDO low) is not supported without significant restructuring. Multiple I²C buses are not supported. This is not a current requirement — the TTGO T-Watch 2020 V3 has one BMA423 — but it is a structural limitation.

**Recommended fix for multi-instance support:** Pass a configuration struct to `bma423_init()` containing the I²C address, GPIO pin, and queue depth. Return an opaque handle. All static module-level state moves into a heap-allocated context struct. This is a significant architectural change not warranted by the current single-sensor use case.

---

## 34.8 Mutex Timeout Path Not Runtime-Validated

**Description:** The code path that returns `I2C_ERR_TIMEOUT` when `xSemaphoreTake()` times out after 50 ms was not triggered during any testing. The code compiles correctly and follows the correct pattern — but its runtime behavior was not observed.

**Impact:** Unknown. The timeout path returns `I2C_ERR_TIMEOUT` to the caller, which `bma423_read_accel()` maps to `BMA423_ERR_BUS`. This triggers the recovery ladder — which is the correct behavior. The concern is not that the path is wrong but that it has not been exercised.

**How to trigger for testing:** Hold the I²C mutex for longer than 50 ms from another task:

```c
// Test task — forces mutex timeout in i2c_read/write
xSemaphoreTake(i2c_mutex_external_ref, portMAX_DELAY);
vTaskDelay(pdMS_TO_TICKS(100));  // hold for 100ms
xSemaphoreGive(i2c_mutex_external_ref);
```

This requires exposing the mutex handle, which is currently static — a test-only modification.

---

## 34.9 `g_inject_fault` Flag in Production Build

**Description:** The fault injection flag `g_inject_fault` in `bma423_read_accel()` must be removed or conditionally compiled out of production builds. It is currently present in the codebase as a global `bool` initialized to `false`.

**Impact:** With `g_inject_fault = false` (the default), there is no runtime impact — the branch is never taken and the compiler may optimize it out entirely. However, the flag is accessible from any code that includes `bma423.h` — an accidental `g_inject_fault = true` in production code would disable the accelerometer silently.

**Recommended fix:**

```c
#ifdef ENABLE_FAULT_INJECTION
    if (g_inject_fault) {
        return BMA423_ERR_BUS;
    }
#endif
```

Define `ENABLE_FAULT_INJECTION` only in debug build configurations. Production builds never compile the fault injection code.

