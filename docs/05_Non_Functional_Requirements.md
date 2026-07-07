# 05 — Non-Functional Requirements

Non-functional requirements describe *how well* the system meets its
functional requirements — timing behavior, resource cost, robustness under
degraded conditions, and maintainability. Each item below is backed by
something observable in the code, not a generic embedded-systems checklist.

## NFR-1: Timing

| ID | Requirement | Basis |
|---|---|---|
| NFR-1.1 | Inter-register-write delay during BMA423 configuration shall be ≥1000 µs, using a busy-wait, not a tick-based delay. | `BMA423_DELAY_US(1000)` between config writes; `bma423_platform.h` explicitly documents why `vTaskDelay` is unsafe here (10 ms tick granularity at 100 Hz tick rate would blow past the 1 ms datasheet window, and scheduler preemption adds unbounded jitter on top). |
| NFR-1.2 | Post-interrupt data read shall be preceded by a 100 µs settling delay, not zero delay and not a scheduler delay. | `BMA423_DELAY_US(100)` in `bma423_read_accel()`, chosen specifically to avoid slow status-polling as an alternative. |
| NFR-1.3 | I2C bus transactions shall complete or time out within 10 ms. | `i2c_master_cmd_begin(..., pdMS_TO_TICKS(10))` in both `i2c_read` and `i2c_write`. |
| NFR-1.4 | Mutex acquisition for bus access shall not block longer than 50 ms. | `xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50))` in both transaction functions. |
| NFR-1.5 | Post-reset settling time shall be 50 ms before the sensor is considered ready for chip-ID verification. | Datasheet specifies internal reboot time; 50 ms is used as safety margin in `bma423_init()`. |

## NFR-2: Robustness / Fault Tolerance

| ID | Requirement | Basis |
|---|---|---|
| NFR-2.1 | No single I2C transaction failure shall cause an unhandled crash, hang, or silent data corruption. | Every `i2c_read`/`i2c_write` call site checks the return status explicitly. |
| NFR-2.2 | The system shall distinguish between transient faults (recoverable via retry) and persistent faults (requiring re-init or shutdown). | Three-tier recovery ladder in `bma423_task`. |
| NFR-2.3 | The system shall degrade to a defined, observable "offline" state rather than an undefined one when recovery is exhausted. | `vTaskSuspend(NULL)` with explicit `[CRITICAL]` log block, not a silent `return`. |
| NFR-2.4 | Configuration correctness shall be verified at write time, not assumed. | Read-back verification after every configuration register write in `bma423_init()`. |

## NFR-3: Concurrency Safety

| ID | Requirement | Basis |
|---|---|---|
| NFR-3.1 | The I2C bus shall be safe for use by multiple independent subsystems without corrupting in-flight transactions. | `i2c_mutex` wraps every transaction in `i2c.c`; both `bma423.c` and `power.c` route through the same `i2c_read`/`i2c_write` entry points. |
| NFR-3.2 | ISR-to-task handoff shall not perform blocking or bus operations inside interrupt context. | `bma423_isr_handler` only calls `xQueueSendFromISR` and conditionally `portYIELD_FROM_ISR` — no I2C calls in ISR context. |

## NFR-4: Resource Usage

| ID | Requirement | Basis |
|---|---|---|
| NFR-4.1 | The acquisition task shall run with a bounded, fixed stack allocation. | `xTaskCreate(bma423_task, ..., 2048, ...)` — fixed 2KB stack, not dynamically sized. |
| NFR-4.2 | The ISR-to-task event queue shall carry minimal payload (signal only, not data). | Queue element is a single `uint8_t` dummy value — the actual read happens in task context, not passed through the queue. |
| NFR-4.3 | Queue depth shall bound the number of pending un-processed interrupt events. | `ACCEL_QUEUE_DEPTH = 10` — bounds memory use and defines backpressure behavior if the task falls behind. |

## NFR-5: Observability / Debuggability

| ID | Requirement | Basis |
|---|---|---|
| NFR-5.1 | Every register operation during initialization shall log its target register and before/after value where a state change is involved. | `printf` before/after logging throughout `bma423_init()` and `axp202_set_bit()`. |
| NFR-5.2 | Bus topology shall be independently verifiable via a diagnostic scan, without requiring the higher-level drivers to be functioning. | `i2c_scan()` operates directly on raw addresses, independent of `bma423_init()`/`power_init()` success. |
| NFR-5.3 | Recovery attempts shall be individually distinguishable in logs (which tier, which attempt number, success/failure). | `[RECOVERY L1]` / `[RECOVERY L2]` tagged logs with retry/attempt counters in `bma423_task`. |

## NFR-6: Portability

| ID | Requirement | Basis |
|---|---|---|
| NFR-6.1 | Platform-specific timing primitives shall be isolated behind a single header, not scattered through driver logic. | `bma423_platform.h` isolates `BMA423_DELAY_US` as the only platform-coupled primitive in the BMA423 protocol layer. |
| NFR-6.2 | Porting to a different MCU family shall require changes only to the I2C transport layer (`i2c.c`) and the platform delay header, not to `bma423.c` or `bma423_isr.c` register logic. | `bma423.c` calls only `i2c_read`/`i2c_write`/`BMA423_DELAY_US` — no direct ESP-IDF calls in the protocol layer itself. *(Caveat, addressed honestly in Section 34: `bma423_isr.c` currently calls ESP-IDF GPIO/FreeRTOS APIs directly, so the ISR/task layer is not yet as portable as the register-protocol layer.)* |

## NFR-7: Maintainability

| ID | Requirement | Basis |
|---|---|---|
| NFR-7.1 | Register addresses, bit positions, and bit masks shall be defined as named constants, not magic numbers, at call sites. | Full bitfield/position/mask definitions in `bma423_regs.h`. |
| NFR-7.2 | Rail-enable logic for the PMIC shall be implemented once as a generic, reusable primitive rather than duplicated per rail. | `axp202_set_bit()` is a single generic bit-set-with-verify helper reused by `axp202_enable_ldo2()` and `axp202_enable_ldo3()`. |
