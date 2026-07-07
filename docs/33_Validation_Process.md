# 33_Validation_Process.md

This section documents how the driver was validated — what was tested, how it was tested, what the pass criteria were, and what the results were. This is not a formal test plan — it is an honest account of the validation that actually happened, including what was not tested and why.

---

## 33.1 Validation Philosophy

Validation for this driver followed two principles.

First — validate each layer independently before combining layers. The I²C layer was validated before any sensor code was written. The initialization sequence was validated before the interrupt pipeline was built. Each layer's correctness was established on its own terms before it became a dependency for the next layer.

Second — validate both the happy path and the failure path. A driver that works correctly under normal conditions but fails silently or dangerously under abnormal conditions is not a robust driver. Every major component has a corresponding failure test — not just a success test.

---

## 33.2 I²C Layer Validation

**Test:** Bus scan across all valid 7-bit I²C addresses (0x08–0x77).

**Method:** `i2c_scan()` attempted a single-byte read from register 0x00 at each address. Addresses that ACKed were printed; addresses that NACKed printed `--`.

**Pass criteria:** Known devices appear at correct addresses. No false positives on empty addresses.

**Result:**
```
0x19 → BMA423 ✅
0x35 → AXP202 ✅
0x51 → PCF8563 ✅
All other addresses → -- ✅
```

**What this validated:** I²C master initialization correct. SDA/SCL pin assignment correct. Pull-up resistors functional. Address decoding working. BMA423 address confirmed as 0x19 (not assumed 0x18).

**What this did not validate:** Multi-byte reads, repeated START, write transactions, timeout behavior. These were validated implicitly through initialization testing.

---

## 33.3 Initialization Sequence Validation

Each step of `bma423_init()` was validated with explicit serial output before the next step was added. The final validation checked the complete 10-step sequence end-to-end.

**Step-by-step pass criteria:**

| Step | Pass criteria | Validated by |
|---|---|---|
| Soft reset | No I²C error returned | `[DEBUG] Soft reset sent` log line |
| Boot delay | System continues normally after delay | Implicit — next step executes |
| Dummy read | No crash, result discarded | Implicit |
| Chip ID | Returns `0x13` | `[CHIP_ID] Read = 0x13` log line |
| ERR_REG | Returns `0x00` | `[ERR_REG] = 0x00` log line |
| ACC_CONF | Write succeeds, read-back matches `0x26` | Verification check passes silently |
| ACC_RANGE | Write succeeds, read-back matches `0x01` | Verification check passes silently |
| INT1_IO_CTRL | Write succeeds, read-back matches `0x0B` | Verification check passes silently |
| INT_MAP | Write succeeds, read-back matches `0x04` | `[INT_MAP] = 0x04` log line |
| PWR_CTRL | Write succeeds, read-back matches `0x04` | `[PWR_CTRL] Accelerometer enabled` log line |

**Full initialization pass output:**
```
[CHIP_ID] Read = 0x13
[CHIP_ID] BMA423 detected correctly
[ERR_REG] = 0x00
[INT_MAP] = 0x04
[PWR_CTRL] Accelerometer enabled
```

**What this validated:** Every register write takes effect. Every read-back matches the written value. The sensor is alive, correctly identified, and fully configured.

**What this did not validate:** Behavior when initialization fails mid-sequence. This was validated separately via fault injection — see Section 33.6.

---

## 33.4 Interrupt Pipeline Validation

**Test 1 — ISR fires on sensor interrupt:**

**Method:** Added temporary `printf` inside `bma423_isr_handler` (removed after validation — printf inside ISR is dangerous in production).

**Pass criteria:** ISR log appears at regular intervals matching configured ODR.

**Result:** ISR fired at approximately 25 Hz. Confirmed interrupt routing from BMA423 INT1 pin to GPIO39 to ESP32 ISR handler is correct.

**Test 2 — Queue delivers events to task:**

**Method:** Observed `X=... Y=... Z=...` output in serial monitor.

**Pass criteria:** Output appears at ~25 Hz. No output without sensor connected (confirms events come from real interrupts, not spurious sources).

**Result:** Output appeared consistently at 25 Hz. ✅

**Test 3 — Data responds to physical motion:**

**Method:** Picked up the watch and moved it in various orientations.

**Pass criteria:** XYZ values change significantly and plausibly during movement. Values stabilize when watch is set down.

**Result:**
```
At rest (Y-axis vertical):   X≈0, Y≈-512, Z≈0
During movement:             values change dynamically, reaching ±400+ on active axes
After settling:              values return to stable resting values
```

Y-axis reading of approximately -512 LSB at rest corresponds to approximately -1g — correct for gravity acting on the Y axis in the watch's resting orientation. This confirms not just that data is being read, but that the data is physically meaningful and the scaling is correct.

**Test 4 — No polling loops:**

**Method:** Code review. Searched for `while(1)` loops outside of `bma423_task`'s intentional top-level loop.

**Pass criteria:** No busy-polling loop checking a data-ready flag. All task blocking via `xQueueReceive` with `portMAX_DELAY`.

**Result:** Confirmed. ✅

---

## 33.5 Level 1 Recovery Validation

**Context:** The ESP-IDF legacy I²C driver produces periodic `ESP_FAIL (0x107)` errors at approximately 1.5 second intervals. This is the naturally-occurring failure that Level 1 was designed to handle.

**Test:** Ran the driver for an extended period with retry logging enabled.

**Pass criteria:**
- Failure detected and logged correctly
- Retry 1 succeeds
- Normal operation resumes immediately after retry
- No data corruption before or after failure event

**Result:**
```
[DEBUG] i2c_master_cmd_begin error in i2c_read: 0x107
[DEBUG] i2c_read failed inside bma423_read_accel: 1
X=89 Y=-505 Z=-62
[RECOVERY L1] Retry 1 succeeded
X=89 Y=-504 Z=-62   ← normal operation resumed
```

**Observed across multiple failure events:** Retry 1 succeeded 100% of the time. Retry 2 and 3 were never needed for this failure mode. Normal operation resumed within 10 ms of each failure event. No data corruption was observed in the samples surrounding each failure event.

**ODR experiment:** Changed ODR from 25 Hz to 50 Hz. Two consecutive failures appeared per event instead of one — confirming the failure window is time-based (~20–30 ms) rather than transaction-count-based. This characterization confirmed the root cause hypothesis.

---

## 33.6 Level 2 and Level 3 Recovery Validation

**Context:** Level 2 and Level 3 do not trigger under normal operation — Level 1 always succeeds for the observed ESP-IDF failure mode. Fault injection was required to validate these paths.

**Fault injection mechanism:**

```c
// In bma423_read_accel() — removed from production build
if (g_inject_fault) {
    g_fault_count++;
    printf("[FAULT INJECT] Forced failure #%d\n", g_fault_count);
    return BMA423_ERR_BUS;
}
```

`g_inject_fault` set to `true` in `main.c` immediately after `bma423_isr_init()`.

**Pass criteria:**
- Level 1: 3 retries all fail, escalation to Level 2 logged
- Level 2: Re-initialization executes, chip ID reads correctly, all registers reconfigured, verification read fails (fault still active), escalation to Level 3 after 3 attempts
- Level 3: CRITICAL logged, task suspends, system continues running
- No crash, no hang, no watchdog reset at any level

**Result:**
```
[FAULT INJECT] Forced failure #1
[FAULT INJECT] Forced failure #2
[RECOVERY L1] Retry 1 failed
[FAULT INJECT] Forced failure #3
[RECOVERY L1] Retry 2 failed
[FAULT INJECT] Forced failure #4
[RECOVERY L1] Retry 3 failed
[RECOVERY L1] All retries exhausted
[RECOVERY L2] Attempting sensor reinitialization...
[CHIP_ID] Read = 0x13
[CHIP_ID] BMA423 detected correctly
[ERR_REG] = 0x00
[INT_MAP] = 0x04
[PWR_CTRL] Accelerometer enabled
[RECOVERY L2] Re-init 1/3 succeeded
[FAULT INJECT] Forced failure #5
[RECOVERY L2] Verification read failed
[CHIP_ID] Read = 0x13
...
[RECOVERY L2] Re-init 3/3 succeeded
[FAULT INJECT] Forced failure #7
[RECOVERY L2] Verification read failed
=====================================
[CRITICAL] BMA423 recovery failed
[CRITICAL] Sensor subsystem offline
[CRITICAL] Suspending BMA423 task
=====================================
I (1521) main_task: Returned from app_main()
```

All pass criteria met. ✅

**What the fault injection result confirmed beyond recovery logic:**

The re-initialization during Level 2 succeeded completely — chip ID verified, all registers reconfigured and verified — while the application-level read failed. This confirms that the recovery architecture correctly separates sensor-level health (the sensor is alive and configurable) from application-level health (data reads are succeeding). A sensor that responds to configuration but not to data reads is a distinct failure state that the recovery ladder handles correctly.

---

## 33.7 Mutex Behavior Validation

**Test 1 — Mutex prevents concurrent access:**

**Method:** Confirmed via logging that no two I²C transactions overlap. With a single `bma423_task` and no other task performing I²C operations during the test window, this was implicitly validated by the absence of bus errors during normal operation.

**Test 2 — Mutex release on error path:**

This was validated by finding the bug (Section 29.5) and confirming the fix. The bug manifested as total I²C bus loss after the first failed transaction — an unambiguous symptom of a permanently held mutex. After the fix, the bus scan produces expected results and normal operation continues through bus errors.

**Test 3 — Mutex timeout:**

Not directly triggered during testing. The 50 ms timeout was not hit in any observed scenario. This remains a partially validated path — the timeout code exists and compiles correctly, but its runtime behavior was not observed.

---

## 33.8 Data Validity Validation

**Physical orientation test:**

The ±4g range with 12-bit resolution produces approximately 1.95 mg/LSB. At 1g gravitational acceleration, the expected reading is approximately:

```
1000 mg / 1.95 mg/LSB ≈ 512 LSB
```

With the watch resting with its Y axis aligned vertically:

```
Expected Y reading: ≈ ±512 LSB
Observed Y reading: ≈ -505 to -512 LSB ✅
```

The sign (negative) is consistent with gravity acting in the negative Y direction for the watch's physical orientation. The magnitude is within 1.5% of the theoretical value — within the BMA423's specified offset tolerance.

**Sign extension validation:**

Negative values (Y ≈ -512) are correctly represented as negative `int16_t` values. If sign extension were broken, negative values would appear as large positive numbers (~3584 for -512 in a 12-bit unsigned interpretation). The correct negative output confirms sign extension works.

**Dynamic response validation:**

During active movement, values reached ±400–600 LSB on multiple axes simultaneously — consistent with accelerations of approximately 0.8–1.2g during typical wrist motion. Values returned to stable resting readings within one or two sample periods after movement stopped — consistent with the 4-sample averaging filter's response time at 25 Hz.

---

## 33.9 What Was Not Validated

The following were identified as gaps in the validation process. They are documented here rather than omitted, consistent with the principle that known gaps are safer than undocumented assumptions.

**Stack high-water mark:** `uxTaskGetStackHighWaterMark()` was not called. The 2048 word stack allocation was not verified against actual usage. Recommended as a first measurement in any production deployment.

**Mutex timeout path:** The 50 ms mutex timeout was not triggered during testing. The code path that returns `I2C_ERR_TIMEOUT` was not exercised at runtime.

**Physical disconnect test:** The sensor was not physically disconnected from the I²C bus during operation. Level 2 and Level 3 were validated only via software fault injection. Physical disconnect may produce different failure signatures — bus stuck conditions, different error codes — that the recovery ladder handles differently than software-injected failures.

**Temperature and voltage variation:** All testing was performed at room temperature with nominal supply voltage. Embedded systems often exhibit different behavior at temperature extremes or marginal supply voltages. No characterization was performed under these conditions.

**Long-duration stability:** The driver was not run for extended periods (hours or days) to check for memory leaks, stack growth, or failure rate changes over time.

