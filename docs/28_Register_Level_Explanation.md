# 28_Register_Level_Explanation.md

This section explains every register used in this driver at the bit level — not just what value was written, but what each bit controls, why that specific value was chosen, and what would happen if it were configured differently. This is the register map as understood by the engineer who implemented the driver, not copied from the datasheet.

---

## 28.1 CHIP_ID — 0x00

```
Bits [7:0] → chip_id
Reset value: 0x13
Access: Read-only
```

This register has one purpose: identity verification. The BMA423 always returns `0x13` from this address regardless of power mode or configuration state. It is the first register read after reset — before any configuration — because it is the earliest possible check that communication is established with the correct device at the correct address.

What a wrong value here actually means:

| Value read | Most likely cause |
|---|---|
| `0x00` | Device not responding, SDA held low, wrong I²C address |
| `0xFF` | Bus floating, no pull-up resistors, device absent |
| Any other value | Wrong device at this address, or bus fault corrupting data |

In this driver, any value other than `0x13` returns `BMA423_ERR_CHIP_ID` immediately and halts initialization. There is no recovery from a wrong chip ID — it indicates a hardware or wiring problem that no amount of software can fix.

---

## 28.2 ERR_REG — 0x02

```
Bit 7   → aux_err       Auxiliary sensor error
Bit 6   → fifo_err      FIFO overflow or internal FIFO error
Bits[4:2] → error_code  3-bit field: internal error classification
Bit 1   → cmd_err       Invalid command received
Bit 0   → fatal_err     Unrecoverable internal error
Reset value: 0x00
Access: Read-only
```

This register is read immediately after chip ID verification — before any configuration writes — to confirm the sensor came out of reset cleanly. A non-zero `ERR_REG` before any configuration has been written indicates a hardware problem with the sensor itself, not a software error.

**Why the three error types require different responses:**

`fatal_err` (bit 0): The sensor's internal state machine has entered an unrecoverable condition. No register write will fix this. The only recovery is a full soft reset via the CMD register. In this driver this returns `BMA423_ERR_FATAL`.

`cmd_err` (bit 1): The sensor received a command it rejected — wrong value, wrong timing, illegal operation sequence. This is a software error — the driver sent something wrong. The correct response is to fix the command, not reset the sensor. Returns `BMA423_ERR_CMD`.

`error_code` (bits 4:2): A 3-bit field encoding internal error classifications. Value `0x01` means `acc_err` — accelerometer configuration error. This is a mid-level error: the sensor is alive but the accelerometer subsystem rejected its configuration. Re-sending the correct configuration is the appropriate response. Returns `BMA423_ERR_CONFIG`.

The bit-level check in code:

```c
if (err_reg & ERR_REG_FATAL_ERR_MASK)  return BMA423_ERR_FATAL;
if (err_reg & ERR_REG_CMD_ERR_MASK)    return BMA423_ERR_CMD;
if (err_reg & ERR_REG_ERROR_CODE_MASK) return BMA423_ERR_CONFIG;
```

Order matters: `fatal_err` is checked first because it supersedes all other errors — a sensor with `fatal_err` set cannot be configured regardless of what `cmd_err` says.

---

## 28.3 STATUS — 0x03

```
Bit 7 → drdy_acc    Accelerometer data ready
Bit 5 → drdy_aux    Auxiliary sensor data ready
Bit 4 → cmd_rdy     Command decoder ready
Bit 2 → aux_man_op  Manual auxiliary operation ongoing
Reset value: 0x10  (cmd_rdy = 1 at power-on)
Access: Read-only
```

The reset value `0x10` is significant: bit 4 (`cmd_rdy`) is set immediately at power-on. This means the sensor is ready to accept commands without an explicit wait after reset — confirmed by reading the datasheet timing section. The `cmd_rdy` bit does not need to be polled.

`drdy_acc` (bit 7) is set when a new accelerometer sample has been written to the data registers and is ready to be read. It clears automatically when the data registers are read. In this driver's architecture, the drdy bit is intentionally not checked inside `bma423_read_accel()` — the INT1 interrupt edge is the authoritative data-ready signal. Checking drdy after the interrupt arrives introduces a race condition: the bit may have already cleared by the time the task context reads it. See Section 17.4 for the full reasoning.

---

## 28.4 ACC_CONF — 0x40

```
Bit 7     → acc_perf_mode   0 = power/averaging mode, 1 = performance mode
Bits[6:4] → acc_bwp         Bandwidth / averaging parameter
Bits[3:0] → acc_odr         Output data rate
Reset value: 0xA8
Access: Read/Write
```

**Value written: `0x26`**

Decoded:

```
Bit 7   = 0  → acc_perf_mode = power mode (averaging)
Bits[6:4] = 010 = 0x02 → acc_bwp = norm_avg4 (average 4 samples)
Bits[3:0] = 0110 = 0x06 → acc_odr = 25 Hz
```

**acc_perf_mode = 0 (power mode):**

In performance mode the sensor uses an OSR (oversampling ratio) filter. In power mode it uses a simple averaging filter. Power mode was chosen because the application — step counting — involves slow, predictable human motion. The averaging filter suppresses high-frequency vibration noise (road surface, fabric movement) that would otherwise register as false motion events. The response latency introduced by averaging is irrelevant at 1–2 Hz human step frequency.

**acc_bwp = 0x02 (norm_avg4):**

In power mode, `acc_bwp` selects the number of samples averaged per output. `0x02` selects 4-sample averaging. The sensor internally samples at 4× the ODR (100 Hz at 25 Hz output) and averages groups of 4 into each output sample. This provides approximately 6 dB of noise reduction compared to no averaging.

**acc_odr = 0x06 (25 Hz):**

Human walking produces approximately 1–2 steps per second. By Nyquist, a minimum of 4 Hz sampling is required to detect this signal. 25 Hz was chosen to provide 12.5× the Nyquist minimum — sufficient to capture the waveform shape of a step (heel-strike peak, push-off peak) without aliasing. ODRs above 50 Hz provide no additional information for step detection and increase power consumption proportionally.

**The reset value `0xA8` is not a valid operating configuration.** Decoded: `acc_perf_mode = 1` (performance mode), `acc_bwp = 0x0A` — which is not a defined value in the performance mode BWP table. This register must always be explicitly written before use.

---

## 28.5 ACC_RANGE — 0x41

```
Bits[1:0] → acc_range   Measurement range
Reset value: 0x01
Access: Read/Write
```

| Value | Range | Resolution (12-bit) |
|---|---|---|
| 0x00 | ±2g | ~0.98 mg/LSB |
| 0x01 | ±4g | ~1.95 mg/LSB |
| 0x02 | ±8g | ~3.91 mg/LSB |
| 0x03 | ±16g | ~7.81 mg/LSB |

**Value written: `0x01` (±4g)**

±2g was rejected: normal walking generates ~1.5g peak; running and sharp wrist movements can exceed 2g, causing the sensor to clip at its maximum value and return `±2048` regardless of actual acceleration. Clipped data corrupts step detection.

±8g was rejected: provides 2× worse resolution than ±4g with no benefit for the expected acceleration range of human motion.

±4g covers walking (~1.5g), running (~3g), and sharp wrist movements without clipping, at approximately 1.95 mg/LSB — sufficient resolution for step detection algorithms.

The reset default is already `0x01` — this register was written explicitly anyway. Relying on reset defaults is a hidden dependency that fails silently on warm reboot, partial reset, or register corruption.

---

## 28.6 INT1_IO_CTRL — 0x53

```
Bit 4 → input_en    Enable INT1 as input to sensor (for external trigger)
Bit 3 → output_en   Enable INT1 as output from sensor
Bit 2 → od          0 = push-pull, 1 = open-drain
Bit 1 → lvl         0 = active low, 1 = active high
Bit 0 → edge_ctrl   Input edge control (irrelevant when used as output)
Reset value: 0x00
Access: Read/Write
```

**Value written: `0x0B`**

Decoded:

```
Bit 4 = 0 → input_en  = disabled  (INT1 used as output, not input)
Bit 3 = 1 → output_en = enabled
Bit 2 = 0 → od        = push-pull
Bit 1 = 1 → lvl       = active high
Bit 0 = 1 → edge_ctrl = edge (irrelevant for output — set to match ESP32 config)
```

**output_en = 1:** INT1 is used as an output from the sensor to the ESP32. If output_en is not set, the INT1 pin remains in a high-impedance state regardless of interrupt events — the ISR never fires.

**od = 0 (push-pull):** Push-pull actively drives both logic states. The sensor drives the line to VDDIO (3.3V) on interrupt and to GND otherwise. No external resistor is required. Open-drain was rejected because GPIO39 on the ESP32 has no internal pull-up capability — open-drain without a pull-up leaves the line floating in the idle state, causing spurious interrupts from noise.

**lvl = 1 (active high):** The line is driven high on interrupt assertion and held low otherwise. This aligns with `GPIO_INTR_POSEDGE` on the ESP32 side — the interrupt fires on the low-to-high transition. Active low was considered but rejected: with push-pull drive, active high produces cleaner transitions and is the natural polarity for a rising-edge trigger.

---

## 28.7 INT_MAP — 0x58

```
Bit 6 → drdy → INT2    Route data-ready interrupt to INT2 pin
Bit 2 → drdy → INT1    Route data-ready interrupt to INT1 pin
(other bits route other interrupt sources)
Reset value: 0x00
Access: Read/Write
```

**Value written: `0x04`**

Decoded: bit 2 set → data-ready interrupt routed to INT1.

This register is a routing matrix — it connects internal interrupt events to physical output pins. Setting a bit here does not enable the interrupt source; it only determines which pin carries the signal if the source fires. The interrupt source itself is enabled by the sensor's internal configuration (ODR configuration and accelerometer enable implicitly enable the data-ready event).

INT1 was chosen over INT2 because GPIO39 on the TTGO T-Watch 2020 V3 is the pin physically connected to the BMA423 INT1 pad on the PCB. This is a board-level constraint, not a software preference.

---

## 28.8 PWR_CONF — 0x7C

```
Bit 0 → adv_power_save   0 = disabled, 1 = advanced power save mode
Reset value: 0x03
Access: Read/Write
```

**Value written: `0x00`**

Advanced power save mode reduces power consumption by duty-cycling the sensor's internal logic between samples. However, it introduces constraints on register access timing and requires additional wake-up sequences before configuration changes. For this driver — which prioritizes correctness and predictable behavior over power optimization — advanced power save was disabled before enabling the accelerometer. This ensures the accelerometer is fully active and registers are immediately accessible without wake-up delays.

---

## 28.9 PWR_CTRL — 0x7D

```
Bit 2 → acc_en    0 = accelerometer disabled, 1 = accelerometer enabled
Bit 0 → aux_en    0 = auxiliary sensor disabled, 1 = enabled
Reset value: 0x00
Access: Read/Write
```

**Value written: `0x04`**

The accelerometer is disabled by default after every reset. This is the single most consequential register in the initialization sequence — without writing `acc_en = 1`, the sensor initializes correctly, all configuration registers verify correctly, interrupts fire correctly, but all six data registers return zero. There is no error indication from the sensor — it simply returns zeros for a disabled accelerometer.

This was discovered empirically during development. It is now the final step of `bma423_init()`, after all configuration is verified, because enabling the accelerometer before its configuration is set could result in the first samples being captured at the wrong ODR or range.

---

## 28.10 CMD — 0x7E

```
Bits[7:0] → command value
Access: Write-only
```

| Command | Value | Effect |
|---|---|---|
| Soft reset | 0xB6 | Returns all registers to reset defaults |

This is the only register in the driver that is write-only — reading it returns undefined data. The soft reset command is the first operation in `bma423_init()`. It guarantees the sensor starts from a known state regardless of what happened before — prior boot, watchdog reset, partial initialization, or register corruption.

After writing `0xB6`, the sensor requires a minimum of 1 ms before it is ready to accept new commands (datasheet power-up time specification). This driver waits 50 ms — a conservative margin that also covers the sensor's internal NVM load cycle. A dummy read of CHIP_ID is performed after the wait to clear any residual state from the reset, before the real chip ID verification read.

