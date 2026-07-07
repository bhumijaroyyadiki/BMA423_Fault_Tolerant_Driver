# 27_Important_Algorithms.md

This section documents the non-trivial algorithmic decisions in the driver — pieces of logic where the correct approach is not obvious, where a naive implementation would produce wrong results, or where the implementation makes assumptions that need to be stated explicitly.

---

## 27.1 12-Bit Signed Value Reconstruction

**The problem:** The BMA423 stores each acceleration axis as a 12-bit two's complement signed integer split across two 8-bit registers. The raw register layout is:

```
LSB register (e.g. 0x12 for X):
  bits [7:4] → data bits [3:0]
  bits [3:0] → reserved (always 0)

MSB register (e.g. 0x13 for X):
  bits [7:0] → data bits [11:4]
```

**Step 1 — Reconstruct the raw 12-bit value:**

```c
uint16_t raw_x = ((uint16_t)buf[1] << 4) | (buf[0] >> 4);
```

`buf[1]` is the MSB register — its 8 bits become bits [11:4] of the result by shifting left 4. `buf[0]` is the LSB register — shifting right 4 discards the reserved lower nibble and moves bits [7:4] into positions [3:0]. The OR combines them into a 12-bit value sitting in the lower 12 bits of a `uint16_t`.

At this point `raw_x` is an unsigned 16-bit integer holding a 12-bit value. If the acceleration is positive, bits [15:12] are zero and the value is correct. If the acceleration is negative — bit 11 is set in two's complement — the value is wrong: it reads as a large positive number instead of a negative one.

**Step 2 — Sign extension:**

```c
*x = ((int16_t)(raw_x << 4)) >> 4;
```

Left-shifting by 4 moves bit 11 into bit 15 — the sign bit position of a 16-bit integer. Casting to `int16_t` at this point makes the compiler treat bit 15 as a sign bit. The arithmetic right-shift by 4 then propagates that sign bit back into bits [15:12], filling them with 1s if the value is negative and 0s if positive.

**Why this works for negative values — concrete example:**

```
Sensor output: -512 in 12-bit two's complement
Binary:        1000 0000 0000  (bit 11 set = negative)

After step 1:
raw_x = 0x0800  =  0000 1000 0000 0000

After (raw_x << 4):
       = 0x8000  =  1000 0000 0000 0000
Cast to int16_t: -32768

After >> 4 (arithmetic):
       = 0xF800  =  1111 1000 0000 0000
As int16_t: -2048

Wait — that's wrong. Let me recheck.
```

Correction — concrete example with actual sensor value:

```
Sensor output: -1 in 12-bit two's complement
12-bit binary: 1111 1111 1111

After step 1:
raw_x = 0x0FFF

After (raw_x << 4):
      = 0xFFF0
Cast to int16_t: -16

After >> 4 (arithmetic):
      = 0xFFFF
As int16_t: -1  ✓
```

**The C standard caveat:** Right-shifting a signed integer is implementation-defined in the C standard. Every embedded compiler targeting ARM Cortex (GCC, Clang, ARMCC) performs arithmetic right shift on signed integers — but this is not guaranteed by the standard. A fully portable alternative:

```c
// Portable sign extension without relying on arithmetic shift
int16_t val = (int16_t)(raw_x << 4);
val >>= 4;  // implementation-defined but universally arithmetic on all embedded targets
```

Or using a mask-based approach that is fully portable:

```c
int16_t val;
if (raw_x & 0x0800) {
    val = (int16_t)(raw_x | 0xF000);  // fill upper bits with 1s
} else {
    val = (int16_t)raw_x;
}
```

The shift-based approach was used in this driver — accepted as implementation-defined but universally correct on all ARM Cortex-M targets.

---

## 27.2 Register Value Construction

**The problem:** Multiple independent bit fields must be packed into a single 8-bit register write without any field contaminating its neighbors.

**The pattern used throughout `bma423_init()`:**

```c
uint8_t acc_conf =
    ((ACC_PERF_MODE_AVG & 0x01) << ACC_CONF_PERF_MODE_POS) |
    ((ACC_BWP_AVG_4     & 0x07) << ACC_CONF_BWP_POS)       |
    ((ACC_ODR_25HZ      & 0x0F) << ACC_CONF_ODR_POS);
```

**Why mask before shift, not after:**

The mask (`& 0x01`, `& 0x07`, `& 0x0F`) is applied to the raw field value before shifting it into position. This ensures that even if a constant is defined incorrectly — say `ACC_BWP_AVG_4` is accidentally defined as `0x12` instead of `0x02` — the mask truncates it to the correct field width before it is shifted, preventing it from bleeding into adjacent fields.

Masking after shift requires the mask to be the shifted version (`& 0x70` instead of `& 0x07`), which is harder to read and easier to get wrong when the field position changes.

**The field width masks:**

| Field | Width | Raw mask |
|---|---|---|
| `perf_mode` | 1 bit | `0x01` |
| `acc_bwp` | 3 bits | `0x07` |
| `acc_odr` | 4 bits | `0x0F` |

These match the datasheet field widths exactly. A field width of N bits has a raw mask of `(1 << N) - 1`.

---

## 27.3 Burst Read for Multi-Axis Data

**The problem:** X, Y, Z acceleration data occupies six consecutive registers (0x12–0x17). They could be read as six separate single-byte transactions or as one six-byte burst.

**The implementation:**

```c
i2c_status_t result = i2c_read(BMA423_ADDR,
                                BMA423_ACC_X_LSB_REG,
                                buf,
                                6);
```

One I²C transaction reads all six bytes. The BMA423 auto-increments its internal register pointer after each byte read within a single transaction.

**Why burst reading is correct here — not just efficient:**

Reading six registers in six separate transactions introduces a race condition. Between any two transactions, the BMA423 may have produced a new sample — meaning X could come from sample N and Y from sample N+1. The resulting XYZ triplet would be a mix of two different physical moments — a coherent sample set is not guaranteed.

A burst read within a single I²C transaction reads all six bytes atomically from the sensor's perspective. The BMA423 latches the sample set when the first register is addressed — subsequent bytes in the same transaction come from the same latched sample.

**The assumption this relies on:** The six data registers are at consecutive addresses (0x12, 0x13, 0x14, 0x15, 0x16, 0x17) and the BMA423 auto-increments correctly across them within a single transaction. Both were verified against the datasheet register map.

---

## 27.4 Graduated Recovery — The Retry Ladder

**The problem:** `i2c_master_cmd_begin` returns `ESP_FAIL` periodically. The system must recover without crashing, without disrupting other devices on the shared I²C bus, and without permanently blocking if the sensor becomes unresponsive.

**The algorithm:**

```
On BMA423_ERR_BUS from bma423_read_accel():

Level 1: for i in [0, MAX_RETRY_COUNT):
             wait RETRY_DELAY_MS
             retry bma423_read_accel()
             if success: break and continue normally

         if still failing after MAX_RETRY_COUNT:
             escalate to Level 2

Level 2: for i in [0, MAX_REINIT_COUNT):
             wait REINIT_DELAY_MS
             call bma423_init()
             if init fails: log and try again
             if init succeeds: verify with bma423_read_accel()
             if verification succeeds: break and continue normally

         if still failing after MAX_REINIT_COUNT:
             escalate to Level 3

Level 3: log CRITICAL
         vTaskSuspend(NULL)
```

**Parameter values and rationale:**

| Parameter | Value | Rationale |
|---|---|---|
| `MAX_RETRY_COUNT` | 3 | Sufficient to span the observed ~20–30 ms ESP-IDF failure window at 10 ms intervals |
| `RETRY_DELAY_MS` | 10 | Empirically determined minimum delay for spontaneous recovery from the 0x107 failure |
| `MAX_REINIT_COUNT` | 3 | Matches retry count for symmetry; three full re-inits failing indicates a hardware problem |
| `REINIT_DELAY_MS` | 50 | Longer than retry delay — re-init is expensive (soft reset, 50 ms boot wait, 10 register operations); allowing more recovery time before each attempt |

**Why break on first success:**

The retry loop breaks immediately on the first successful read — it does not always run all iterations. This was an explicit design decision: there is no value in performing a successful retry and then performing two more. The loop runs the minimum number of iterations needed.

**Why recovery lives in `bma423_task()` not `bma423_read_accel()`:**

`bma423_read_accel()` is a sensor layer function. Its contract is to attempt a read and report the result. Recovery — which involves re-initializing a sensor on a shared bus, coordinating with FreeRTOS task state, and making decisions about system degradation — is a system-level concern. Putting recovery inside `bma423_read_accel()` would violate the layer boundary: a sensor function would be making decisions about system behavior. `bma423_task()` is the correct layer because it owns the event loop and can safely coordinate all resources involved in recovery.

---

## 27.5 Mutex Acquire / Release — Guaranteed Symmetry

**The problem:** `i2c_read()` and `i2c_write()` must guarantee `xSemaphoreGive()` is called on every code path that called `xSemaphoreTake()` — including error paths and early returns.

**The pattern:**

```c
i2c_status_t i2c_read(...) {
    // Argument validation BEFORE mutex — no release needed on this path
    if (len == 0 || data == NULL) {
        return I2C_ERR_INVALID_ARG;
    }

    // Acquire mutex
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return I2C_ERR_TIMEOUT;  // never acquired — no release needed
    }

    // ... transaction ...

    // Single exit point after mutex acquisition
    i2c_status_t result = (err != ESP_OK) ? I2C_ERR_BUS : I2C_OK;
    xSemaphoreGive(i2c_mutex);  // always reached
    return result;
}
```

**The critical structural rule:** Argument validation happens before mutex acquisition. This means the early-return path for invalid arguments never holds the mutex and therefore needs no release. After the mutex is acquired, there is exactly one exit point — the `xSemaphoreGive` / `return result` pair at the end. There are no early returns after acquisition.

This structure was arrived at after a bug where the mutex was released only on the success path — an early return on transaction failure skipped the release, permanently deadlocking all subsequent I²C calls after a single failed transaction.

