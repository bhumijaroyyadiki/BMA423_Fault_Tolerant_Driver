# 06 — System Architecture

## 6.1 Overview

The system architecture has two layers worth separating cleanly: the
**physical/electrical topology** (what's on the I2C bus, what powers what) and
the **runtime concurrency topology** (ISR, queue, task, mutex). Conflating
these two is a common source of confusion in embedded documentation, so they
are shown as separate diagrams here.

## 6.2 Physical / Bus Topology

```
                              ┌─────────────────────────┐
                              │         ESP32            │
                              │   (FreeRTOS, dual-core)  │
                              │                          │
                              │  GPIO21 ─── SDA          │
                              │  GPIO22 ─── SCL          │
                              │  GPIO39 ─── INT1 (input) │
                              └──────┬────────┬──────────┘
                                     │        │
                         I2C BUS    │        │  GPIO interrupt
                    (400 kHz, SDA/SCL, shared, pulled up)
                                     │
        ┌─────────────┬─────────────┼─────────────┬─────────────┐
        │              │            │             │              
   ┌────▼────┐   ┌─────▼─────┐ ┌────▼─────┐ ┌─────▼─────┐        
   │ AXP202  │   │  BMA423   │ │ PCF8563  │ │  FT6236   │        
   │  PMIC   │   │  Accel.   │ │   RTC    │ │  Touch    │        
   │ 0x35    │   │  0x19     │ │  0x51    │ │  0x38     │        
   │         │   │           │ │          │ │           │        
   │ Powers  │   │ INT1 ─────┼─┘ (bus     │ │ (bus      │        
   │ DC3=ESP │   │ (GPIO39)  │  peer,     │ │ peer,     │        
   │ (never  │   │           │  not       │ │ not       │        
   │ disable)│   │           │  driven    │ │ driven    │        
   └─────────┘   └───────────┘  by this   │ │ by this   │        
                                 project)  │ │ project)  │        
                                 └─────────┘ └───────────┘        
```

**Why this matters architecturally:** the BMA423 shares a physical bus with a
PMIC that is actively switching power rails during operation (backlight,
audio). This is precisely the kind of shared-bus contention that motivates
the mutex in `i2c.c` (NFR-3.1) — a rail-enable write to the AXP202 and an
accelerometer read must not interleave at the transaction level.

## 6.3 Runtime / Concurrency Topology

```
   Hardware                ISR Context              Task Context
  ┌─────────┐        ┌──────────────────┐      ┌────────────────────┐
  │ BMA423  │ INT1   │ bma423_isr_      │ post │  accel_event_queue │
  │ INT1 pin│───────▶│ handler()        │─────▶│  (depth = 10,      │
  │ (edge)  │        │                  │      │   uint8_t payload) │
  └─────────┘        │ - xQueueSendFrom │      └──────────┬──────────┘
                      │   ISR()          │                 │
                      │ - portYIELD_FROM │                 │ xQueueReceive
                      │   _ISR() if      │                 │ (blocking)
                      │   needed         │                 ▼
                      │                  │      ┌────────────────────┐
                      │ NO I2C calls     │      │   bma423_task()    │
                      │ NO blocking      │      │                    │
                      └──────────────────┘      │  1. settling delay │
                                                 │     (100us)        │
                                                 │  2. read accel     │
                                                 │     via i2c_read   │
                                                 │  3. on failure:    │
                                                 │     retry ladder   │
                                                 │     (Section 21)   │
                                                 └──────────┬──────────┘
                                                            │
                                                            ▼
                                                  ┌────────────────────┐
                                                  │  i2c_read/write()  │
                                                  │  (mutex-protected, │
                                                  │   shared by BMA423 │
                                                  │   + AXP202 paths)  │
                                                  └────────────────────┘
```

**Design principle behind this split:** the ISR does the absolute minimum
required to hand off control — signal only, no data, no bus access. This is a
deliberate choice, not a default. See Section 23 (Design Decisions) for the
alternatives considered (e.g. reading data directly in the ISR) and why they
were rejected.

## 6.4 Initialization-Time vs Runtime Architecture

The system has two distinct architectural phases that must not be confused:

- **Init phase** (`app_main` → `i2c_init` → `power_init` → `bma423_init` →
  `bma423_isr_init`): strictly sequential, blocking, each step gated on the
  previous step's success. No interrupts are live during this phase until
  `bma423_isr_init()` runs *last*.
- **Runtime phase**: fully event-driven. No sequential control flow — the
  system is idle until an interrupt occurs, at which point the
  ISR → queue → task chain executes.

This separation is why `bma423_isr_init()` is deliberately the *last* call in
`app_main()` — enabling interrupts before the sensor is verified and
configured would allow the ISR to fire against an unconfigured or
partially-configured device (FR-6.3).

## 6.5 Layer Responsibility Summary

| Layer | File(s) | Responsibility | Knows about hardware specifics of |
|---|---|---|---|
| Application entry | `main.c` | Orchestrates init order, aborts on first failure | Nothing — only calls into subsystem init functions |
| Power | `power.c` | AXP202 rail state, stabilization timing | AXP202 register map only |
| Transport | `i2c.c` | Bus transactions, mutex, timeouts | ESP32 I2C peripheral (ESP-IDF) |
| Sensor protocol | `bma423.c` | Register-level BMA423 configuration and read | BMA423 register map only |
| Sensor concurrency | `bma423_isr.c` | ISR, queue, task, recovery ladder | ESP32 GPIO/FreeRTOS APIs directly (see NFR-6.2 caveat) |
| Platform timing | `bma423_platform.h` | Microsecond delay abstraction | `esp_rom_delay_us` (ESP-IDF) |
| Register map | `bma423_regs.h` | Named constants for addresses/bits/masks | Nothing — pure data, no logic |

