# 21 — Error Handling Strategy

Earlier sections described error handling mechanics as they came up
(Section 9.4/9.5 — error taxonomies; Section 10.3 — failure call flow;
Section 11 — state machine; Section 12.3 — error data flow). This section
steps back and states the **strategy** those mechanics implement — the
policy decisions that everything else follows from.

## 21.1 Core Strategy Statement

Every I2C transaction result is checked at the point of call. No result is
ever assumed successful, and no error is ever silently discarded. Failure
handling policy is decided at exactly one place per failure class — never
duplicated, never overridden layer-to-layer.

## 21.2 Three Distinct Error-Handling Policies, by Layer

This project does **not** use one uniform error-handling policy — it
deliberately uses three different policies depending on *when* in the
system's lifecycle a failure occurs, because the right response to a
failure depends on what stage produced it.

| Layer / Phase | Policy | Why this policy fits this phase |
|---|---|---|
| **Init-time** (`bma423_init()`, `power_init()`, `i2c_init()`) | Fail-fast, no retry, immediate abort | Init-time failures (bad chip ID, failed config verification, can't read PMIC) usually indicate a wiring, power-sequencing, or hardware-identity problem. Retrying a fixed number of times is unlikely to fix a problem that isn't transient — better to surface the failure immediately and loudly than mask it with a retry loop that just delays an inevitable failure |
| **Steady-state runtime** (`bma423_task`'s read path) | Bounded tiered recovery (retry → re-init → suspend) | Runtime bus faults during normal operation (NACK from bus noise, transient contention with the AXP202 driver) are far more likely to be transient. A tiered approach gives cheap recovery a chance first (retry) before escalating to expensive recovery (full re-init), and only gives up (suspend) after both have been exhausted |
| **ISR context** | No error handling at all — by design | The ISR performs one operation (`xQueueSendFromISR`) that is not expected to meaningfully fail in a way the ISR itself could usefully react to. Error handling is deferred entirely to task context, consistent with Section 17.3's reasoning for keeping the ISR minimal |

## 21.3 What "Fail-Fast" Actually Means Here (Init-Time)

Concretely: every `bma423_init()` failure path does exactly one thing —
`return <specific error code>` — with no retry, no fallback configuration,
no "proceed anyway with defaults." `main.c` mirrors this by checking every
subsystem init's return value and aborting the entire boot sequence on the
first failure (Section 14.4). The system does not have a degraded-but-
functional boot mode; it either reaches full operational state or it
doesn't proceed past the failing stage at all.

**Why this is the right call for this project, not just a default:** a
firmware that "proceeds anyway" after a failed chip-ID check or a failed
config verification would produce a system that appears to boot
successfully but silently produces wrong or absent data — this is a worse
failure mode than a loud boot-time abort, because it defers discovery of
the problem to whoever is consuming the (bad) sensor data downstream,
likely much later and with much less context about the root cause.

## 21.4 What "Tiered Recovery" Actually Means Here (Runtime)

The tiered ladder (Section 10.3, Section 18.2) is not a generic retry-until-
give-up loop — it escalates through qualitatively different recovery
actions, each more expensive and more invasive than the last:

1. **Retry** — re-attempt the exact same operation, assuming the bus/sensor
   state is otherwise intact and the failure was a one-off transient event.
2. **Re-init** — assume the sensor's internal configuration state may have
   been disturbed (e.g. by a brown-out or bus glitch) and re-establish it
   from scratch via the full `bma423_init()` sequence, then verify with a
   read before declaring success.
3. **Suspend** — assume the fault is not transient and further automated
   attempts are not productive; stop consuming CPU/bus cycles on a failing
   subsystem and report a clear terminal state rather than looping forever.

Each tier is bounded (`MAX_RETRY_COUNT`, `MAX_REINIT_COUNT`) — there is no
unbounded retry anywhere in this system. This is a deliberate rejection of
"retry forever," discussed further in Section 23 (Design Decisions).

## 21.5 Error Propagation Rule

Once established at a layer boundary, an error type is never leaked
upward past its own layer (Section 12.3 diagrammed this). Concretely:

- `bma423_isr.c` never inspects an `i2c_status_t` or `esp_err_t` directly —
  only `bma423_status_t`.
- `bma423.c` never inspects `esp_err_t` directly — only `i2c_status_t`.
- No layer above `i2c.c` ever calls an ESP-IDF I2C function directly.

This rule is what keeps the recovery-ladder logic in `bma423_isr.c`
decoupled from *how* a failure occurred at the transport level — it only
ever needs to answer "did `bma423_read_accel()` succeed, yes or no," not
"was this a NACK, a timeout, or a mutex contention failure."

## 21.6 What This Strategy Deliberately Does Not Cover

Stated plainly rather than left implicit:

- **No error is currently classified by root cause** at the point of
  recovery decision-making — a NACK and a bus timeout both collapse into
  the same `BMA423_ERR_BUS` and trigger the same Tier 1 retry, even though
  they may warrant different responses in a more mature system (e.g. a
  timeout might suggest a different problem than a NACK).
- **No error/fault event is persisted or reported outside serial logging**
  — there's no fault counter surviving a reset, no persistent fault log,
  no upstream notification to a higher-level watchdog or system health
  monitor. Section 34 (Limitations) covers this as a real gap.
- **The distinction between "sensor genuinely dead" and "sensor
  temporarily unreachable due to bus contention with the AXP202 driver"
  is not made** — both would exhaust the same recovery ladder identically.

