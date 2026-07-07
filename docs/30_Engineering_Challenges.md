# 30_Engineering_Challenges.md

This section documents the genuinely difficult problems encountered during this project — not bugs with obvious fixes, but challenges that required reasoning about hardware behavior, system architecture, and trade-offs before a solution could be designed. Each entry below represents a problem where the correct answer was not immediately obvious and where an incorrect answer would have produced a system that appeared to work but failed in subtle or dangerous ways.

---

## 30.1 Designing for a Bus You Don't Fully Control

**The challenge:** The I²C bus is shared between four devices — BMA423 (0x19), AXP202 PMIC (0x35), PCF8563 RTC (0x51), and FT6236 touch controller (0x38). The BMA423 driver owns only one of these. Any recovery action that affects the bus affects all four.

This constraint shaped every recovery decision in the driver. The natural first instinct when an I²C transaction fails is to reset the bus — delete the driver, reinstall it, start fresh. On a single-device bus this is safe. On a shared bus it is potentially catastrophic: the AXP202 manages power rails including the one supplying the ESP32 itself. An I²C driver teardown mid-operation could corrupt the AXP202's internal state, potentially turning off a power rail and killing the system.

**How it was resolved:** The recovery ladder was designed around the principle of minimum necessary intervention. Level 1 retries the read — no bus interaction beyond the retry itself. Level 2 reinitializes the BMA423 sensor only — `bma423_init()` sends a soft reset to one device and reconfigures its registers. The I²C driver itself is never touched during recovery. Full bus reset is documented as a theoretical Level 4 — not implemented, reserved only for a scenario where the bus is physically stuck (SDA held low by a device), which was never observed.

**What this taught:** Recovery architecture in a shared-resource system cannot be designed in isolation. Every recovery action must be analyzed for its effect on every other consumer of the shared resource — not just the one being recovered.

---

## 30.2 The Missed Interrupt Problem — Edge vs Level Triggering

**The challenge:** Edge-triggered interrupts fire once per transition. If the CPU misses the rising edge — because it is in a higher-priority context, in a critical section, or executing from flash during a cache miss — the interrupt is lost permanently. The sensor has new data. The INT1 pin went high. The ESP32 never saw it. No ISR ran. No queue event was deposited. The task never wakes.

This is not a theoretical concern — it is a real failure mode that had to be consciously accepted or mitigated.

**The alternative considered:** Level-triggered interrupts fire continuously as long as the pin is asserted. A missed edge is not possible — the interrupt keeps firing until the ISR runs and clears the source. This eliminates missed interrupts entirely.

**Why level triggering was rejected:** Level-triggered interrupts require the ISR to clear the interrupt source before returning — otherwise the ISR fires again immediately in an infinite loop. Clearing the BMA423 data-ready interrupt source means reading the data registers inside the ISR. Reading six bytes over I²C at 400 kHz takes approximately 150–200 µs. This violates the ISR minimalism requirement — and more critically, `i2c_read()` acquires a FreeRTOS mutex, which is illegal from ISR context and will corrupt the scheduler.

**Resolution:** Edge triggering was accepted with a conscious risk trade-off. At 25 Hz, the probability of missing an edge due to scheduler preemption is extremely low — the ESP32 at 160 MHz has no operation that blocks interrupts for 40 ms (the inter-sample period). The retry ladder in `bma423_task()` handles the occasional missed sample gracefully — a missed interrupt means the queue receives no event for that sample, the task simply waits for the next one, and operation continues normally. No data corruption occurs — one sample is skipped.

**What this taught:** Hardware interrupt configuration decisions have cascading implications for ISR design, which have cascading implications for driver architecture. The decision to use edge triggering was not made in isolation — it was made after reasoning through the full chain of consequences, including the mutex constraint that made level triggering genuinely unimplementable without a redesign.

---

## 30.3 The Race Condition Between Interrupt and Data Register

**The challenge:** The data-ready interrupt fires when a new sample is available. But "available" means the sensor's internal ADC has completed and written the result to its internal registers. The question is: is the data stable in the registers at the exact moment INT1 goes high, or does INT1 go high slightly before the register write completes?

If INT1 asserts before the registers are stable, a read immediately after the interrupt could return partially written data — a mix of the new sample and the previous one, with no indication anything is wrong.

**How this was investigated:** The BMA423 datasheet describes the data-ready interrupt as asserting "when a new data set is ready" — implying data is available at interrupt time. However no explicit timing parameter specifying the relationship between interrupt assertion and register stability was found.

**Resolution:** A 100 µs busy-wait was added before the burst read as a conservative stabilization margin. This was treated as an empirical safeguard rather than a datasheet-specified requirement. In all testing, correct data was returned consistently. The delay costs 2.5 ms per second of CPU busy-wait at 25 Hz — a known and accepted cost documented in Section 17.4.

**The honest limitation:** Without oscilloscope measurement of the INT1 assertion timing relative to register write completion, the 100 µs delay cannot be precisely justified. It can only be characterized as "sufficient in all observed cases." A production driver for a safety-critical application would require scope measurement and a datasheet-backed timing specification before shipping.

**What this taught:** Absence of a timing specification in a datasheet is not confirmation that timing does not matter. It is a gap in documentation that requires either empirical characterization or conservative design. Assuming best-case behavior in the absence of specification is a common source of intermittent embedded bugs.

---

## 30.4 Diagnosing a Periodic Failure With No Obvious Cause

**The challenge:** A failure appearing every ~1.5 seconds with no code change between occurrences, no pattern in the data preceding it, and no obvious relationship to anything in the application code. The failure recovered spontaneously — which made it simultaneously less urgent and harder to diagnose. A failure that recovers by itself provides no crash dump, no stuck state to inspect, and no obvious point of failure to examine.

**Why this was genuinely difficult:** Every standard debugging instinct points toward something the code did — a wrong value, a missing check, a race condition triggered by a specific sequence of events. A periodic failure driven by framework-internal timing is invisible to application-level inspection. The code is correct. The sequence of calls is correct. The failure happens anyway, determined entirely by an internal timer in a component the application does not directly control.

**The diagnostic approach that worked:**

First — characterize the failure precisely before attempting any fix. Exact error code (`0x107`), exact frequency (~1.5 seconds), exact recovery behavior (10 ms delay sufficient). This characterization took priority over fixing.

Second — design experiments that distinguish between candidate causes. The ODR experiment (changing from 25 Hz to 50 Hz and observing whether failure count doubled) was the key step. If the failure were transaction-count-based, doubling the ODR would double the failure rate. If it were time-based, doubling the ODR would double the number of transactions affected per failure event while keeping the failure interval constant. The result — two consecutive failures at 50 Hz, same ~1.5 second interval — confirmed time-based causation definitively.

Third — match the characterization against known framework behavior. The boot-time warning about the legacy I²C driver, combined with the time-based failure pattern, pointed to an internal driver timing issue without requiring access to the ESP-IDF source code.

**What this taught:** Characterization is more valuable than immediate fixing. A fix applied before the failure is characterized may eliminate the symptom without understanding the cause — leaving an unknown fragility in the system. The time invested in the ODR experiment and error code logging paid off in a confident root cause assessment rather than a lucky guess.

---

## 30.5 Keeping Recovery Architecture at the Right Layer

**The challenge:** When `bma423_read_accel()` returns `BMA423_ERR_BUS`, the natural impulse is to add recovery logic inside that function — retry there, re-init there, handle everything where the failure is detected. This is the path of least resistance: the failure and the fix are in the same function, the code is self-contained, and the caller receives either data or an error with no additional responsibility.

The problem with this approach is not that it fails to work — it does work, in isolation. The problem is what it does to the architecture.

`bma423_read_accel()` is a sensor layer function. Its contract is narrow: attempt to read six registers and reconstruct the acceleration values. If recovery logic is added inside it, the function now also owns: deciding how many times to retry, deciding when to re-initialize the sensor, deciding whether to involve the I²C driver, and deciding what "unrecoverable" means at the system level. These are system-level decisions — they require knowledge of the shared bus, the FreeRTOS task state, and the overall system degradation policy. None of that knowledge belongs in a function whose stated purpose is reading six registers.

More concretely: `bma423_init()` — which would be called from `bma423_read_accel()` during recovery — sends a soft reset to the sensor and reconfigures it. That soft reset affects the shared I²C bus. A sensor layer function that sends a soft reset to a device and reconfigures it is no longer just reading data — it is making system-level decisions about bus management.

**Resolution:** Recovery lives in `bma423_task()`. The sensor layer reports failure and exits. The task layer decides what to do about it. This maintains the contract of each layer: `bma423_read_accel()` reads data, `bma423_task()` manages the operational state of the sensor subsystem.

**The validation:** During fault injection testing, `bma423_init()` was called from `bma423_task()` while the fault flag was active. The re-init succeeded — the sensor was correctly reconfigured. The subsequent verification read failed — the fault was still active. The task correctly escalated to Level 3 and suspended. At no point did `bma423_read_accel()` need to know anything about recovery — it simply returned `BMA423_ERR_BUS` and let the task handle it.

**What this taught:** The question "where should this code live?" is not answered by "wherever the failure is detected." It is answered by "which layer has the knowledge and authority to make this decision?" Recovery requires system-level knowledge. System-level knowledge belongs in the system-level layer.

---

## 30.6 Testing Recovery Paths That Do Not Trigger Naturally

**The challenge:** Level 1 recovery triggers naturally every ~1.5 seconds due to the ESP-IDF driver behavior. Level 2 and Level 3 require Level 1 to fail completely — which never happens in normal operation because the 10 ms retry delay is always sufficient for recovery. A recovery path that is never triggered is a recovery path that may not work.

The naive approach — "it's coded correctly, it should work" — is not acceptable for safety-relevant code. Recovery logic that has never executed is recovery logic that has never been tested.

**The challenge within the challenge:** Physically disconnecting the sensor to force a real hardware failure was possible but impractical as a repeatable test. Physical disconnection is not a controlled failure — it can trigger multiple simultaneous failure modes (bus contention, pull-up issues, undefined states on shared lines) that obscure which part of the recovery is being tested.

**Resolution:** Fault injection via a software flag. A `g_inject_fault` boolean in `bma423_read_accel()` forced unconditional `BMA423_ERR_BUS` returns when set. This produced a controlled, repeatable, level-selectable failure mode:

- With `g_inject_fault = true`: all reads fail → Level 1 exhausted → Level 2 triggers → re-init succeeds but verification fails → Level 3 triggers → task suspends
- With `g_inject_fault = false`: normal operation resumes

The flag is compile-time controlled and removed from production builds.

**What the test confirmed:**
- Level 1 exhausted correctly after 3 consecutive failures
- Level 2 attempted re-init 3 times, each time succeeding at the sensor level but failing the verification read
- Level 3 logged CRITICAL and suspended the task cleanly
- The system continued running after task suspension — no crash, no hang, no watchdog reset

**What this taught:** Untested recovery code is not recovery code — it is recovery-shaped code. Testing recovery paths requires deliberate fault injection because real failures are either too infrequent, too uncontrolled, or too destructive to use as primary test vectors. The fault injection mechanism should be designed as carefully as the recovery mechanism itself.

