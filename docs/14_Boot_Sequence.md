# 14 — Boot Sequence

## 14.1 Scope of This Section

Section 13 documented the timing and failure-exit behavior of driver
initialization *inside* `app_main()`. This section zooms out one level
further — the complete system startup path from power application to
normal event-driven operation — and focuses on **why the sequence is
ordered the way it is**, not on register-level detail (already covered in
Section 13 and Section 16).

## 14.2 Full Boot Path

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Power-on                                                   │
│    AXP202 begins supplying DC3 (ESP32 core rail)               │
└───────────────────────────┬───────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ 2. ESP32 Boot ROM                                              │
│    Fixed, immutable first-stage boot code (masked into silicon)│
│    Loads second-stage bootloader from flash                    │
└───────────────────────────┬───────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ 3. Second-stage bootloader (ESP-IDF)                            │
│    Reads partition table, locates and validates app image       │
│    Loads app image into RAM/flash-mapped execution                │
└───────────────────────────┬───────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ 4. ESP-IDF runtime init                                          │
│    C runtime setup, heap init, FreeRTOS scheduler starts           │
└───────────────────────────┬───────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ 5. app_main() entry  ← this project's firmware begins here       │
└───────────────────────────┬───────────────────────────────────┘
                             ▼
                  (see Section 14.3 below)
```

Steps 1–4 are standard ESP-IDF/ESP32 boot behavior and are not
project-specific — this firmware does not customize the bootloader,
partition table, or ESP-IDF runtime init. They are included here only for
completeness, since "boot sequence" as a documentation section is
incomplete if it starts at `app_main()` and pretends nothing came before
it.

## 14.3 Firmware Startup Sequence (This Project's Responsibility)

```
app_main() entry
   │
   ▼
Stage 1 — I2C bus bring-up
   (i2c_init: mutex + peripheral config)
   │
   ▼
Stage 2 — Power subsystem verification
   (power_init: read AXP202 state, confirm DC3 active, stabilization delay)
   │
   ▼
Stage 3 — Bus topology verification
   (i2c_scan: diagnostic confirmation of expected devices present)
   │
   ▼
Stage 4 — Sensor hardware configuration
   (bma423_init: reset, chip-ID verify, ACC_CONF/RANGE/INT config,
    each write verified before proceeding)
   │
   ▼
Stage 5 — Interrupt pipeline activation
   (bma423_isr_init: queue, GPIO, ISR, task — installed last)
   │
   ▼
Stage 6 — Normal operation
   (system idle, event-driven; task blocked on queue until next interrupt)
```

## 14.4 Why This Order Is Enforced, Stage by Stage

This is the part that actually matters for this section — the ordering is
not incidental, it's a dependency chain where each stage is a
precondition for the next:

- **I2C before anything else (Stage 1):** every subsequent stage —
  power verification, sensor config, even the bus scan — depends on a
  working transport layer. There is no meaningful way to check power
  state or configure the sensor without a functioning I2C peripheral and
  mutex already in place.

- **Power verification before sensor configuration (Stage 2):** the BMA423
  sits on the always-on rail tied to the ESP32's own supply, so if the
  code has reached this point at all, the sensor is already powered.
  What Stage 2 actually protects against is proceeding on the *assumption*
  that the AXP202 is in a known state — reading and logging `PWR_OUT_CTRL`
  before touching the sensor means any unexpected rail state is visible in
  the boot log before it can manifest as a confusing sensor-init failure
  three stages later.

- **Bus scan before sensor init, not after (Stage 3):** this is a
  deliberate debugging-first ordering. If the BMA423 or another expected
  device doesn't show up in the scan, that's diagnosed as a wiring/power
  problem immediately, rather than surfacing later as an ambiguous
  `BMA423_ERR_BUS` from `bma423_init()` that could just as easily be a
  transient bus fault. The scan buys an earlier, clearer failure signal.

- **Sensor must be fully configured and verified before interrupts are
  enabled (Stage 4 before Stage 5) — this is the single most important
  ordering constraint in the whole boot sequence.** Installing the GPIO
  ISR before the sensor's data-ready interrupt is actually configured (or
  before the sensor is even out of reset) would allow a spurious or
  premature edge on GPIO39 to enqueue an event and wake `bma423_task`
  against a device that isn't in a known state — the recovery ladder in
  `bma423_isr.c` assumes the device *was* working and has since faulted;
  it has no logic for "device was never valid to begin with." Enforcing
  Stage 4 fully complete before Stage 5 begins eliminates this race by
  construction rather than by detecting and handling it after the fact.

- **Each stage validates the previous stage's success before proceeding
  (not just before starting):** `main.c` checks the return status of every
  stage and aborts immediately on failure — there is no "proceed anyway
  and hope" path anywhere in the boot sequence. This is what makes the
  system's bring-up deterministic: a given hardware/firmware combination
  either reaches Stage 6 in a fully known-good state, or it stops at a
  specific, logged stage and goes no further. There is no partially-
  initialized state that Stage 6 (normal operation) can be entered from.

## 14.5 What "Deterministic Bring-Up" Means Here, Concretely

Two guarantees fall out of this design:

1. If the system reaches `"[MAIN] Driver pipeline fully operational!"`,
   every prior stage is known to have succeeded — there is no code path
   that reaches this log line with, for example, an unverified
   configuration register or an uninstalled ISR.
2. If the system does *not* reach that line, the last successful log
   message identifies exactly which stage failed, without needing a
   debugger attached — this was a deliberate observability choice, not
   an incidental side effect of adding print statements (see Section
   29, Debugging Journey, for how this logging was actually used to
   isolate real faults during development).
