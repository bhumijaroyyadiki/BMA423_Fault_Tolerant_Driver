# 23_Design_Decisions.md

This section documents the non-obvious design decisions made during this project — not what was built, but why each significant choice was made over its alternatives, what was explicitly rejected, and what the real trade-off was. Every decision here was made consciously, not by default.

---

## 23.1 Event-Driven Architecture Over Polling

**Decision:** The driver uses a hardware interrupt on INT1 to signal data availability. The main task blocks on a FreeRTOS queue and only wakes when the ISR deposits an event.

**Rejected alternative:** A polling loop calling `bma423_read_accel()` on a fixed timer — simpler to implement, no GPIO configuration required.

**Why rejected:** Polling at 25 Hz means the task wakes 25 times per second regardless of whether new data exists. With an interrupt-driven design the task sleeps in `xQueueReceive(..., portMAX_DELAY)` and the CPU is free to idle between samples. On a wearable running on battery, idle time is directly recoverable as battery life. Polling also introduces a phase relationship problem — a fixed timer and a fixed ODR can drift relative to each other, causing reads to land between samples and return stale data. The interrupt eliminates that entirely: the sensor tells the MCU exactly when data is ready.

**Trade-off accepted:** Edge-triggered interrupts introduce a missed-interrupt risk if the CPU is busy during the rising edge window. This was mitigated by the drain-queue pattern in `bma423_task()` and documented in the retry recovery ladder.

---

## 23.2 FreeRTOS Queue Over Volatile Flag

**Decision:** The ISR signals the task via `xQueueSendFromISR()` into a depth-5 queue, not by setting a `volatile bool` flag.

**Rejected alternative:** A single `volatile bool g_data_ready` flag set in the ISR and polled in the task.

**Why rejected:** A flag is a single bit of state. If the ISR fires three times while the task is busy, the flag is set three times but the task sees only one event — two samples are silently lost with no indication anything was dropped. A queue preserves event count: three ISR firings deposit three entries, the task drains all three. At 25 Hz with a queue depth of 5, the task has 200 ms of slack before overflow — sufficient for any realistic FreeRTOS scheduling jitter on this platform.

**Trade-off accepted:** A queue requires heap allocation at init time and adds a small overhead per event versus a flag read. Both costs are negligible at 25 Hz. The `xQueueSendFromISR` return value is currently not checked — a known gap documented in Section 34.

---

## 23.3 Minimal ISR — Deferred Processing

**Decision:** `bma423_isr_handler()` does exactly two things: deposits a byte into the queue and yields if a higher-priority task was woken. No I2C reads, no register access, no data processing happen inside the ISR.

**Rejected alternative:** Reading the six acceleration registers directly inside the ISR.

**Why rejected:** `i2c_read()` internally acquires a FreeRTOS mutex via `xSemaphoreTake()`. Mutex acquisition inside an ISR is illegal in FreeRTOS — it will either deadlock or corrupt the scheduler state depending on timing. Even setting aside FreeRTOS constraints, an I2C transaction at 400 kHz transferring 6 bytes takes approximately 150–200 µs. Holding the CPU inside an ISR for 200 µs at 25 Hz means 0.5% of CPU time is spent in non-preemptable ISR context — time during which all equal or lower priority interrupts are blocked. For a peripheral driver this is unacceptable design.

**Trade-off accepted:** Deferred processing introduces a small latency between interrupt assertion and data availability to the application. At 25 Hz ODR this latency is irrelevant — the application has 40 ms per sample window.

---

## 23.4 Edge-Triggered Over Level-Triggered Interrupt

**Decision:** INT1 is configured as rising-edge triggered (`GPIO_INTR_POSEDGE`), not level-triggered.

**Rejected alternative:** Level-triggered interrupt, which keeps asserting as long as the pin stays high.

**Why edge was chosen:** Level-triggered interrupts require the ISR to clear the interrupt source — specifically read the data register — before returning, otherwise the ISR fires again immediately in an infinite loop. This re-introduces the problem from 23.3: a data read inside or immediately mandated by the ISR. Edge triggering fires once per rising edge and requires no such clearance, keeping the ISR genuinely minimal.

**Risk acknowledged:** If the CPU misses the rising edge — because it was in a higher-priority context during that specific clock cycle — no re-trigger occurs and that sample is lost. This risk was accepted because at 25 Hz the probability of missing an edge due to scheduler preemption is extremely low, and the recovery ladder in `bma423_task()` handles the occasional missed sample gracefully.

---

## 23.5 Push-Pull Active-High Output on INT1

**Decision:** INT1 configured as push-pull output, active high (`lvl = 1`, `od = 0` in `INT1_IO_CTRL`).

**Rejected alternative:** Open-drain with external or internal pull-up, active low.

**Why push-pull:** Push-pull actively drives both logic states — the sensor drives the line high on interrupt and low otherwise. No external resistor is required, transitions are fast and clean, and there is no continuous current draw through a pull-up resistor during the idle-low state. Active high was chosen to align with the ESP32 GPIO39 default state — the line sits low at rest and rises on interrupt, which is the natural polarity for `GPIO_INTR_POSEDGE`.

**Open-drain rejected because:** GPIO39 on the ESP32 has no internal pull-up capability (it is an input-only pin). Open-drain without a pull-up leaves the line floating in the idle state — a genuinely dangerous configuration that would cause spurious interrupts from noise pickup.

---

## 23.6 Layered Software Architecture

**Decision:** The codebase is divided into four strictly separated layers: `i2c.c` (bus transactions), `power.c` (PMIC), `bma423.c` (sensor logic), `bma423_isr.c` (interrupt infrastructure). `main.c` orchestrates but contains no hardware detail.

**Rejected alternative:** A flat single-file implementation where sensor logic, I2C transactions, and ISR setup are interleaved.

**Why layered:** During development the I2C layer was modified three times — timeout tuning, mutex addition, mutex resource-leak fix — without touching a single line of `bma423.c`. This is the concrete payoff of layering: changes to one layer's implementation don't propagate upward if the interface contract is maintained. The same property means `bma423.c` could be ported to STM32 by replacing only `i2c.c` with an STM32 HAL implementation — the sensor logic, register map, and recovery architecture are entirely platform-agnostic.

**Trade-off accepted:** More files, more header dependencies, slightly more indirection to trace a single call path. Accepted without hesitation — the maintenance and portability benefit dominates at any non-trivial project size.

---

## 23.7 Sensor-Only Re-init in Recovery Ladder — Not Full Bus Reset

**Decision:** Level 2 recovery calls `bma423_init()` only — it does not tear down and rebuild the I2C driver.

**Rejected alternative:** Full `i2c_driver_delete()` + `i2c_init()` sequence as the first recovery response.

**Why rejected:** The I2C bus is shared between three devices — BMA423 (0x19), AXP202 PMIC (0x35), and PCF8563 RTC (0x51). Deleting and reinstalling the I2C driver mid-operation would invalidate the mutex, interrupt any in-progress transaction on the shared bus, and potentially corrupt the state of the power management IC — a device that, if misconfigured, can remove power from the ESP32 itself. Full bus reset is therefore reserved as a last resort only, and is not implemented in the current recovery ladder. The observed failure mode — ESP-IDF legacy driver returning `ESP_FAIL (0x107)` periodically — does not require bus reset; `bma423_init()` alone is sufficient.

---

## 23.8 Explicit Register Configuration Despite Correct Reset Defaults

**Decision:** `ACC_RANGE` is explicitly written to `0x01` (±4g) in `bma423_init()` even though the BMA423 reset default for that register is also `0x01`.

**Rejected alternative:** Skip the write since the default is already correct.

**Why explicit:** Relying on reset defaults creates a hidden dependency — the code silently assumes the sensor has been freshly reset and no prior code has touched that register. This assumption breaks in at least three real scenarios: a warm reboot where the sensor was not power-cycled, a partial reset that didn't restore all registers, or a cosmic-ray bit flip corrupting a register during operation. Explicit configuration is self-documenting, resilient to all three scenarios, and costs one I2C write at startup — a negligible price.

---

## 23.9 ODR = 25 Hz, Range = ±4g, Bandwidth = avg4 / Power Mode

**Decision:** ACC_CONF written as `0x26` — 25 Hz ODR, 4-sample averaging, power mode. ACC_RANGE written as `0x01` — ±4g.

**Rationale:**

*ODR:* Human walking produces acceleration events at approximately 1–2 Hz. By Nyquist, a minimum of 4 Hz sampling is required to reconstruct the signal. 25 Hz was chosen to provide sufficient margin to capture the full waveform shape of a step — the heel-strike and push-off peaks — without the aliasing that would occur at the minimum rate. Rates above 50 Hz provide no additional signal content for step detection and increase power consumption proportionally.

*Range:* Normal walking generates approximately 1.5g peak acceleration; running approximately 3g. ±2g was rejected because it clips on running and sharp wrist movements. ±8g provides unnecessary headroom and halves resolution compared to ±4g. ±4g covers all expected motion without clipping and provides 12-bit resolution of approximately 2mg per LSB.

*Bandwidth:* Power mode with 4-sample averaging was chosen over performance mode because the application — step counting — involves slow, predictable motion where noise reduction matters more than response speed. The averaging filter suppresses vibration artifacts (bumpy road, surface texture) that would otherwise register as false steps. The power consumption reduction from performance mode to power mode is a secondary benefit consistent with wearable battery constraints.

