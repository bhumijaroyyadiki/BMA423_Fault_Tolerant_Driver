# 29_Debugging_Journey.md

This section documents every real bug encountered during development — in the order they were found, with the full diagnostic process for each. This is not a cleaned-up retrospective. It is an honest account of what went wrong, what was tried, what failed, and what eventually worked. The debugging notes kept during development are the primary source for this section.

---

## 29.1 Wrong I²C Address — BMA423 at 0x19, Not 0x18

**When found:** Phase 2, during I²C layer testing.

**Symptom:** The I²C bus scan during initial bring-up showed a device at `0x19`. The driver had been written assuming `0x18` — the BMA423 datasheet lists `0x18` as the primary address.

**Investigation:** No investigation was needed — the scan output was unambiguous. The question was why.

**Root cause:** The BMA423 I²C address is determined by the logic level on the SDO pin. SDO low → `0x18`. SDO high → `0x19`. On the TTGO T-Watch 2020 V3, the SDO pin is pulled high by the board hardware. This is a board-level constraint not visible in the sensor datasheet alone — it requires reading the board schematic or discovering it empirically via bus scan.

**Fix:** Changed `BMA423_ADDR` in `bma423_regs.h` from `0x18` to `0x19`.

**Lesson:** Never assume a device's I²C address from the datasheet alone. Always verify with a bus scan during initial bring-up. The scan tool added in Phase 2 caught this immediately — if it had not existed, the error would have surfaced as a mysterious initialization failure with no obvious cause.

---

## 29.2 Chip ID Read Returning Garbage After Soft Reset

**When found:** Phase 3, during `bma423_init()` development.

**Symptom:** After adding the soft reset command as the first initialization step, the chip ID read immediately following it occasionally returned values other than `0x13` — sometimes `0x00`, sometimes random bytes.

**Hypothesis 1:** Reset delay too short. The 1 ms delay immediately after reset was below FreeRTOS tick resolution (`pdMS_TO_TICKS(1)` rounded to zero or one tick = 0–10 ms depending on tick rate). The sensor may not have completed its internal power-up sequence.

**Test:** Increased delay to `pdMS_TO_TICKS(50)` — 50 ms, well above the datasheet's 1 ms minimum.

**Result:** Partially improved but still occasionally failed.

**Hypothesis 2:** Residual state on the bus or in the sensor's internal state machine immediately after reset requires a dummy transaction to clear.

**Test:** Added a throwaway `i2c_read()` of the chip ID register immediately after the delay — result explicitly discarded — before the real chip ID read.

**Result:** Chip ID read became 100% reliable.

**Root cause:** The BMA423 requires a dummy read after soft reset to complete its internal NVM load and settle its I²C state machine before responding correctly to subsequent transactions. The datasheet does not document this explicitly — it was found empirically.

**Fix:** Added dummy read with discarded result after the post-reset delay. Comment in code documents this as intentional:

```c
// Dummy read required after soft reset — clears reset state,
// allows internal NVM load to complete before real chip ID read
i2c_read(BMA423_ADDR, BMA423_CHIP_ID_REG, &chip_id, 1);
```

**Lesson:** Datasheet timing specifications are minimums under ideal conditions. Real hardware often requires conservative margins and empirical validation. A dummy read that looks like dead code is not dead code — it is a documented hardware workaround.

---

## 29.3 Correct Initialization, Zero Acceleration Data

**When found:** Phase 3, after completing the full initialization sequence.

**Symptom:** `bma423_init()` returned `BMA423_OK`. All register write verifications passed. The interrupt fired correctly at 25 Hz. `bma423_read_accel()` returned `BMA423_OK`. But all three axes returned exactly zero regardless of orientation or movement.

**Hypothesis 1:** Sign extension bug — negative values being incorrectly extended to zero.

**Test:** Tilted the sensor to an orientation where at least one axis should read strongly positive. All three axes still returned zero.

**Ruled out:** A sign extension bug would produce wrong non-zero values, not zero. Pure zeros on all axes is a different failure mode.

**Hypothesis 2:** Wrong register addresses for data registers. Reading from wrong locations that happen to contain zero.

**Test:** Printed the raw register addresses being read. Confirmed `0x12`–`0x17` — correct per datasheet.

**Ruled out.**

**Hypothesis 3:** Accelerometer not enabled. The sensor has a separate power control register that enables the accelerometer subsystem — disabled by default after reset.

**Test:** Read back `PWR_CTRL` register (`0x7D`). Value was `0x00` — accelerometer disabled.

**Root cause confirmed:** `PWR_CTRL` defaults to `0x00` after reset. Bit 2 (`acc_en`) must be explicitly set to `1` to enable the accelerometer. Without this, the sensor initializes correctly and produces no errors — it simply does not measure anything.

**Fix:** Added explicit `PWR_CTRL` write as the final initialization step, with read-back verification:

```c
uint8_t pwr_ctrl = (PWR_CTRL_ACC_EN << PWR_CTRL_ACC_EN_POS);  // 0x04
i2c_write(BMA423_ADDR, BMA423_PWR_CTRL_REG, &pwr_ctrl, 1);
// verify...
```

**Lesson:** Read the full register map, not just the configuration registers. Power control registers are easy to overlook — they are not part of the "configure the sensor" mental model, but they are mandatory. A driver that produces zero data with no errors is exhibiting one of the most dangerous failure modes in embedded systems: silent incorrect behavior.

---

## 29.4 Intermittent `[BMA423_TASK] Read failed: 1`

**When found:** Phase 5, after the full interrupt pipeline was working and producing correct data.

**Symptom:** Approximately every 1.5 seconds, one or two consecutive reads failed with `BMA423_ERR_BUS`. The system recovered immediately on the next read — no intervention required. The failure was reproducible and periodic, not random.

**Hypothesis 1:** Bus contention between `bma423_task` and other tasks sharing the I²C bus (AXP202 at 0x35, PCF8563 at 0x51).

**Test:** Added FreeRTOS mutex around all `i2c_read()` and `i2c_write()` calls to serialize bus access.

**Result:** Mutex confirmed working via logging — no mutex timeout messages ever appeared. Failure persisted at the same rate. Ruled out.

**Hypothesis 2:** I²C transaction timeout too short, causing `i2c_master_cmd_begin` to abort valid transactions.

**Test:** Increased timeout from 10 ms to 50 ms.

**Result:** No change in failure rate or timing. Ruled out.

**Hypothesis 3:** Transient internal ESP-IDF legacy I²C driver behavior.

**Test:** Added raw error code logging around `i2c_master_cmd_begin`:

```c
if (err != ESP_OK) {
    printf("[DEBUG] i2c_master_cmd_begin error: 0x%x\n", err);
}
```

**Result:** Error code was consistently `0x107` — `ESP_FAIL` from the ESP-IDF driver, not a bus-specific error code. No mutex timeout message ever appeared.

**Characterization experiment:** Changed ODR from 25 Hz to 50 Hz. At 25 Hz, one read was affected per failure event. At 50 Hz, two consecutive reads were affected — the failure window doubled in transaction count but remained constant in time (~20–30 ms).

**Conclusion:** The failure window is fixed in time (~20–30 ms), not in transaction count. It occurs at a regular ~1.5 second interval. This is consistent with an internal semaphore or timer reset inside the ESP-IDF legacy I²C driver — corroborated by the boot-time warning:

```
W (271) i2c: This driver is an old driver, please migrate 
your application code to adapt `driver/i2c_master.h`
```

**Fix:** Level 1 retry — 3 attempts with 10 ms delay, break on first success. In all observed instances, retry 1 succeeds. The 10 ms delay is sufficient to outlast the ~20–30 ms failure window in most cases because the first retry is attempted after the window has passed.

**Why not migrate to the new driver:** Assessed and deferred — see Section 34. The retry workaround handles the failure completely. Migration would require rewriting `i2c.c` against a new API with no benefit to the other 95% of the codebase.

**Lesson:** Not all failures are hardware failures. A periodic, time-based failure that recovers instantly after a short delay is almost certainly a software or driver-level timing issue, not a physical bus problem. The characterization experiment — changing ODR to observe whether failure count scales with transaction rate — was the key diagnostic step that distinguished hardware from software root cause.

---

## 29.5 Mutex Deadlock After Adding I²C Mutex

**When found:** Immediately after adding the FreeRTOS mutex to `i2c.c`.

**Symptom:** After flashing the mutex implementation, the I²C scan showed zero devices. `bma423_init()` failed immediately at chip ID read. The system appeared to have lost the entire I²C bus.

**Hypothesis 1:** Mutex not initialized before first use.

**Test:** Checked `i2c_init()` — mutex created correctly before any transactions. Ruled out.

**Hypothesis 2:** Mutex acquired but not released on error path.

**Test:** Traced every exit path in `i2c_write()`. Found:

```c
if (err != ESP_OK) {
    return I2C_ERR_BUS;  // ← early return, mutex never released
}
xSemaphoreGive(i2c_mutex);  // ← never reached on error
return I2C_OK;
```

**Root cause confirmed:** On the first failed transaction (any address that NACKed during the bus scan), the mutex was acquired but never released. Every subsequent `i2c_read()` or `i2c_write()` waited 50 ms for the mutex, timed out, and returned `I2C_ERR_TIMEOUT`. The bus appeared dead — it was not. The mutex was permanently held.

**Fix:** Restructured every post-acquisition exit to use a single result variable:

```c
i2c_status_t result = (err != ESP_OK) ? I2C_ERR_BUS : I2C_OK;
xSemaphoreGive(i2c_mutex);  // always reached
return result;
```

Additionally moved argument validation to before mutex acquisition — ensuring the early-return path for invalid arguments never holds the mutex:

```c
if (len == 0 || data == NULL) {
    xSemaphoreGive(i2c_mutex);  // BUG: mutex not yet acquired here
    return I2C_ERR_INVALID_ARG;
}
```

The above was the initial fix — it was then corrected to move validation before the `xSemaphoreTake` call entirely, eliminating the need to release on that path.

**Lesson:** Mutex acquisition and release must be symmetric on every code path — including error paths, early returns, and argument validation exits. The pattern of acquiring a mutex and then having multiple return statements is a resource leak waiting to happen. The correct pattern is: validate before acquiring, single exit point after acquiring.

---

## 29.6 Retry Loop Running All Iterations After Success

**When found:** Phase 6, during Level 1 recovery implementation.

**Symptom:** After implementing the retry loop, the output showed:

```
[RECOVERY L1] Retry 1 succeeded
[RECOVERY L1] Retry 2 succeeded
[RECOVERY L1] Retry 3 succeeded
[RECOVERY L1] Read retries exhausted
```

Retry 1 succeeded — but the loop continued to retry 2 and 3, and then printed "retries exhausted" even though recovery had been successful.

**Root cause 1:** `retry_count` was being incremented twice — once by the `for` loop increment expression and once manually inside the loop body. This caused the loop to skip every other retry index and behave inconsistently.

```c
for (retry_count = 0; retry_count < 3; retry_count++) {
    // ...
    retry_count++;  // ← double increment
}
```

**Root cause 2:** "Retries exhausted" message was printed unconditionally after the loop — outside any check on whether the loop exited via `break` (success) or via exhaustion (failure).

**Fix 1:** Removed the manual `retry_count++` inside the loop body — the `for` increment expression handles it.

**Fix 2:** Wrapped the exhausted message in a status check:

```c
if (status != BMA423_OK) {
    printf("[RECOVERY L1] All retries exhausted\n");
    // escalate to Level 2
}
```

**Lesson:** A `for` loop with a manual increment inside the body is a classic off-by-one error waiting to happen. Post-loop state checks are required when a loop can exit via either `break` or natural termination — the caller needs to know which happened.

---

## 29.7 Level 2 Recovery — Verification Reads Failing During Fault Injection

**When found:** Phase 6, during fault injection testing.

**Symptom:** During fault injection testing with `g_inject_fault = true`, Level 2 re-initialization succeeded (chip ID read, all register writes and verifications passed) but the verification read after re-init failed:

```
[RECOVERY L2] Re-init 1/3 succeeded
[FAULT INJECT] Forced failure #5
[RECOVERY L2] Verification read failed
```

**This was expected behavior, not a bug.** The fault injection flag forces `bma423_read_accel()` to return `BMA423_ERR_BUS` unconditionally. After re-init, the verification read calls `bma423_read_accel()` — which also hits the fault flag. The verification fails not because the sensor is misconfigured but because the fault is still active.

**Why this is the correct behavior to test:** In a real hardware failure scenario, re-initializing the sensor does not clear an electrical fault. If INT1 is shorted, or SDA is marginal, `bma423_init()` may succeed (it uses slower, more robust transactions) while `bma423_read_accel()` still fails. The verification read after re-init specifically tests this scenario — and correctly escalates to Level 3 when it fails three times.

**Lesson:** Fault injection testing reveals not just whether recovery code runs, but whether it runs correctly under sustained fault conditions. A re-init that succeeds but is followed by a failed verification is a more realistic test of the recovery ladder than one where the fault clears itself after re-init.
