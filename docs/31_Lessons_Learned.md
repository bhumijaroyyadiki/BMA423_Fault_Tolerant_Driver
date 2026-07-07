# 31_Lessons_Learned.md

This section distills what this project taught — not about the BMA423 specifically, but about embedded systems engineering in general. Each lesson below was learned by making a mistake, hitting an unexpected failure, or reasoning through a problem that turned out to be more complex than it first appeared. None of these are generic advice — each one is traceable to a specific moment in this project.

---

## 31.1 An ACK Is Not a Confirmation

The I²C protocol's ACK mechanism confirms that a device received a byte on the wire. It does not confirm that the device's internal logic acted on it. A register write that is ACKed may not have taken effect — the device could be in a reset state, an unexpected internal mode, or experiencing a signal integrity issue that corrupts data without producing a NACK.

This was not theoretical. Every configuration register write in `bma423_init()` is followed by a read-back verification because of this distinction. The verification catches the gap between "device received the byte" and "device applied the byte." An ACK alone is not sufficient evidence that configuration succeeded.

In practice, read-back verification adds one I²C transaction per configuration write — a negligible cost at initialization time. The alternative — trusting ACKs — leaves a class of configuration failure completely undetectable.

---

## 31.2 Default Register Values Are a Hidden Dependency

The BMA423 `ACC_RANGE` register defaults to `0x01` (±4g) after reset — the exact value needed for this driver. The natural conclusion is that this register does not need to be written. This conclusion is wrong.

Relying on reset defaults creates an implicit assumption: that the sensor has been freshly reset and no prior code has modified that register. This assumption fails silently on warm reboot, partial reset, register corruption from electrical noise, or future code changes that modify the initialization order. There is no error indication when the assumption fails — the register simply holds whatever value it held before.

Explicit configuration is self-documenting and resilient to all of these scenarios. The cost is one I²C write per register at initialization. The benefit is a driver that behaves identically regardless of prior system state.

---

## 31.3 The Accelerometer Is Off Until You Turn It On

The most consequential omission during development was not writing to `PWR_CTRL`. The sensor initialized without error. All registers verified correctly. Interrupts fired at the correct rate. Data reads returned `BMA423_OK`. And every axis read zero.

There was no error indication because from the sensor's perspective nothing was wrong — it was operating correctly with its accelerometer disabled, as configured by its reset default. The driver had no way to know the accelerometer was off except by reading the output data and noticing it was uniformly zero.

The lesson is not "always check PWR_CTRL." The lesson is that initialization is not complete when configuration registers are set — it is complete when the subsystem being configured is actually enabled and producing expected output. A working initialization sequence must be verified end-to-end, not just step-by-step.

---

## 31.4 Characterize Before Fixing

When the periodic `0x107` failure appeared, the first instinct was to fix it. Add a retry. Increase the timeout. Reset the bus. Any of these would have made the symptom disappear — but none of them would have identified the cause.

The decision to characterize first — log the raw error code, measure the failure interval, run the ODR experiment — took longer than applying any of the immediate fixes. But it produced a confident root cause assessment: internal ESP-IDF legacy driver timing behavior, not a hardware fault, not application code, not a race condition.

That assessment changed the fix. A hardware fault requires bus reset. A race condition requires synchronization. An internal driver timing issue requires a short retry delay — nothing more. The ODR experiment was the key step: if failure rate doubled when ODR doubled, the cause was transaction-count-based. It did not — confirming the cause was time-based.

Applying a fix before characterization risks fixing the wrong problem. A bus reset would have "worked" — the retry would succeed after the reset — but it would have introduced unnecessary disruption to the AXP202 and PCF8563 on the shared bus, and left the actual cause unidentified.

---

## 31.5 ISR Constraints Are Not Suggestions

The rule "do not acquire a mutex inside an ISR" is documented in the FreeRTOS API reference. It is easy to read, understand, and then forget when staring at a failing system and looking for the quickest path to getting data out of the sensor.

The temptation during development was real: reading the six acceleration registers inside the ISR would have been simpler. One function, one place, data available immediately. The constraint that made this impossible — `i2c_read()` acquires a mutex, mutex acquisition inside an ISR corrupts the FreeRTOS scheduler — is not a stylistic preference. Violating it produces undefined behavior that manifests as random crashes, scheduler corruption, or priority inversion — bugs that are extremely difficult to diagnose because they do not correlate with the line of code that caused them.

The ISR minimalism rule — ISR sets flag or sends queue event, task does everything else — exists because it is the only way to guarantee that ISR execution does not interfere with the rest of the system. Every line added to an ISR is a line that runs in a context where normal rules do not apply. Keeping ISRs minimal is not a best practice — it is a hard constraint imposed by the execution environment.

---

## 31.6 Layering Pays Off Precisely When Things Go Wrong

The I²C layer was modified three times after the sensor driver was complete:

1. Timeout reduced from 1000 ms to 10 ms
2. FreeRTOS mutex added to serialize bus access
3. Mutex resource leak fixed — release on all exit paths

In each case, `bma423.c` was untouched. The sensor driver saw none of these changes because it calls only `i2c_read()` and `i2c_write()` — it has no knowledge of timeouts, mutexes, or internal driver state.

This is the concrete payoff of layering: changes to implementation details do not propagate across layer boundaries if the interface contract is maintained. During development, this meant three potentially destabilizing changes to a foundational layer produced zero regression in the sensor layer. In a production codebase with multiple engineers, this property scales — teams can modify their layers independently without coordinating every detail with adjacent teams.

The effort to maintain the layering — keeping ESP-IDF types out of `bma423.c`, keeping GPIO numbers out of `main.c`, keeping recovery logic in the task layer — paid back that effort with interest during every subsequent modification.

---

## 31.7 Silent Failures Are More Dangerous Than Loud Ones

This project produced two silent failures:

The first: accelerometer disabled, driver returning zeros with `BMA423_OK`. The system appeared to work. Data was being read. No errors were reported. The output was wrong.

The second: mutex acquired, transaction failed, early return without releasing mutex. The I²C bus appeared dead. No error indicated that a mutex was permanently held. Every subsequent transaction timed out waiting for a mutex that would never be released.

In both cases, the failure was invisible at the point of occurrence and only detectable through its downstream effects — zero acceleration data in the first case, total I²C bus loss in the second. Both required reasoning backwards from the symptom to the cause.

Loud failures — crashes, assertions, explicit error codes — are easier to debug because they point to their own cause. Silent failures require the engineer to notice that something is wrong before the debugging process can even begin. Defensive design that makes failures loud — read-back verification, explicit enable writes, mutex release on every exit path — is not paranoia. It is the recognition that silent failures are the hardest class of embedded bug to find and fix.

---

## 31.8 Test the Recovery, Not Just the Happy Path

Level 2 and Level 3 of the recovery ladder were implemented correctly on the first attempt. They were also completely untested until fault injection was added. A recovery path that has never executed is indistinguishable from a recovery path that does not work — until a real failure occurs in a context where debugging is difficult or impossible.

The fault injection mechanism — a single boolean flag that forces `bma423_read_accel()` to return `BMA423_ERR_BUS` unconditionally — required perhaps thirty minutes to implement and confirmed in one test run that:

- Level 1 exhausted correctly
- Level 2 triggered and executed re-initialization correctly
- Level 3 triggered and suspended the task without crashing the system
- The system continued running after task suspension

Without fault injection, all of this would have remained untested. In a real deployment, the first time Level 2 or Level 3 triggered would have been during a real hardware failure — the worst possible time to discover that the recovery code has a bug.

---

## 31.9 Read the Full Register Entry, Not Just the Bits You Care About

The `STATUS` register was first examined to find the `drdy_acc` bit at position 7. The rest of the register was initially ignored. A later look at the full register entry found that the reset value is `0x10` — `cmd_rdy` (bit 4) is set at power-on.

This single fact changed the initialization design: there is no need to poll `cmd_rdy` before sending configuration commands — the sensor is ready immediately after the post-reset delay. If the full register entry had not been read, a polling loop might have been added unnecessarily, or worse, a polling loop might have been added incorrectly (polling the wrong bit) and silently passed because `cmd_rdy` was already set.

Every register in this driver was read in full before any code was written that touched it. Bit fields that were not directly used were still read and understood — because adjacent bits, reset values, and access restrictions (read-only, write-only, read/write) all affect how the register behaves when the bits you do care about are written.

---

## 31.10 Document What You Did Not Do and Why

The migration to `driver/i2c_master.h` was assessed and deferred. The `drdy` check was removed from `bma423_read_accel()` and the reason was documented in a code comment. The `xQueueSendFromISR` return value is not checked — and this is documented as a known gap in Section 34.

Undocumented decisions look like oversights. Documented decisions — even decisions not to do something — demonstrate that the option was considered, the trade-off was understood, and the choice was deliberate. In a code review or an interview, "I decided not to do X because Y" is a stronger answer than silence followed by "I hadn't thought of that."

The gaps in this driver are not hidden. They are in Section 34 with explanations. That transparency is itself an engineering judgment — it is more valuable to know the exact boundaries of what a system handles than to have vague confidence that it handles everything.

