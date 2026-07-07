# 24_Code_Organization.md

This section describes how the codebase is physically structured, what lives where and why, and what conventions govern the boundaries between files. The goal is that any engineer picking up this project can locate any piece of logic without reading everything.

---

## 24.1 File Structure

```
project/
├── main/
│   ├── main.c               — Entry point. Orchestration only.
│   ├── i2c.c                — I²C bus transactions, mutex, timeout
│   ├── i2c.h                — Public I²C interface
│   ├── power.c              — AXP202 PMIC init, rail control, bus scan
│   ├── power.h              — Public power interface
│   ├── bma423.c             — Sensor driver: init, read, error codes
│   ├── bma423.h             — Public sensor interface + status enum
│   ├── bma423_regs.h        — Register addresses, bit positions, masks, values
│   ├── bma423_isr.c         — GPIO config, ISR handler, FreeRTOS queue, task
│   ├── bma423_isr.h         — Public ISR interface
│   └── bma423_platform.h    — Platform-specific abstractions (delay macros)
```

---

## 24.2 Responsibility of Each File — One Sentence Each

| File | Single Responsibility |
|---|---|
| `main.c` | Calls init functions in order, checks results, does nothing else |
| `i2c.c` | Owns all wire-level I²C transactions and the mutex protecting them |
| `i2c.h` | Declares the three functions (`i2c_init`, `i2c_read`, `i2c_write`) and `i2c_status_t` |
| `power.c` | Owns AXP202 register access and power rail state |
| `power.h` | Declares `power_init()` and AXP202 helper functions |
| `bma423.c` | Owns sensor initialization sequence and acceleration data reads |
| `bma423.h` | Declares the driver API and `bma423_status_t` error enum |
| `bma423_regs.h` | Single source of truth for every BMA423 register address, bit position, mask, and named value |
| `bma423_isr.c` | Owns everything needed to connect the sensor interrupt to a FreeRTOS task |
| `bma423_isr.h` | Declares `bma423_isr_init()` — the only symbol `main.c` needs from this layer |
| `bma423_platform.h` | Isolates ESP-IDF-specific delay calls behind macros so `bma423.c` stays portable |

---

## 24.3 The Boundary Rules

Three rules govern what goes where. These were not written down upfront — they emerged from the architecture and are reconstructed here for clarity.

**Rule 1 — `main.c` contains no hardware knowledge.**
`main.c` does not know GPIO numbers, I²C addresses, register names, or FreeRTOS primitives. It calls `i2c_init()`, `power_init()`, `bma423_init()`, `bma423_isr_init()` — in that order — checks the return value of each, and stops if any fails. Nothing else. This rule means `main.c` never needs to change when hardware details change.

**Rule 2 — `bma423.c` contains no ESP-IDF types.**
`bma423.c` does not include `esp_err.h`, does not use `esp_err_t`, and does not call any ESP-IDF function directly. All ESP-IDF interaction is handled by `i2c.c`, which translates ESP-IDF error codes into `i2c_status_t` at the boundary. `bma423.c` sees only `I2C_OK` or `I2C_ERR_*` — it has no knowledge of what platform is underneath. This is what makes the sensor driver genuinely portable.

**Rule 3 — `bma423_regs.h` is the only place register addresses appear.**
No raw hex register address (`0x40`, `0x7D`, etc.) appears in any `.c` file. Every register access uses a named constant from `bma423_regs.h`. This means a register address error is fixed in exactly one place, not hunted across multiple files.

---

## 24.4 Why `bma423_isr.c` Is Separate From `bma423.c`

This was a deliberate decision made during implementation, not upfront planning. The original design had ISR setup inside `bma423.c`. It was separated for three reasons:

First, `bma423.c` deals with sensor register logic — a concern that is entirely platform-agnostic. ISR setup involves GPIO numbers, FreeRTOS queue creation, and ESP-IDF interrupt service APIs — all platform-specific. Mixing them would contaminate the portable layer with ESP32 specifics.

Second, `bma423_isr_init()` manages four distinct resources (queue, GPIO, ISR service, task) with a cleanup ladder on failure. That complexity belongs in its own file where it can be read and reasoned about independently.

Third, in a future port to STM32, `bma423.c` moves unchanged. Only `i2c.c` and `bma423_isr.c` need STM32-specific rewrites. The separation makes the porting surface explicit.

---

## 24.5 `bma423_regs.h` — Structure and Conventions

This file follows a consistent pattern for every register:

```c
// Register address
#define BMA423_ACC_CONF_REG         0x40

// Field values (unshifted — raw field value, not shifted into position)
#define ACC_PERF_MODE_AVG           0x00
#define ACC_BWP_AVG_4               0x02
#define ACC_ODR_25HZ                0x06

// Bit positions
#define ACC_CONF_PERF_MODE_POS      7
#define ACC_CONF_BWP_POS            4
#define ACC_CONF_ODR_POS            0

// Shifted masks (for read-modify-write and verification)
#define ACC_CONF_PERF_MODE_MASK     (0x01 << ACC_CONF_PERF_MODE_POS)
#define ACC_CONF_BWP_MASK           (0x07 << ACC_CONF_BWP_POS)
#define ACC_CONF_ODR_MASK           (0x0F << ACC_CONF_ODR_POS)
```

The separation between unshifted values and shifted masks is intentional. Register construction uses:

```c
uint8_t reg = ((ACC_PERF_MODE_AVG & 0x01) << ACC_CONF_PERF_MODE_POS) |
              ((ACC_BWP_AVG_4     & 0x07) << ACC_CONF_BWP_POS)       |
              ((ACC_ODR_25HZ      & 0x0F) << ACC_CONF_ODR_POS);
```

Masking before shifting guards against a corrupt or out-of-range value contaminating adjacent bit fields — a class of bug that produces silent misconfiguration with no runtime error.

---

## 24.6 Naming Conventions

| Pattern | Example | Meaning |
|---|---|---|
| `BMA423_*_REG` | `BMA423_ACC_CONF_REG` | Register address |
| `BMA423_*_CMD` | `BMA423_SOFT_RESET_CMD` | Command value written to a register |
| `ACC_*_POS` | `ACC_CONF_ODR_POS` | Bit position of a field (LSB of field) |
| `ACC_*_MASK` | `ACC_CONF_ODR_MASK` | Shifted bitmask for that field |
| `ACC_*_VAL` / plain name | `ACC_ODR_25HZ` | Named field value, unshifted |
| `BMA423_ERR_*` | `BMA423_ERR_BUS` | Driver-level error code |
| `I2C_ERR_*` | `I2C_ERR_TIMEOUT` | I²C layer error code |
| `[RECOVERY L1]` | log prefix | Identifies which recovery level produced the log line |

---

## 24.7 What Intentionally Does Not Exist

Two things are absent from the codebase that might be expected:

**No vendor driver dependency.** Bosch publishes a reference BMA423 driver. It is not used here. The entire register map, initialization sequence, and data path were implemented from the datasheet directly. This was a deliberate learning choice — and means there is no vendor library version dependency, no licensing concern, and no abstraction layer hiding what the hardware is actually doing.

**No Arduino framework dependency.** The project runs on ESP-IDF directly. `delay()`, `Wire.h`, and Arduino-style abstractions are absent. This was required to use FreeRTOS primitives (queues, semaphores, tasks) properly and to have deterministic control over I²C transaction structure.

