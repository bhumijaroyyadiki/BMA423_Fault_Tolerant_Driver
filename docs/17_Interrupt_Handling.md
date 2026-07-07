# 17 — Interrupt Handling

*(Flag from Section 16.2 still open — I'll keep referencing it as
unverified where relevant rather than assuming it's resolved.)*

## 17.1 Interrupt Source

Single interrupt source: **BMA423 data-ready interrupt**, routed to INT1,
wired to **ESP32 GPIO39**, configured **positive-edge triggered**
(`GPIO_INTR_POSEDGE`).

There is exactly one external interrupt in this system. No other GPIO
interrupts, no shared interrupt lines, no interrupt priority contention
between multiple sensor sources — this simplicity is itself a scope
decision (Section 1.4) worth stating rather than leaving implicit.

## 17.2 ISR Design: What It Does and Deliberately Does Not Do

```c
void IRAM_ATTR bma423_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t dummy_val = 1;

    xQueueSendFromISR(accel_event_queue, &dummy_val, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
```

| Property | Value | Why |
|---|---|---|
| Placement | `IRAM_ATTR` | Forces the ISR into internal RAM rather than flash-mapped memory. On ESP32, flash can be temporarily inaccessible during flash operations (e.g. SPI flash writes/erases elsewhere in the system); an ISR that might fire during such a window **must** live in IRAM or it can crash/hang. This is a correctness requirement on ESP32, not a performance optimization. |
| I2C calls | None | The single most important property of this ISR. I2C transactions are not interrupt-safe to perform directly (they block, they take a mutex, they can take milliseconds) — doing sensor reads inside the ISR would hold off other interrupts and violate real-time behavior for the rest of the system. |
| Payload size | 1 byte (`dummy_val`) | The queue carries a signal, not data. The actual read happens later, in task context, closer to when it's needed — this also means the "settling delay" (Section 12.1, 100 µs) is applied at the correct point: right before the read, not right after the ISR fires, which could otherwise be delayed by scheduler latency anyway. |
| Yield behavior | `portYIELD_FROM_ISR` only if a higher-priority task was woken | Standard FreeRTOS pattern — avoids a hard yield when it isn't needed, minimizing worst-case interrupt latency for whatever else might be pending. |

## 17.3 Why Deferred Interrupt Processing (Not Read-in-ISR)

This is one of the "interview gold" questions from your own scoping notes
(Section 1), so it's worth being explicit about the reasoning rather than
just stating the pattern:

**Alternative considered:** read the 6 data bytes directly inside the ISR
and pass the parsed X/Y/Z values through the queue instead of a dummy
signal byte.

**Why this was rejected:**
- I2C transactions on this platform are not ISR-safe in the way GPIO/queue
  operations are — the ESP-IDF I2C driver's blocking transaction API is
  not designed to be called from interrupt context, and doing so would
  either fail outright or introduce unbounded interrupt latency.
- Even if it were technically callable, holding the I2C mutex from inside
  an ISR while another task (e.g. the AXP202 power path) is waiting on
  that same mutex creates a priority inversion risk that's much harder to
  reason about than deferring the read to a normal task.
- The 100 µs settling delay (Section 12.1) needs to happen immediately
  before the read, not immediately after the interrupt — moving the read
  into the ISR wouldn't eliminate the need for that delay, it would just
  relocate a busy-wait into interrupt context, which is worse for overall
  system latency, not better.

**Why the chosen design (signal-only ISR + deferred task read) was
selected:** it keeps interrupt latency minimal and bounded (a queue send is
O(1) and fast), and pushes all variable-latency work (I2C transaction,
retries, potential re-init) into a normal-priority task context where the
FreeRTOS scheduler can manage it alongside everything else in the system.

## 17.4 Interrupt Timing Diagram

```
Time ──────────────────────────────────────────────────────────▶

BMA423 internal:
  ADC conversion complete ──┐
                            │  (some internal latency before
                            │   DRDY status register updates)
                            ▼
  INT1 pin asserts ─────────●
                            │
ESP32 GPIO39:               │
  Edge detected ─────────────●───┐
                                 │ (interrupt latency —
                                 │  ISR entry, hardware +
                                 │  FreeRTOS interrupt overhead)
  bma423_isr_handler() runs ─────●
    xQueueSendFromISR() ─────────●
    portYIELD_FROM_ISR() ────────●───┐
                                     │ (scheduler decides
                                     │  whether to context-
                                     │  switch immediately
                                     │  or continue current
                                     │  ISR-return path)
  bma423_task() resumes ───────────────●
    xQueueReceive() unblocks ──────────●
    BMA423_DELAY_US(100) ──────────────●───┐
                                            │ 100µs busy-wait —
                                            │ covers the gap between
                                            │ "interrupt fired" and
                                            │ "internal ADC transfer
                                            │  fully settled"
    i2c_read() burst (6 bytes) ─────────────●
```

The 100 µs delay is an implementation-specific safeguard, not a datasheet-mandated requirement. The BMA423 datasheet indicates the data-ready interrupt is asserted only after a complete sample set is available, but specifies no explicit post-interrupt settling delay. This 100 µs guard interval was introduced during implementation as a conservative margin against that assumption, not derived from a cited timing parameter.
This is also a quantifiable, named optimization opportunity: at 25 Hz ODR, this delay costs 2.5 ms of busy-wait CPU time per second (100 µs × 25 samples/sec). In a power-constrained production build, this delay could potentially be removed after scope/logic-analyzer validation confirms data register stability at the moment of interrupt assertion — trading a currently-unverified assumption for a measured one. This is carried forward as a specific line item in Section 36 (Possible Optimizations), not left as a vague "could be faster" statement.

## 17.5 Interrupt-Related Failure Modes This Design Addresses

| Failure mode | How this design handles it |
|---|---|
| Interrupt fires before data fully settled | 100 µs settling delay before read (empirical, pending your confirmation above) |
| Missed interrupt while task is busy in recovery (Tier 1/2) | Queue depth of 10 provides limited buffering, but see honest caveat below |
| Spurious interrupt before sensor configured | Prevented architecturally — ISR isn't installed until `bma423_init()` fully succeeds (Section 14.4) |
| ISR runs too long / blocks other interrupts | Prevented by design — ISR does no blocking work |

**Honest caveat, not glossed over:** the queue depth of 10 means up to 10
pending "data ready" signals can buffer if the task falls behind (e.g.
while stuck in the Tier 2 re-init loop, which itself takes ≥150 ms across
3 attempts at 50 ms each). At 25 Hz ODR, that's a 400 ms buffer before
events would start being dropped by `xQueueSendFromISR` failing silently
(the return value of `xQueueSendFromISR` is not currently checked in
`bma423_isr_handler` — a missed send is not detected or logged). This is a
real, currently-unaddressed gap, not a resolved edge case — noted properly
in Section 34 (Limitations) rather than hidden.
