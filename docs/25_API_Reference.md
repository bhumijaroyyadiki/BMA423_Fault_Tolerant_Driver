# 25_API_Reference.md

This section documents every public function exposed by this driver — the contract each function makes with its caller, what parameters it expects, what it returns, and what the caller is responsible for. Internal static functions are not documented here.

---

## 25.1 I²C Layer — `i2c.h`

---

### `i2c_init`

```c
i2c_status_t i2c_init(void);
```

**Description:** Initializes the ESP32 I²C master driver on I2C_NUM_0 (SDA: GPIO21, SCL: GPIO22, 400 kHz) and creates the internal mutex that serializes bus access across tasks.

**Parameters:** None.

**Returns:**

| Value | Meaning |
|---|---|
| `I2C_OK` | Driver installed, mutex created, bus ready |
| `I2C_ERR_BUS` | Driver installation or parameter configuration failed |

**Caller responsibilities:**
- Must be called before any `i2c_read()` or `i2c_write()` call.
- Must be called from task context — not from ISR context.
- Must not be called more than once without an intervening `i2c_driver_delete()`.

**Side effects:** Allocates a FreeRTOS mutex. Installs the ESP-IDF I²C driver.

---

### `i2c_read`

```c
i2c_status_t i2c_read(uint8_t dev_addr,
                       uint8_t reg,
                       uint8_t *data,
                       size_t   len);
```

**Description:** Performs a standard I²C register read: writes the device address with WRITE bit, writes the register address, issues a repeated START, writes the device address with READ bit, reads `len` bytes into `data`. The final byte is NACKed as required by the I²C protocol. The entire transaction is mutex-protected.

**Parameters:**

| Parameter | Type | Description |
|---|---|---|
| `dev_addr` | `uint8_t` | 7-bit I²C device address (not shifted — shifting is handled internally) |
| `reg` | `uint8_t` | Register address to read from |
| `data` | `uint8_t *` | Caller-allocated buffer to receive the read bytes |
| `len` | `size_t` | Number of bytes to read. Must be ≥ 1 |

**Returns:**

| Value | Meaning |
|---|---|
| `I2C_OK` | Transaction completed successfully |
| `I2C_ERR_BUS` | Transaction failed — device NACKed, bus error, or ESP-IDF internal error |
| `I2C_ERR_TIMEOUT` | Mutex acquisition timed out (50 ms) — bus held by another task too long |
| `I2C_ERR_INVALID_ARG` | `len == 0` or `data == NULL` |

**Caller responsibilities:**
- `data` buffer must be allocated by the caller and must be at least `len` bytes.
- Must not be called from ISR context — acquires a mutex internally.
- Return value must be checked. A successful return does not guarantee data correctness — only that the transaction completed without a bus error.

**Notes:** The repeated-START between write phase and read phase is mandatory per I²C protocol for register-addressed reads. Issuing a STOP between the two phases would cause some devices to reset their internal register pointer.

---

### `i2c_write`

```c
i2c_status_t i2c_write(uint8_t  dev_addr,
                        uint8_t  reg,
                        uint8_t *data,
                        size_t   len);
```

**Description:** Performs a standard I²C register write: writes the device address with WRITE bit, writes the register address, then writes `len` bytes from `data`. The entire transaction is mutex-protected.

**Parameters:**

| Parameter | Type | Description |
|---|---|---|
| `dev_addr` | `uint8_t` | 7-bit I²C device address |
| `reg` | `uint8_t` | Register address to write to |
| `data` | `uint8_t *` | Buffer containing bytes to write. May be NULL if `len` is 0 |
| `len` | `size_t` | Number of data bytes to write after the register address |

**Returns:**

| Value | Meaning |
|---|---|
| `I2C_OK` | Transaction completed, device ACKed all bytes |
| `I2C_ERR_BUS` | Transaction failed — NACK received or bus error |
| `I2C_ERR_TIMEOUT` | Mutex acquisition timed out |

**Caller responsibilities:**
- Must not be called from ISR context.
- A return of `I2C_OK` confirms the device ACKed the bytes on the wire. It does not confirm the device's internal register logic applied the write. Callers that require write confirmation must perform an explicit read-back (as `bma423_init()` does for every configuration register).
- The BMA423 datasheet requires a minimum 1000 µs inter-write delay between consecutive register writes. `i2c_write()` does not enforce this delay — the caller is responsible.

---

## 25.2 Sensor Driver — `bma423.h`

---

### `bma423_status_t` — Error Enum

```c
typedef enum {
    BMA423_OK = 0,
    BMA423_ERR_BUS,
    BMA423_ERR_CHIP_ID,
    BMA423_ERR_RESET,
    BMA423_ERR_CONFIG,
    BMA423_ERR_FATAL,
    BMA423_ERR_CMD,
    BMA423_ERR_NO_DATA,
    BMA423_ERR_INVALID_ARG
} bma423_status_t;
```

| Code | Meaning | Typical recovery |
|---|---|---|
| `BMA423_OK` | Success | None needed |
| `BMA423_ERR_BUS` | I²C transaction failed | Retry, then re-init |
| `BMA423_ERR_CHIP_ID` | Chip ID read returned unexpected value | Check wiring, check address |
| `BMA423_ERR_RESET` | Soft reset command failed | Retry init |
| `BMA423_ERR_CONFIG` | Register write verification failed | Check ERR_REG, retry init |
| `BMA423_ERR_FATAL` | ERR_REG fatal_err bit set | Full sensor reset required |
| `BMA423_ERR_CMD` | ERR_REG cmd_err bit set | Fix command sequence |
| `BMA423_ERR_NO_DATA` | Data ready bit not set at read time | Wait for next interrupt |
| `BMA423_ERR_INVALID_ARG` | NULL pointer passed to function | Fix caller |

---

### `bma423_init`

```c
bma423_status_t bma423_init(void);
```

**Description:** Performs the complete 10-step sensor initialization sequence:
1. Soft reset via CMD register (0x7E ← 0xB6)
2. 50 ms boot delay
3. Dummy read to clear reset state
4. Chip ID verification (0x00 → expect 0x13)
5. ERR_REG check (0x02 → expect 0x00)
6. ACC_CONF configuration (0x40 ← 0x26: 25 Hz, avg4, power mode)
7. ACC_RANGE configuration (0x41 ← 0x01: ±4g)
8. INT1_IO_CTRL configuration (0x53 ← 0x0B: output enable, push-pull, active high, edge)
9. INT_MAP configuration (0x58 ← 0x04: drdy → INT1)
10. PWR_CTRL accelerometer enable (0x7D ← 0x04)

Every configuration write (steps 6–10) is followed by a read-back verification.

**Parameters:** None.

**Returns:** `BMA423_OK` if all steps succeed. The specific error code from the first step that fails — allowing the caller to distinguish a wiring fault (`BMA423_ERR_CHIP_ID`) from a configuration fault (`BMA423_ERR_CONFIG`) from a hardware fault (`BMA423_ERR_FATAL`).

**Caller responsibilities:**
- `i2c_init()` must be called and must have returned `I2C_OK` before calling this function.
- Must be called from task context — not ISR context.
- Must be called before `bma423_isr_init()`.
- Safe to call again as a re-initialization — the soft reset at step 1 returns the sensor to a known state before reconfiguring.

**Side effects:** Sends a soft reset to the sensor, which clears all sensor registers to their reset defaults before reconfiguring them. Any prior sensor configuration is lost.

---

### `bma423_read_accel`

```c
bma423_status_t bma423_read_accel(int16_t *x,
                                   int16_t *y,
                                   int16_t *z);
```

**Description:** Reads the six acceleration data registers (0x12–0x17) in a single burst I²C transaction and reconstructs three signed 12-bit acceleration values with correct sign extension.

The raw register layout is:

```
0x12 — ACC_X_LSB: bits [7:4] = x[3:0],  bits [3:0] = reserved
0x13 — ACC_X_MSB: bits [7:0] = x[11:4]
0x14 — ACC_Y_LSB: bits [7:4] = y[3:0],  bits [3:0] = reserved
0x15 — ACC_Y_MSB: bits [7:0] = y[11:4]
0x16 — ACC_Z_LSB: bits [7:4] = z[3:0],  bits [3:0] = reserved
0x17 — ACC_Z_MSB: bits [7:0] = z[11:4]
```

Reconstruction per axis:

```c
uint16_t raw = ((uint16_t)msb << 4) | (lsb >> 4);
int16_t  val = ((int16_t)(raw << 4)) >> 4;  // sign extension
```

The left-shift moves bit 11 into bit 15, making it the sign bit of the `int16_t`. The arithmetic right-shift then propagates the sign into bits [15:12].

**Parameters:**

| Parameter | Type | Description |
|---|---|---|
| `x` | `int16_t *` | Caller-allocated. Receives signed 12-bit X-axis value |
| `y` | `int16_t *` | Caller-allocated. Receives signed 12-bit Y-axis value |
| `z` | `int16_t *` | Caller-allocated. Receives signed 12-bit Z-axis value |

**Returns:**

| Value | Meaning |
|---|---|
| `BMA423_OK` | All six bytes read, values reconstructed and written to x/y/z |
| `BMA423_ERR_BUS` | I²C burst read failed |
| `BMA423_ERR_INVALID_ARG` | Any of x, y, z is NULL |

**Caller responsibilities:**
- All three output pointers must be non-NULL.
- This function is intended to be called from `bma423_task()` after an ISR event — not from ISR context.
- The drdy status register is intentionally not checked inside this function. The ISR edge is the authoritative data-ready signal. Re-checking drdy inside this function introduces a race condition where the flag clears between the interrupt and the read, causing false `BMA423_ERR_NO_DATA` returns.
- Output values are in raw LSB units. To convert to mg: `mg = lsb * (4000 / 4096)` for ±4g range (~0.977 mg/LSB).

**Notes:** A 100 µs busy-wait is applied before the burst read as a conservative stabilization margin. See Section 17.4 for full discussion.

---

## 25.3 ISR Subsystem — `bma423_isr.h`

---

### `bma423_isr_init`

```c
bma423_status_t bma423_isr_init(void);
```

**Description:** Initializes the complete interrupt pipeline in safe dependency order:
1. Creates the FreeRTOS event queue (depth 5, `uint8_t` items)
2. Configures GPIO39 as input, no pull resistors, rising-edge triggered
3. Installs the ESP-IDF GPIO ISR service
4. Attaches `bma423_isr_handler` to GPIO39
5. Creates `bma423_task` (stack: 2048 words, priority: 5)

On any failure, all resources allocated up to that point are freed before returning. No partial initialization state is left behind.

**Parameters:** None.

**Returns:** `BMA423_OK` if all five steps succeed. `BMA423_ERR_CONFIG` if any step fails — the specific failing step is identifiable from the serial log output.

**Caller responsibilities:**
- `bma423_init()` must have returned `BMA423_OK` before calling this.
- `i2c_init()` must have completed successfully — the task created here will call `i2c_read()`.
- Must be called from task context. Must not be called from ISR context.
- Must not be called more than once without full teardown — calling twice will attempt to install the GPIO ISR service twice and create a second task against the same queue.

**Side effects:** Creates one FreeRTOS task and one FreeRTOS queue. Registers a GPIO ISR handler. After this function returns `BMA423_OK`, the ISR is live — the BMA423 may assert INT1 and trigger `bma423_isr_handler` at any time.

