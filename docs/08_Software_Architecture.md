# 08 — Software Architecture

## 8.1 Architectural Style

The software follows a **layered driver architecture** with a strict
dependency direction: each layer depends only on the layer directly below
it, never sideways, never upward. This is the standard pattern for embedded
peripheral drivers precisely because it's what makes Section 6.5's
portability claim (NFR-6.1/6.2) possible — swap the transport layer, and the
protocol layer above it doesn't need to change.

## 8.2 Layer Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                      APPLICATION LAYER                        │
│                        (main.c)                                │
│   Orchestrates init order. Owns no hardware knowledge.         │
└───────────────────────────┬────────────────────────────────────┘
                             │ calls
┌───────────────────────────▼────────────────────────────────────┐
│                  SUBSYSTEM CONCURRENCY LAYER                    │
│                    (bma423_isr.c)                                │
│   ISR, event queue, acquisition task, fault recovery ladder.    │
│   Depends on: bma423.c (protocol layer), FreeRTOS, ESP-IDF GPIO │
└───────────────────────────┬────────────────────────────────────┘
                             │ calls
┌───────────────────────────▼────────────────────────────────────┐
│                    SENSOR PROTOCOL LAYER                        │
│                       (bma423.c)                                 │
│   Register-level BMA423 config/read logic. No task/ISR/GPIO     │
│   awareness — pure register sequencing against the datasheet.   │
│   Depends on: i2c.c (transport), bma423_regs.h, bma423_platform.h│
└───────────────────────────┬────────────────────────────────────┘
                             │ calls
┌───────────────────────────▼────────────────────────────────────┐
│                     TRANSPORT LAYER                              │
│                        (i2c.c)                                   │
│   Mutex-protected I2C transactions, timeouts.                   │
│   Depends on: ESP-IDF driver/i2c.h                                │
└───────────────────────────┬────────────────────────────────────┘
                             │ calls
┌───────────────────────────▼────────────────────────────────────┐
│                    PLATFORM / VENDOR LAYER                       │
│              (ESP-IDF: driver/i2c.h, driver/gpio.h,               │
│               freertos/*, esp_rom_sys.h)                          │
└──────────────────────────────────────────────────────────────────┘

        ┌────────────────────────────────────────────┐
        │              PEER SUBSYSTEM                  │
        │                (power.c)                      │
        │   AXP202 rail control. Same layer position    │
        │   as bma423.c — both sit directly on i2c.c,    │
        │   neither depends on the other.                │
        └────────────────────────────────────────────┘
```

## 8.3 Why This Layering, Specifically

- **`bma423.c` has zero FreeRTOS/GPIO/task awareness.** It only calls
  `i2c_read`, `i2c_write`, and `BMA423_DELAY_US`. This is deliberate: the
  protocol layer describes *what* the BMA423 needs, not *how* the system
  schedules work around it. It could be called from a bare superloop with
  no RTOS at all, with zero changes.
- **`bma423_isr.c` is where scheduling policy lives.** ISR-to-task handoff,
  retry counts, recovery tiers — all concurrency/timing *policy* decisions
  are isolated in this one file, not scattered across the protocol layer.
- **`power.c` and `bma423.c` are structural peers, not a hierarchy.** Both
  are independent consumers of the transport layer. Neither driver has
  visibility into the other — the AXP202 driver doesn't know the BMA423
  exists, and vice versa. Any interaction between them is purely at the bus
  arbitration level (the shared mutex in `i2c.c`), never at the API level.
  This matters because it means power-rail logic can be modified or removed
  without touching a single line of the accelerometer driver.
- **`bma423_regs.h` carries no logic.** It's pure data — register addresses,
  bit positions, masks. This keeps the datasheet-derived constants in one
  auditable place, separate from the sequencing logic that uses them
  (Section 24 covers this file-by-file).

## 8.4 What This Layering Does *Not* Achieve (Honest Limitation)

The transport layer (`i2c.c`) is still ESP-IDF-coupled at the call level
(`i2c_cmd_link_create`, `i2c_master_start`, etc.), and `bma423_isr.c` calls
ESP-IDF GPIO/FreeRTOS APIs directly rather than through an abstracted HAL.
So while the *sensor protocol* layer is genuinely platform-agnostic, the
*transport* and *concurrency* layers are not — porting to another MCU means
rewriting `i2c.c` and the ISR/task setup in `bma423_isr.c`, not just
swapping a single HAL header. This is stated plainly rather than
oversold, and is revisited in Section 34 (Limitations).

