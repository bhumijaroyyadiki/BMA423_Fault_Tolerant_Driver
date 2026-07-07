# 15 — Communication Protocol

## 15.1 Protocol Layer

The physical/electrical protocol is I2C (Inter-Integrated Circuit),
operating in **Fast Mode (400 kHz)**, master mode, 7-bit addressing. This
project implements two transaction patterns on top of raw I2C: a
**write transaction** and a **combined write-then-read transaction**
(register read via repeated start) — both defined in `i2c.c` and used
identically by every device on the bus (BMA423, AXP202).

## 15.2 Write Transaction

```c
i2c_master_start(cmd);
i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
i2c_master_write_byte(cmd, reg, true);
i2c_master_write(cmd, data, len, true);
i2c_master_stop(cmd);
```

**Protocol diagram:**

```
Master:  START │ ADDR+W │      │ REG  │      │ DATA[0..len-1] │      │ STOP
         ──────┼────────┼──────┼──────┼──────┼────────────────┼──────┼──────
Slave:         │        │ ACK  │      │ ACK  │                │ ACK  │
                (7-bit addr,    (register       (data bytes,
                 write bit=0)    address)         one ACK per byte)
```

Every byte written by the master (address, register, each data byte) is
expected to be ACKed by the slave (`true` in each `i2c_master_write_byte`/
`i2c_master_write` call requests ACK checking). If any byte is NACKed, the
ESP-IDF driver surfaces this as a non-`ESP_OK` return from
`i2c_master_cmd_begin()`, which `i2c_write()` translates to
`I2C_ERR_BUS`.

## 15.3 Read Transaction (Register Read via Repeated Start)

```c
i2c_master_start(cmd);
i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
i2c_master_write_byte(cmd, reg, true);
i2c_master_start(cmd);                    // repeated start — no STOP first
i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);   // if len > 1
i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
i2c_master_stop(cmd);
```

**Protocol diagram:**

```
Master: START│ADDR+W │    │ REG  │    │RESTART│ADDR+R │    │ ...DATA...       │ STOP
        ─────┼───────┼────┼──────┼────┼───────┼───────┼────┼──────────────────┼─────
Slave:       │       │ACK │      │ACK │       │       │ACK │ [byte1]..[byteN] │
                                                              M-ACK  ... M-NACK
```

- The **repeated start** (no STOP between the write phase and the read
  phase) is deliberate and required — it atomically sets the register
  pointer and then reads from it without releasing the bus in between,
  preventing another master (there isn't one here, but this is also
  correct practice generally) or an interleaved transaction from changing
  the register pointer between the two phases.
- For a multi-byte read, the master ACKs every byte except the **last**,
  which is NACKed (`I2C_MASTER_NACK`) — this is the standard I2C mechanism
  by which the master signals the slave "stop sending, this is the last
  byte I want," per the I2C specification. The code's conditional
  (`if (len > 1) i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);`
  followed by a single `i2c_master_read_byte(..., I2C_MASTER_NACK)` for
  the final byte) implements this correctly for both single-byte and
  multi-byte reads.

## 15.4 Application-Level Framing (BMA423 Register Access Convention)

Above the raw I2C transaction, the BMA423 (like most register-mapped I2C
peripherals) uses a simple convention: **first byte after the address is
always the register pointer**, and subsequent bytes are either the value(s)
to write there, or (on a read) the value(s) read starting from there and
auto-incrementing. This is why a single `i2c_read(BMA423_ADDR,
ACC_X_LSB_REG, buf, 6)` call correctly retrieves all six axis bytes
(`ACC_X_LSB` through `ACC_Z_MSB`) in one transaction — the BMA423 internally
auto-increments its register pointer after each byte read, so the 6-byte
burst read in Section 12.1 relies on this device-side behavior, not
anything the master explicitly requests per-byte.

## 15.5 Addressing

| Device | 7-bit address | Address as written (8-bit, shifted) |
|---|---|---|
| BMA423 | `0x19` | `0x32` (write) / `0x33` (read) |
| AXP202 | `0x35` | `0x6A` (write) / `0x6B` (read) |

The code operates on 7-bit addresses throughout (`BMA423_ADDR`,
`AXP202_ADDR`) and performs the `<<1 | R/W-bit` shift internally in
`i2c_write`/`i2c_read` — callers never construct the 8-bit address form
directly. This is the correct convention and avoids the common bug of a
caller double-shifting or hardcoding the 8-bit form somewhere inconsistent
with the driver.

## 15.6 Bus Arbitration Across Devices (Protocol-Level Interaction)

Because the mutex in `i2c.c` wraps the *entire* transaction (start through
stop) rather than individual bytes, no transaction from one device (e.g. an
AXP202 rail-enable write) can interleave with a BMA423 transaction at the
bit or byte level — each logical transaction diagrammed above in 15.2/15.3
executes atomically with respect to the other device's driver. This is the
protocol-level guarantee that Section 6.2's shared-bus topology depends on.
