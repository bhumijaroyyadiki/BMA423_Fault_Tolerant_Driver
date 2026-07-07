# 13 — Initialization Sequence

Section 10.1 showed *which functions call which* during init. This section
focuses on **timing budgets and exact failure-exit points** — the sequence
diagram view, with what happens at each possible abort point.

## 13.1 Sequence Diagram: Full Init Path

```
main.c          i2c.c           power.c         bma423.c        bma423_isr.c
  │                │                │                │                │
  │─i2c_init()────▶│                │                │                │
  │                │ create mutex   │                │                │
  │                │ i2c_param_config│               │                │
  │                │ i2c_driver_install│              │                │
  │◀──I2C_OK───────│                │                │                │
  │  (else: return, abort — no further init attempted)                │
  │                │                │                │                │
  │─power_init()────────────────────▶│                │                │
  │                │◀─read PWR_OUT──│                │                │
  │                │──byte──────────▶│               │                │
  │                │                │ log DC3/LDO2/LDO3 state          │
  │                │                │ vTaskDelay(50ms)                 │
  │◀──ESP_OK─────────────────────────│                │                │
  │  (else: return, abort — cannot read AXP202 register)               │
  │                │                │                │                │
  │─i2c_scan()──────▶│              │                │                │
  │                │ probe 0x08–0x77│                │                │
  │◀──(diagnostic only, no failure path)──────────────│                │
  │                │                │                │                │
  │─bma423_init()───────────────────────────────────▶│                │
  │                │◀─soft reset (0xB6→CMD_REG)───────│                │
  │                │                │  vTaskDelay(50ms)                │
  │                │◀─dummy CHIP_ID read──────────────│                │
  │                │◀─CHIP_ID read (evaluated)─────────│                │
  │                │                │  compare to 0x13                 │
  │                │                │  (else: return BMA423_ERR_CHIP_ID)│
  │                │◀─ERR_REG read──────────────────────│              │
  │                │                │  check fatal/cmd/config bits      │
  │                │                │  (else: return FATAL/CMD/CONFIG)  │
  │                │◀─ACC_CONF write+verify─────────────│              │
  │                │                │  DELAY_US(1000)                   │
  │                │                │  (verify fail → ERR_CONFIG)        │
  │                │◀─ACC_RANGE write+verify────────────│              │
  │                │                │  DELAY_US(1000)                   │
  │                │◀─INT1_IO_CTRL write+verify─────────│              │
  │                │◀─INT_MAP write+verify──────────────│              │
  │                │◀─PWR_CONF write (no verify)─────────│             │
  │                │                │  DELAY_US(1000)                   │
  │                │◀─PWR_CTRL write+verify──────────────│             │
  │                │                │  DELAY_US(1000)                   │
  │◀──BMA423_OK─────────────────────────────────────────│              │
  │  (else: return, abort — bma423_isr_init() never called)            │
  │                │                │                │                │
  │─bma423_isr_init()──────────────────────────────────────────────▶│
  │                │                │                │  xQueueCreate  │
  │                │                │                │  gpio_config   │
  │                │                │                │  install_isr   │
  │                │                │                │  handler_add   │
  │                │                │                │  xTaskCreate   │
  │◀──BMA423_OK────────────────────────────────────────────────────│
  │  (else: return — system enters init-failed state, no acquisition) │
  │                │                │                │                │
  │  "[MAIN] Driver pipeline fully operational!"                       │
```

## 13.2 Timing Budget

| Step | Fixed delay | Cumulative (worst case, all steps succeed first try) |
|---|---|---|
| I2C init | none | ~0 ms |
| Power init read + log | none | ~0 ms |
| Power init stabilization | 50 ms | ~50 ms |
| BMA423 soft reset | none (write only) | ~50 ms |
| Post-reset settle | 50 ms | ~100 ms |
| ACC_CONF config + delay | 1000 µs | ~101 ms |
| ACC_RANGE config + delay | 1000 µs | ~102 ms |
| INT1_IO_CTRL config | none listed | ~102 ms |
| INT_MAP config | none listed | ~102 ms |
| PWR_CONF + delay | 1000 µs | ~103 ms |
| PWR_CTRL config + delay | 1000 µs | ~104 ms |
| ISR/task setup | none | ~104 ms |

**Total time from `app_main()` entry to "pipeline fully operational" is
approximately 104 ms** under the no-fault path, dominated almost entirely
by the two fixed 50 ms delays (power stabilization, post-reset settle) —
the microsecond-scale inter-write delays are negligible by comparison.
This number is derived directly from the delays present in the code, not
measured on a scope; actual wall-clock time will also include I2C
transaction time (bounded at ≤10 ms per transaction per NFR-1.3) and mutex
wait time, both of which are normally sub-millisecond on an idle bus.

## 13.3 Every Failure Exit Point Is a Dead Stop, Not a Retry

Worth stating explicitly: **none of the init-time failure paths in
`bma423_init()` retry internally.** A failed chip-ID check, a failed
config-write verification, or a failed `ERR_REG` check all return
immediately with a specific error code, and `main.c` aborts on any
non-`BMA423_OK` return (`return;` with no further attempt). This is a
deliberate asymmetry with the *runtime* recovery ladder in
`bma423_isr.c` (Section 21), which does retry — the distinction being
that init-time failures typically indicate a wiring, power-sequencing, or
hardware-identity problem that a fixed retry count is unlikely to resolve,
whereas runtime failures during steady-state acquisition are far more
likely to be transient bus noise. This trade-off is expanded on in Section
23 (Design Decisions).

