# 03 — Objectives

## 3.1 Primary Objective

Design and implement a register-level BMA423 accelerometer driver on ESP32
that acquires data purely through hardware interrupts, and that detects and
recovers from I2C bus faults and sensor faults without operator intervention
or MCU crash/hang.

## 3.2 Specific Objectives

1. **Direct register-level control of the BMA423**
   Every configuration step (soft reset, chip ID verification, `ACC_CONF`,
   `ACC_RANGE`, `INT1_IO_CTRL`, `INT_MAP`, `PWR_CONF`, `PWR_CTRL`) is performed
   through explicit register writes against the datasheet map — no vendor
   BMA423 driver or Bosch Sensor API in the loop.

2. **Write verification, not write-and-hope**
   Every configuration register write in `bma423_init()` is immediately
   read back and compared against the intended value before proceeding.
   A write that doesn't stick is treated as a config failure
   (`BMA423_ERR_CONFIG`), not silently ignored.

3. **Fully event-driven acquisition**
   No polling loop anywhere in the steady-state read path. A GPIO interrupt
   on the BMA423's INT1 line is the only trigger for a read. The ISR itself
   does no I2C work — it only signals a FreeRTOS task via a queue
   (`bma423_isr_handler` → `accel_event_queue` → `bma423_task`).

4. **Correct 12-bit signed data reconstruction**
   The BMA423 outputs 12-bit signed acceleration data packed across LSB/MSB
   register pairs. Objective: reconstruct and sign-extend this correctly,
   not just byte-concatenate and treat as 16-bit unsigned.

5. **Tiered fault detection and recovery**
   Define and implement a bounded recovery ladder for I2C read failures:
   - Tier 1: bounded retry of the read operation (`MAX_RETRY_COUNT`)
   - Tier 2: sensor re-initialization if retries are exhausted
     (`MAX_REINIT_COUNT`)
   - Tier 3: controlled task suspension (`vTaskSuspend`) if re-init also
     fails — the system reports the subsystem as offline rather than
     spinning, crashing, or silently going quiet.

6. **Shared-bus safety**
   The I2C transport layer must be safe to call concurrently from multiple
   subsystems (BMA423 driver, AXP202 PMIC driver) on the same physical bus,
   enforced via a FreeRTOS mutex (`i2c_mutex`) with a bounded acquisition
   timeout.

7. **Diagnostic visibility during bring-up and fault injection**
   Provide an I2C bus scanner (`i2c_scan()`) and structured logging at every
   register operation, so failures are debuggable from serial output alone
   without a logic analyzer.

## 3.3 Explicitly Deferred Objectives

Stated up front to avoid overstating scope in later sections:

| Deferred item | Status |
|---|---|
| FIFO batch read support | Not implemented |
| BMA423 low-power/suspend mode transitions | Not implemented |
| ESP32 light/deep sleep integration | Not implemented |
| Multi-sensor fusion | Out of scope by design |
| Cloud/companion app integration | Out of scope by design |

These are addressed as forward-looking items in Section 35 (Future
Improvements), not retrofitted into this section as if they exist.

## 3.4 Success Criteria

The objectives above are considered met when:

- The driver reads correct, correctly-scaled XYZ data continuously under
  normal operation, triggered only by hardware interrupts.
- Injected I2C faults (NACK, bus busy) are observably caught, logged, and
  recovered from per the tiered recovery ladder, without an MCU reset.
- A sensor-level fault (failed chip ID check, `ERR_REG` non-zero,
  configuration verification failure) is detected during init and reported
  with enough log detail to diagnose without re-flashing.

