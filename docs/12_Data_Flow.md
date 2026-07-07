# 12 — Data Flow

Section 10 showed *control* flow (which function calls which). This section
tracks the *data* itself — where raw bytes originate, how they're
transformed, and where they end up.

## 12.1 Acceleration Data Flow (Steady State)

```
┌─────────────────┐
│  BMA423 internal │
│  ADC registers   │
│  ACC_X_LSB..MSB  │
│  ACC_Z_LSB..MSB  │
│  (6 bytes,       │
│   0x12–0x17)     │
└────────┬─────────┘
         │ I2C burst read (6 bytes, 1 transaction)
         ▼
┌─────────────────────────────┐
│ uint8_t buf[6]               │  <- i2c_read(BMA423_ADDR,
│ (stack, bma423_read_accel)   │     ACC_X_LSB_REG, buf, 6)
└────────┬─────────────────────┘
         │
         │  raw_x = (buf[1] << 4) | (buf[0] >> 4)   [12-bit unsigned]
         │  raw_y = (buf[3] << 4) | (buf[2] >> 4)
         │  raw_z = (buf[5] << 4) | (buf[4] >> 4)
         ▼
┌─────────────────────────────┐
│ uint16_t raw_x/raw_y/raw_z    │
│ (12 significant bits,         │
│  right-justified)             │
└────────┬─────────────────────┘
         │
         │  *x = ((int16_t)(raw_x << 4)) >> 4   [sign-extend 12→16 bit]
         │  *y = ((int16_t)(raw_y << 4)) >> 4
         │  *z = ((int16_t)(raw_z << 4)) >> 4
         ▼
┌─────────────────────────────┐
│ int16_t x, y, z               │  <- caller-owned (bma423_task's
│ (signed, caller's stack)      │     local x/y/z variables)
└────────┬─────────────────────┘
         │
         ▼
┌─────────────────────────────┐
│ printf("X=%d Y=%d Z=%d\n")    │  <- terminal/serial output only.
│ Serial/UART console            │     No further consumer exists yet
└─────────────────────────────┘     (no step-counting/UI integration
                                      shown in shared code)
```

**Why the bit manipulation matters enough to trace explicitly:** the
BMA423 packs each axis as a 12-bit signed value across two 8-bit registers
(4 bits of the LSB register are unused/zero-padded). Byte-concatenating
LSB and MSB naively (`(msb << 8) | lsb`) would put the 12 significant bits
in the wrong bit positions and would not sign-extend correctly for negative
acceleration values. The `<<4` then `>>4` pair is doing two jobs in
sequence: first realigning the 12 meaningful bits to bit positions 0–11 by
shifting out the 4 don't-care LSB bits, then sign-extending from bit 11
using an arithmetic right shift — which only works correctly because `x`,
`y`, `z` are declared `int16_t` (signed), so `>>` is guaranteed arithmetic,
not logical.

## 12.2 Configuration Data Flow (Init Time)

```
┌──────────────────┐        ┌──────────────────┐
│ Compile-time      │        │ bma423_regs.h     │
│ constants          │───────▶│ named bitfield     │
│ (this project's     │        │ constants          │
│  chosen settings:  │        │ (register map)     │
│  ODR=25Hz, ±4g,    │        └────────┬───────────┘
│  perf_mode=avg)     │                 │
└──────────────────┘                 │ bitwise OR into
                                       │ single register byte
                                       ▼
                              ┌──────────────────┐
                              │ uint8_t acc_conf/  │
                              │ acc_range/         │
                              │ int1_io_ctrl/      │
                              │ int_map (local)     │
                              └────────┬───────────┘
                                       │ i2c_write()
                                       ▼
                              ┌──────────────────┐
                              │ BMA423 register    │
                              │ (hardware)          │
                              └────────┬───────────┘
                                       │ i2c_read() [readback]
                                       ▼
                              ┌──────────────────┐
                              │ uint8_t *_readback │
                              │ (local)             │
                              └────────┬───────────┘
                                       │ == comparison
                                       ▼
                              ┌──────────────────┐
                              │ bma423_status_t     │
                              │ (BMA423_OK or        │
                              │  BMA423_ERR_CONFIG)  │
                              └──────────────────┘
```

This is the same pattern repeated four times in `bma423_init()`
(`ACC_CONF`, `ACC_RANGE`, `INT1_IO_CTRL`, `INT_MAP`) — data flows out to
hardware and back before the function trusts that the configuration took
effect. No configuration value is treated as "written" until it's been
observed coming back from the device.

## 12.3 Error/Status Data Flow

```
┌──────────────────┐
│ ESP-IDF esp_err_t  │  (I2C driver's own error type)
└────────┬───────────┘
         │ translated in i2c.c:
         │   result = (err != ESP_OK) ? I2C_ERR_BUS : I2C_OK
         ▼
┌──────────────────┐
│ i2c_status_t        │  (transport layer's error type)
└────────┬───────────┘
         │ translated in bma423.c:
         │   if (i2c_read(...) != I2C_OK) return BMA423_ERR_BUS;
         ▼
┌──────────────────┐
│ bma423_status_t     │  (protocol layer's error type)
└────────┬───────────┘
         │ consumed in bma423_isr.c:
         │   if (status != BMA423_OK) { retry ladder }
         ▼
┌──────────────────┐
│ printf() logging +  │
│ recovery-tier        │
│ control decision     │
└──────────────────┘
```

Each layer boundary is also an **error-type translation boundary** — this
is deliberate and matches the layer diagram in Section 8. A caller in
`bma423_isr.c` never sees an `esp_err_t` or an `i2c_status_t` directly; it
only ever branches on `bma423_status_t`. This is what makes it possible to
change `i2c.c`'s underlying implementation (e.g. porting to STM32 HAL)
without touching a single `if` statement in `bma423_isr.c`.

