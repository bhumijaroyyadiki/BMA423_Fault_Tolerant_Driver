# 16 — Register Configuration

This section documents every register this project actually writes or
reads, the exact value chosen, and why — sourced directly from
`bma423_regs.h` and the write sequence in `bma423_init()`. Bit-field detail
for each register is expanded further in Section 28 (Register-Level
Explanation); this section is about *configuration choices*, that section
is about *register semantics*.

## 16.1 Registers Touched by This Driver

| Register | Address | Access | Used for |
|---|---|---|---|
| `CHIP_ID` | `0x00` | Read | Device identity verification |
| `ERR_REG` | `0x02` | Read | Post-reset error state check |
| `STATUS` | `0x03` | (defined, not read in current flow) | — |
| `ACC_X_LSB..ACC_Z_MSB` | `0x12`–`0x17` | Read (burst) | Acceleration data |
| `ACC_CONF` | `0x40` | Write + verify | ODR, bandwidth, performance mode |
| `ACC_RANGE` | `0x41` | Write + verify | Full-scale range |
| `INT1_IO_CTRL` | `0x53` | Write + verify | INT1 pin electrical behavior |
| `INT_MAP` (a.k.a. `INT_MAP_DATA`) | `0x58` | Write + verify | Data-ready → INT1 routing |
| `PWR_CONF` | `0x7C` | Write | Advanced power save disable |
| `PWR_CTRL` | `0x7D` | Write + verify | Accelerometer enable |
| `CMD` | `0x7E` | Write | Soft reset trigger |

Note: `BMA423_DATA_X_LSB_REG` (`0x0A`) is defined in `bma423_regs.h` but is
**not** the register actually used for data reads — the driver reads from
`BMA423_ACC_X_LSB_REG` (`0x12`) instead. This is a leftover/unused
definition in the header, flagged here rather than silently treated as
active, and worth cleaning up (noted in Section 34, Limitations).

## 16.2 ACC_CONF (0x40) — Chosen Configuration

```c
uint8_t acc_conf =
    ((ACC_PERF_MODE_AVG & 0x01) << ACC_CONF_PERF_MODE_POS) |
    ((ACC_BWP_AVG_4     & 0x07) << ACC_CONF_BWP_POS)       |
    ((ACC_ODR_25HZ      & 0x0F) << ACC_CONF_ODR_POS);
```

| Field | Bits | Value chosen | Meaning | Why |
|---|---|---|---|---|
| `perf_mode` | 7 | `0x00` (avg/power mode) | Averaging power mode rather than continuous high-performance mode | Lower power draw appropriate for a wearable; full performance mode is unnecessary for step-counting-class motion sensing |
| `acc_bwp` | 6:4 | `0x02` (avg4) | 4-sample averaging bandwidth parameter | Balances noise reduction against latency — matches the `perf_mode=avg` selection above, since `bwp` semantics are defined relative to `perf_mode` |
| `acc_odr` | 3:0 | `0x06` (25 Hz) | Output data rate | 25 Hz is sufficient for step-counting/activity detection; the header also defines `ACC_ODR_50HZ` as an available-but-unused alternative |

**Note:** `ACC_CONF_PERF_MODE_POS` is defined as `7` in `bma423_regs.h`,
but the Bosch BMA423 datasheet defines `perf_mode` at **bit 7** and
`bwp` at **bits 6:4** — this matches. I want to flag one thing I can't
verify without the datasheet PDF in front of me: whether `ACC_BWP_AVG_4 =
0x02` and `ACC_ODR_25HZ = 0x06` are the exact encodings Bosch defines for
those values, or approximate/assumed. **Can you confirm you cross-checked
these specific encodings against the datasheet register table during Week
0, or should I mark these two constants as "value chosen empirically /
not independently re-verified against datasheet encoding table" in the
doc?** I'd rather ask than assert datasheet accuracy I can't check myself.

## 16.3 ACC_RANGE (0x41) — Chosen Configuration

| Value | Setting | Why |
|---|---|---|
| `ACC_RANGE_4G` (`0x01`) | ±4g full scale | Adequate headroom for wrist-worn motion/step detection without wasting dynamic range on ±16g impacts this use case won't see; ±2g was rejected as potentially clipping on sharper wrist motion, ±8g/±16g rejected as unnecessarily coarse resolution per LSB for this application |

## 16.4 INT1_IO_CTRL (0x53) — Chosen Configuration

```c
uint8_t int1_io_ctrl =
    (INT1_INPUT_DIS   << INT1_IO_CTRL_INPUT_EN_POS)  |
    (INT1_OUTPUT_EN   << INT1_IO_CTRL_OUTPUT_EN_POS) |
    (INT1_PUSH_PULL   << INT1_IO_CTRL_OD_POS)        |
    (INT1_ACTIVE_HIGH << INT1_IO_CTRL_LVL_POS)       |
    (INT1_EDGE_TR     << INT1_IO_CTRL_EDGE_CTRL_POS);
```

| Field | Value chosen | Why |
|---|---|---|
| `input_en` | Disabled | INT1 pin is used purely as an output from the sensor in this design — no external signal is ever driven into it |
| `output_en` | Enabled | Required for the sensor to actually drive the interrupt line |
| `od` (open-drain vs push-pull) | Push-pull | No external pull resistor needed; matched to a dedicated, non-shared GPIO39 input on the ESP32 — open-drain would only be necessary if INT1 were wire-OR'd with other interrupt sources, which it is not here |
| `lvl` (active level) | Active-high | Matched directly to `GPIO_INTR_POSEDGE` configured on the ESP32 side (Section 7.4) — this pairing is deliberate and is the single most failure-prone mismatch point in interrupt-driven sensor designs if not kept consistent |
| `edge_ctrl` | Edge-triggered | Matches the ESP32's edge-triggered GPIO interrupt config; a level-triggered sensor config paired with an edge-triggered MCU config would risk missed re-triggers if the MCU doesn't clear/re-arm fast enough |

## 16.5 INT_MAP (0x58) — Chosen Configuration

| Value | Meaning | Why |
|---|---|---|
| `INT_MAP_DRDY_INT1` bit set | Data-ready interrupt routed to INT1 pin | INT1 is the only interrupt line wired to the ESP32 (GPIO39) in this design; INT2 is unused and unwired |

## 16.6 PWR_CONF (0x7C)

| Value written | Meaning | Why |
|---|---|---|
| `0x00` | Advanced power save disabled | Advanced power save mode introduces additional wake-up latency after each access; disabled here to keep interrupt-to-data latency predictable during Phase 1 development. Revisiting this is explicitly deferred to the power-mode work described in Section 35 (Future Improvements) — this is not a final power-optimized setting, it's the setting chosen to keep this phase's fault-handling behavior easy to reason about |

## 16.7 PWR_CTRL (0x7D)

| Value written | Meaning | Why |
|---|---|---|
| `PWR_CTRL_ACC_EN` at bit 2 | Accelerometer enabled | Reset default has the accelerometer **disabled** — this write is not optional boilerplate, it's the step that actually turns on data acquisition. Its verification (read-back check) exists specifically because a silently-failed enable would produce a driver that initializes "successfully" but never produces valid data — a failure mode that would be confusing to debug without the read-back check catching it at init time instead |

## 16.8 CMD (0x7E) — Soft Reset

| Value written | Meaning | Why |
|---|---|---|
| `BMA423_SOFT_RESET_CMD` (`0xB6`) | Triggers full soft reset | Datasheet-defined magic command value; ensures the sensor starts configuration from a known power-on-equivalent state regardless of what state it was left in by a previous boot cycle or a Tier-2 recovery re-init |

