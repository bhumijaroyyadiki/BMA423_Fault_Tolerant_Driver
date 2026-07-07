# 04 — Functional Requirements

Functional requirements are written against what the code in `bma423.c`,
`bma423_isr.c`, `i2c.c`, and `power.c` actually does — not aspirational
behavior.

## FR-1: I2C Transport Layer

| ID | Requirement |
|---|---|
| FR-1.1 | The system shall provide `i2c_init()` to configure the ESP32 I2C master peripheral (SDA=GPIO21, SCL=GPIO22, 400kHz) before any device communication. |
| FR-1.2 | The system shall provide `i2c_write(addr, reg, data, len)` and `i2c_read(addr, reg, data, len)` as the sole transaction primitives used by all higher layers (BMA423 driver, AXP202 driver). |
| FR-1.3 | All I2C transactions shall be serialized through a single mutex (`i2c_mutex`) to prevent concurrent bus access from separate tasks/ISR-deferred contexts. |
| FR-1.4 | Mutex acquisition shall time out (50 ms) rather than block indefinitely, returning `I2C_ERR_TIMEOUT` on failure to acquire. |
| FR-1.5 | Each I2C transaction shall itself time out (10 ms) at the ESP-IDF driver level (`i2c_master_cmd_begin`), returning `I2C_ERR_BUS` on failure rather than hanging. |
| FR-1.6 | The system shall provide an I2C bus scanner (`i2c_scan()`) for bring-up diagnostics, enumerating all responding addresses in the 0x08–0x77 range. |

## FR-2: BMA423 Initialization

| ID | Requirement |
|---|---|
| FR-2.1 | The system shall issue a soft reset (`0xB6` to `CMD_REG`) as the first step of initialization. |
| FR-2.2 | The system shall wait a fixed settling period (50 ms) after soft reset before further register access, per datasheet reboot timing. |
| FR-2.3 | The system shall perform a dummy chip-ID read immediately after reset (to clear residual reset state) before the chip-ID read that is actually evaluated. |
| FR-2.4 | The system shall read and verify `CHIP_ID` against the expected value (`0x13`) and shall fail initialization (`BMA423_ERR_CHIP_ID`) if it does not match. |
| FR-2.5 | The system shall read `ERR_REG` after chip-ID verification and shall classify and fail on fatal, command, or configuration error bits (`BMA423_ERR_FATAL` / `BMA423_ERR_CMD` / `BMA423_ERR_CONFIG`). |
| FR-2.6 | The system shall configure `ACC_CONF` (performance mode, bandwidth, output data rate) and verify the write via read-back before proceeding. |
| FR-2.7 | The system shall configure `ACC_RANGE` (±4g) and verify the write via read-back before proceeding. |
| FR-2.8 | The system shall configure `INT1_IO_CTRL` (push-pull, active-high, edge-triggered, output enabled) and verify the write via read-back before proceeding. |
| FR-2.9 | The system shall map the data-ready interrupt to INT1 via `INT_MAP` and verify the write via read-back before proceeding. |
| FR-2.10 | The system shall disable advanced power save (`PWR_CONF = 0x00`) before enabling the accelerometer. |
| FR-2.11 | The system shall enable the accelerometer via `PWR_CTRL` and verify the write via read-back; the sensor shall not be assumed active by default. |
| FR-2.12 | The system shall enforce a minimum 1000 µs inter-write delay between successive configuration register writes, per datasheet timing requirements, using a busy-wait (`BMA423_DELAY_US`), not a scheduler-based delay. |

## FR-3: Interrupt-Driven Data Acquisition

| ID | Requirement |
|---|---|
| FR-3.1 | The system shall configure a GPIO interrupt (GPIO39) on the BMA423 INT1 line, positive-edge triggered. |
| FR-3.2 | The ISR shall perform no I2C transactions; it shall only post an event to a FreeRTOS queue and yield if a higher-priority task was woken. |
| FR-3.3 | A dedicated FreeRTOS task shall block on the event queue and perform the actual accelerometer read upon event receipt. |
| FR-3.4 | The system shall apply a fixed 100 µs settling delay before reading data registers after an interrupt, to account for the possibility that the interrupt can fire slightly before internal ADC data is fully latched. |
| FR-3.5 | The system shall perform a 6-byte burst read of `ACC_X_LSB` through `ACC_Z_MSB` in a single I2C transaction. |
| FR-3.6 | The system shall reconstruct each axis as a 12-bit value from its LSB/MSB register pair and sign-extend it correctly to a 16-bit signed integer. |
| FR-3.7 | The read function shall validate output pointer arguments and return `BMA423_ERR_INVALID_ARG` if any are NULL. |

## FR-4: Fault Detection and Recovery

| ID | Requirement |
|---|---|
| FR-4.1 | On a failed accelerometer read, the system shall retry the read up to `MAX_RETRY_COUNT` (3) times with a fixed inter-retry delay (10 ms). |
| FR-4.2 | If all retries fail, the system shall attempt sensor re-initialization up to `MAX_REINIT_COUNT` (3) times, with a fixed inter-attempt delay (50 ms). |
| FR-4.3 | After a successful re-init, the system shall immediately perform a verification read before resuming normal operation. |
| FR-4.4 | If re-initialization and verification both fail across all attempts, the system shall log a critical failure state and suspend the acquisition task (`vTaskSuspend`) rather than crash, reset, or spin indefinitely. |

## FR-5: Power Rail Sequencing (AXP202)

| ID | Requirement |
|---|---|
| FR-5.1 | The system shall read and log the current state of the AXP202 power-output-control register (DC3, LDO2, LDO3) during `power_init()` for diagnostic purposes. |
| FR-5.2 | The system shall never attempt to disable DC3 (the rail powering the ESP32 itself). |
| FR-5.3 | The system shall provide independent, bit-verified enable functions for LDO2 (backlight) and LDO3 (audio/backplane), skipping the write if the rail is already enabled. |
| FR-5.4 | The system shall apply a 50 ms stabilization delay after power init to allow BMA423 NVM load to complete (datasheet minimum: 2 ms; implemented with margin). |

## FR-6: Initialization Ordering

| ID | Requirement |
|---|---|
| FR-6.1 | The system shall initialize the I2C bus before any device-specific initialization. |
| FR-6.2 | The system shall initialize power rails (`power_init()`) before sensor hardware configuration. |
| FR-6.3 | The system shall fully complete and verify BMA423 hardware configuration (`bma423_init()`) before installing the GPIO ISR and starting the acquisition task (`bma423_isr_init()`) — hardware must be known-good before interrupts are allowed to fire against it. |
