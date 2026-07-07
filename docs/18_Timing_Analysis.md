# 18 — Timing Analysis

Sections 13.2 and 17.4 covered init-time timing and interrupt-to-read
timing individually. This section consolidates timing analysis
system-wide: steady-state read latency, worst-case recovery duration, and
throughput bounds — the numbers a reviewer would actually want to see
totaled up.

## 18.1 Steady-State Read Latency Budget (Single Sample, No Faults)

| Stage | Time | Source |
|---|---|---|
| Hardware interrupt latency (ADC-ready → INT1 pin assertion) | Not measured (datasheet-internal, not characterized in this project) | Not independently verified — flagged as unknown rather than estimated |
| GPIO edge → ISR entry (ESP32 interrupt latency) | Not measured (typically low microseconds on ESP32, not empirically captured here) | Not independently verified |
| ISR execution (`xQueueSendFromISR` + conditional yield) | Sub-microsecond scale, not profiled | Not independently verified |
| Task wake + `xQueueReceive` unblock | Scheduler-dependent, not profiled | Not independently verified |
| Settling delay | 100 µs (fixed) | `BMA423_DELAY_US(100)` in `bma423_read_accel()` |
| I2C burst read (6 bytes, 400 kHz) | Bounded by 10 ms timeout; actual transaction time at 400 kHz for ~9 bytes on the wire (addr+reg+restart+addr+6 data, with ACKs) is on the order of low tens of microseconds, not independently measured | Timeout ceiling from `i2c_master_cmd_begin(..., pdMS_TO_TICKS(10))`; real-world value not captured with a scope/logic analyzer |
| Mutex acquisition (uncontended) | Near-zero; bounded worst case 50 ms if contended | `xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50))` |

**Honest summary:** the only latency figures in this system that are
*known with certainty* are the fixed delays explicitly coded
(100 µs, 1000 µs, 50 ms, 10 ms, 50 ms) and the timeout ceilings. Actual
interrupt-to-ISR latency, scheduler wake latency, and real I2C bus
transaction time were **not measured with a scope or logic analyzer** in
this project — stating a specific total end-to-end latency number would
be fabricating precision this project doesn't have evidence for. This is
flagged explicitly rather than presenting an invented "typical latency:
Xµs" figure, which is the kind of unverified specific I was told to avoid
assuming.

## 18.2 Worst-Case Recovery Duration (Full Ladder, All Tiers Exhausted)

This *can* be stated precisely, since it's entirely composed of fixed,
code-defined delays and bounded loop counts — no measurement required.

```
Tier 1 (retry):     3 attempts × 10 ms delay           =  30 ms
Tier 2 (re-init):   3 attempts × 50 ms delay           = 150 ms
                    + 3 × full bma423_init() sequence
                      (~104 ms each, per Section 13.2)  = 312 ms
                    + 3 × verification read
                      (~0.1 ms settling + I2C time,
                       negligible relative to above)    ≈ negligible
                                                        ─────────
Total worst-case time from first failed read to        ≈ 492 ms
"[CRITICAL] ... Suspending BMA423 task":
```

**This number is derivable and defensible**, unlike 18.1 — it's a sum of
fixed constants and known loop bounds in the code (`MAX_RETRY_COUNT=3`,
`RETRY_DELAY_MS=10`, `MAX_REINIT_COUNT=3`, `REINIT_DELAY_MS=50`, plus the
~104 ms init sequence from Section 13.2 repeated up to 3 times inside
Tier 2). Roughly **half a second** from first fault to subsystem
suspension in the worst case — a concrete, useful number for anyone
integrating this driver who needs to know how long the accelerometer
data stream could go silent during a fault before the system gives up.

## 18.3 Throughput

| Parameter | Value | Basis |
|---|---|---|
| Configured ODR | 25 Hz | `ACC_ODR_25HZ` in `ACC_CONF` (Section 16.2) |
| Nominal sample period | 40 ms | 1/25 Hz |
| Queue buffering headroom | 10 events × 40 ms = 400 ms | `ACCEL_QUEUE_DEPTH = 10` (Section 17.5) |
| I2C bus utilization | Not calculated — would require the unmeasured transaction time from 18.1 | Flagged as not computed rather than estimated |

**Note on 18.2 vs 18.3 interaction, worth stating plainly:** the worst-case
recovery duration (~492 ms) exceeds the queue's buffering headroom
(~400 ms) at the configured 25 Hz ODR. This means that in the actual
worst case — all three retry attempts fail, all three re-init attempts
fail — **some data-ready events would be silently dropped** before
recovery either succeeds or the task suspends, because
`xQueueSendFromISR`'s return value isn't checked (same gap noted in
Section 17.5). This is a real, quantifiable consequence of the current
design, surfaced here because the numbers make it visible — not
something I'm inferring without basis; it falls directly out of 18.2 and
18.3 together. It belongs in Section 34 (Limitations) as a concrete,
numbers-backed limitation rather than a vague one.

## 18.4 Datasheet Timing Requirements This Design Complies With

| Requirement | Datasheet minimum (as encoded in this project's constants) | This design's value | Margin |
|---|---|---|---|
| Inter-register-write delay | 1000 µs (per code comment referencing datasheet) | 1000 µs exactly | No margin — matches minimum exactly |
| Post-soft-reset settle time | ~2 ms per code comment | 50 ms | 25× margin |
| AXP202/BMA423 NVM load stabilization | ~2 ms per code comment | 50 ms | 25× margin |

**Flag, consistent with Section 16.2's open question:** these datasheet
minimums are taken from the code comments (`bma423_platform.h`,
`power_init()`), which I'm treating as your own datasheet research from
Week 0 rather than something I've independently verified against the
Bosch datasheet PDF myself. If you'd like, I can note in the document that
these are "per project's Week 0 datasheet review" rather than presenting
them as independently re-verified by this documentation process.

