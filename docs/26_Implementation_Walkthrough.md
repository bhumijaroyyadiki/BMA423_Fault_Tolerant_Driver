# 26_Implementation_Walkthrough.md

This section walks through the actual implementation chronologically — not as a tutorial, but as an honest account of how the driver was built, what order decisions were made in, and where the implementation diverged from the original plan. A reader who wants to understand why the code looks the way it does, rather than just what it does, should read this section.

---

## 26.1 Starting Point — What Existed Before

The project began with a working smartwatch firmware base that handled Wi-Fi, battery monitoring, step counting via a vendor library, and basic display output. That codebase was deliberately set aside. The goal was not to integrate a feature into existing firmware — it was to build a driver from scratch that could stand alone as a demonstrable artifact.

The starting constraint was strict: no vendor BMA423 driver, no Arduino Wire library, no abstraction hiding the register map. Every byte sent to the sensor had to be understood before it was sent.

---

## 26.2 Phase 1 — Datasheet Before Code

The first two sessions produced no code. They produced a register map.

Every register used in this driver was located in the BMA423 datasheet, read in full, and its bit fields decoded manually. This included:

- Finding that `CHIP_ID` at `0x00` returns `0x13` — and understanding why reading this at startup is a verification step, not a formality
- Decoding `ERR_REG` at `0x02` as containing three distinct error categories in a non-contiguous bit layout — `fatal_err` at bit 0, `cmd_err` at bit 1, and `error_code` as a 3-bit field at bits 4:2
- Finding that `STATUS` at `0x03` has a reset value of `0x10` — meaning `cmd_rdy` (bit 4) is set at power-on, and the sensor is immediately ready to accept commands without an explicit wait
- Finding that `ACC_CONF` reset value `0xA8` decodes to `0x0A` in the BWP field — which is not a valid value in the datasheet table, indicating the reset default is not a meaningful operating configuration and must be explicitly overwritten
- Confirming that `ACC_RANGE` reset default is `0x01` (±4g) — the intended value — and deciding to write it explicitly anyway rather than rely on the default

This phase established the principle that governed the rest of the implementation: every register write must be justified by a datasheet parameter, and every assumption must be verified rather than inherited.

---

## 26.3 Phase 2 — I²C Layer First

`i2c.c` was written before any sensor code. The rationale was explicit: if the communication layer is not provably correct, no sensor behavior can be trusted.

The initial implementation followed the ESP-IDF legacy I²C driver transaction model:

```
i2c_cmd_link_create()
→ i2c_master_start()
→ i2c_master_write_byte()  [address + W]
→ i2c_master_write_byte()  [register]
→ i2c_master_start()       [repeated START]
→ i2c_master_write_byte()  [address + R]
→ i2c_master_read()        [n-1 bytes, ACK]
→ i2c_master_read_byte()   [final byte, NACK]
→ i2c_master_stop()
→ i2c_master_cmd_begin()
→ i2c_cmd_link_delete()
```

The repeated-START between write and read phases was implemented correctly from the beginning — a deliberate choice after reading the I²C specification, which specifies that a STOP followed by a START between address write and data read is not equivalent and can cause some devices to lose their internal register pointer.

The initial timeout was set to 1000 ms — a placeholder that was immediately identified as excessive (a single transaction at 400 kHz takes approximately 75 µs) and later reduced to 10 ms.

An I²C bus scan was added as a diagnostic tool during this phase. It confirmed the BMA423 at address `0x19` — not the assumed `0x18`. The SDO pin on the TTGO T-Watch 2020 V3 is pulled high, selecting the alternate address. This was caught at the scan stage rather than as a mysterious initialization failure.

---

## 26.4 Phase 3 — Sensor Initialization

`bma423_init()` was built incrementally — one step at a time, each verified with serial output before the next step was added.

**Step order rationale:**

Soft reset was placed first deliberately — not as a formality but as a guarantee. Without it, the driver inherits whatever register state the sensor was left in by a prior boot, partial initialization, or watchdog reset. Starting from a known state is non-negotiable in a robust driver.

The dummy read after reset was added after observing that the first chip ID read after a soft reset occasionally returned garbage. The BMA423 datasheet does not explicitly document a required dummy read, but the behavior is consistent with the sensor's internal state machine completing its NVM load before responding correctly to I²C. The dummy read result is explicitly discarded — it is not an error if it fails.

Read-back verification was added to every configuration write after reasoning about what an I²C ACK actually guarantees: only that the device received the byte on the wire. It does not guarantee the device's internal register logic applied it. The verification read-back catches the gap between those two facts.

The PWR_CTRL register write enabling the accelerometer was the last step added — and the most consequential omission during development. Without it, the sensor initializes correctly, all registers verify correctly, interrupts fire correctly, but all data registers return zero. The accelerometer is disabled by default after reset and must be explicitly enabled. This was discovered empirically when the driver produced valid-looking but zeroed output.

---

## 26.5 Phase 4 — Interrupt Architecture

The interrupt pipeline was designed top-down: the application need (non-blocking data delivery) determined the architecture (ISR → queue → task), which determined the implementation details.

**ISR minimalism** was not a stylistic preference — it was a hard constraint. `i2c_read()` acquires a FreeRTOS mutex. Mutex acquisition inside an ISR is illegal in FreeRTOS and will corrupt the scheduler. This means any approach that involves reading sensor data inside the ISR is not merely inadvisable but will produce undefined behavior in production. The ISR therefore does exactly one thing: `xQueueSendFromISR()`.

**Queue depth** of 5 was chosen to provide approximately 200 ms of scheduling slack at 25 Hz before overflow. In practice, FreeRTOS task scheduling jitter on this platform is measured in single-digit milliseconds — the queue never fills during normal operation.

**File separation** between `bma423.c` and `bma423_isr.c` was not planned upfront. During implementation it became clear that ISR setup involves GPIO numbers, FreeRTOS primitives, and ESP-IDF interrupt APIs — all platform-specific details that had no place in the sensor driver layer. The split was made mid-implementation and required no changes to `bma423.c` — confirming the layering was already clean.

---

## 26.6 Phase 5 — Discovering the 0x107 Failure

After the interrupt pipeline was working and producing correct XYZ output, a periodic failure appeared in the logs:

```
[DEBUG] i2c_master_cmd_begin error in i2c_read: 0x107
[BMA423_TASK] Read failed: 1
```

The diagnostic process followed a strict hypothesis-test-result cycle:

**Hypothesis 1:** Bus contention between `bma423_task` and other tasks sharing the I²C bus.
**Test:** Added FreeRTOS mutex around all I²C transactions.
**Result:** Mutex confirmed working via logging. Failure persisted. Ruled out.

**Hypothesis 2:** I²C timeout too short, causing premature transaction abort.
**Test:** Increased timeout from 10 ms to 50 ms.
**Result:** No change in failure rate or timing. Ruled out.

**Hypothesis 3:** Transient internal ESP-IDF legacy driver timing behavior.
**Test:** Added 10 ms retry delay and retried once on failure.
**Result:** Retry 1 succeeded 100% of the time. Failure is consistently transient.

**Characterization experiment:** Changed ODR from 25 Hz to 50 Hz. At 25 Hz, one read was affected per failure window. At 50 Hz, two consecutive reads were affected — confirming the failure window is approximately 20–30 ms wide and fixed in time, not fixed in transaction count.

**Conclusion:** The failure occurs at a regular ~1.5 second interval with a ~20–30 ms window during which `i2c_master_cmd_begin` returns `ESP_FAIL`. This is consistent with an internal semaphore or timer reset cycle inside the ESP-IDF legacy I²C driver — corroborated by the boot-time warning recommending migration to `driver/i2c_master.h`. The workaround (Level 1 retry) handles it completely. Migration to the new driver was considered and deferred — documented in Section 34.

---

## 26.7 Phase 6 — Recovery Ladder

With the failure characterized, the recovery architecture was designed before any recovery code was written. Three questions were answered explicitly:

1. How many retries before escalating? → 3, with 10 ms delay, break on first success
2. What does escalation mean? → `bma423_init()` only — not I²C driver restart, which would disrupt AXP202 and PCF8563 on the shared bus
3. What happens if escalation fails? → `vTaskSuspend(NULL)` — task stops, system continues

The three levels were implemented in `bma423_task()` — not in `bma423_read_accel()`. This was a deliberate architectural decision: recovery logic belongs at the layer that can coordinate resources safely. `bma423_read_accel()` reports failure and exits cleanly. `bma423_task()` decides what to do about it.

Fault injection was used to verify Levels 2 and 3, which do not trigger under normal operation. A `g_inject_fault` flag in `bma423_read_accel()` forced unconditional `BMA423_ERR_BUS` returns, confirming:

- Level 1 exhausted correctly after 3 failed retries
- Level 2 re-initialized the sensor correctly but continued failing (fault still active)
- Level 3 triggered after Level 2 exhaustion, logged CRITICAL, and suspended the task
- The system continued running after task suspension — no crash, no hang

---

## 26.8 What the Implementation Taught That the Plan Did Not Anticipate

Three things emerged during implementation that were not in the original design:

**The PWR_CTRL register.** The original plan assumed initialization meant configuring ODR, range, and interrupts. It did not account for the accelerometer being disabled by default. This was discovered empirically — a working driver producing zeroed data — and added as an explicit initialization step.

**The 0x107 periodic failure.** No amount of upfront planning would have predicted a periodic ESP-IDF internal driver failure at a 1.5 second interval. It was found through observation, characterized through experiment, and handled through a recovery architecture that was designed around its specific properties.

**The value of layering under pressure.** The I²C layer was modified three times after the sensor driver was complete — timeout change, mutex addition, mutex resource-leak fix. In each case, `bma423.c` was untouched. This was not luck — it was the direct result of the boundary rule that `bma423.c` contains no ESP-IDF types and calls only `i2c_read()` and `i2c_write()`. The architecture absorbed three implementation changes with zero ripple effect.

