# 20 — Power Considerations

## 20.1 Scope Reminder

As established in Section 1.3 and Section 3.3: **MCU/sensor low-power
mode transitions (ESP32 sleep, BMA423 suspend mode) are not implemented in
this phase.** This section documents what the current code *does* touch
regarding power — rail verification and stabilization timing — and is
explicit about what it does not do, rather than implying power management
exists where it doesn't.

## 20.2 What's Actually Implemented

| Capability | Implemented? | Where |
|---|---|---|
| PMIC rail state read/log (DC3, LDO2, LDO3) | Yes | `power_init()` in `power.c` |
| PMIC rail enable with write-verify (LDO2, LDO3) | Yes (helpers exist, not currently called from `power_init()`'s main path — see 20.3) | `axp202_enable_ldo2()`, `axp202_enable_ldo3()` |
| Protection against disabling the ESP32's own core rail (DC3) | Yes, by omission — no code path ever writes to clear the DC3 bit | `power_init()` explicitly never touches DC3 |
| BMA423 power-mode configuration (`PWR_CONF` advanced power save) | Present but disabled (`0x00`) | `bma423_init()`, Section 16.6 |
| BMA423 suspend/low-power mode transitions | **Not implemented** | — |
| ESP32 light sleep / deep sleep integration | **Not implemented** | — |
| Wake-from-sleep re-sync logic for the sensor | **Not implemented** | — |

## 20.3 Rail Verification, Not Rail Sequencing

It's worth being precise about what `power_init()` actually does: it
**reads and logs** the current AXP202 power-output register state, and
applies a stabilization delay — it does not itself *sequence* rail
enables in the sense of turning things on in a specific order at boot.
`axp202_enable_ldo2()`/`axp202_enable_ldo3()` exist as reusable,
write-verified primitives (Section 8.3), but are not called anywhere in
the current `power_init()` flow — they're forward-looking stubs for when
display (LDO2/backlight) or audio (LDO3) subsystems are actually
integrated, per the code comments. Calling this "power sequencing" would
overstate what's happening; "power state verification with rail-enable
primitives ready for future use" is the accurate description.

## 20.4 Why BMA423 Needs No Explicit Power Enable

The BMA423 sits on the AXP202's `DC3` rail — the same rail that powers the
ESP32 itself. This is a hardware fact this firmware relies on rather than
enforces: if `app_main()` is executing at all, `DC3` is necessarily
already active, and therefore the BMA423 already has power. This is why
`power_init()` contains a diagnostic read of `DC3`'s state but no write to
enable it — there's no code path in which the MCU is running but the
sensor isn't powered, given this board's rail assignment. This is stated
as a hardware-topology fact specific to this board, not a general property
of BMA423 designs — a different board could put the BMA423 on a switchable
rail, in which case this assumption would need to be revisited.

## 20.5 Stabilization Delays as the Only Power-Adjacent Timing Behavior

| Delay | Duration | Purpose |
|---|---|---|
| Post-power-init stabilization | 50 ms | Allow BMA423 internal NVM load to complete after power is confirmed present (`power_init()`) |
| Post-soft-reset settle | 50 ms | Allow sensor internal reboot to complete (`bma423_init()`) |

Both of these are conservative margins over datasheet minimums (per
Section 18.4) rather than power-saving measures — they exist to guarantee
correctness of subsequent register access, not to reduce energy
consumption. This distinction matters: this project has *reliability*
timing margins, not *power-optimized* timing — the two are sometimes
conflated in less careful documentation, and I want to keep them separate
here.

## 20.6 Power Draw: Not Measured

No current draw measurements (active mode, or otherwise) were taken for
this project — there is no bench measurement of mA consumed by the BMA423
at 25 Hz ODR with advanced power save disabled, nor a comparison against
what it would be with `PWR_CONF` power-save enabled. Any number I could
provide here would come from the Bosch datasheet's typical current
consumption tables, not from this project's own measurement — and since
I don't have datasheet figures confirmed with you, I'm not going to insert
a placeholder "typical current consumption: X µA" figure. If you have
datasheet current-consumption tables or bench measurements, I'll add real
numbers; otherwise this section states plainly that power draw was not
characterized in Phase 1.

## 20.7 Why Power Modes Were Deferred (Not Just "Not Done")

Restating and sharpening the reasoning from your original scoping notes,
since "we ran out of time" is a weaker statement than the actual reasoning
here: implementing BMA423 suspend-mode transitions and ESP32 sleep
integration correctly requires solving a re-synchronization problem — after
a suspend/resume or MCU sleep/wake cycle, the sensor's configuration state
and the driver's implicit state (Section 11) need to be reconciled, since
sleep transitions on many accelerometers reset volatile configuration
registers. Building this correctly would mean extending the state machine
in Section 11 with explicit `SLEEPING`/`WAKING`/`RESYNC` states and
defining what "recovery" means across a sleep boundary versus across a bus
fault — genuinely new design work, not an extension of the existing fault
ladder. Deferring it was a scope-control decision to keep Phase 1 focused
on bus/sensor fault tolerance as a single, demonstrable skill, rather than
attempting fault tolerance and power-state management simultaneously and
risking neither being solid.


