# 11 — State Machine

**Note on scope:** there is no explicit state variable or enum tracking
driver state anywhere in the shared code (`bma423.c`, `bma423_isr.c`). What
follows is the **implicit state machine** — inferred from control flow and
return-value handling — not a state machine that exists as a literal
`typedef enum { ... } driver_state_t` in the source. This distinction is
stated here rather than left ambiguous, since claiming an explicit state
machine that doesn't exist in code would misrepresent the implementation.

## 11.1 Implicit States

| State | Entered when | Exited when |
|---|---|---|
| **UNINITIALIZED** | System reset / power-on | `bma423_init()` called |
| **INITIALIZING** | `bma423_init()` begins execution | `bma423_init()` returns |
| **CONFIGURED** | `bma423_init()` returns `BMA423_OK` | `bma423_isr_init()` called |
| **ARMED** | `bma423_isr_init()` returns `BMA423_OK` — ISR installed, task running, blocked on queue | Interrupt fires |
| **ACQUIRING** | Task unblocks from `xQueueReceive()` | `bma423_read_accel()` returns |
| **RECOVERING_L1** (retry) | `bma423_read_accel()` returns non-`BMA423_OK` | Retry succeeds (→ ARMED) or retries exhausted (→ RECOVERING_L2) |
| **RECOVERING_L2** (re-init) | Tier 1 retries exhausted | Re-init + verification succeeds (→ ARMED) or re-init attempts exhausted (→ SUSPENDED) |
| **SUSPENDED** | Tier 2 recovery exhausted | Never — terminal state until external reset |

## 11.2 State Diagram

```
                    ┌───────────────┐
                    │ UNINITIALIZED │
                    └───────┬───────┘
                            │ bma423_init()
                            ▼
                    ┌───────────────┐
              ┌─────│ INITIALIZING  │
              │     └───────┬───────┘
              │  fail       │ BMA423_OK
              │ (BUS/CHIP_ID/       ▼
              │  FATAL/CMD/  ┌───────────────┐
              │  CONFIG)     │  CONFIGURED   │
              │              └───────┬───────┘
              │                      │ bma423_isr_init()
              ▼                      ▼
      ┌───────────────┐      ┌───────────────┐
      │  INIT FAILED  │      │     ARMED     │◀────────────┐
      │ (main.c abort)│      │ (blocked on   │              │
      └───────────────┘      │  event queue) │              │
                              └───────┬───────┘              │
                                      │ interrupt fires       │
                                      ▼                       │
                              ┌───────────────┐               │
                              │  ACQUIRING    │               │
                              └───────┬───────┘               │
                             success  │  failure               │ retry/reinit
                                      │                        │ succeeds
              ┌───────────────────────┘                        │
              │                                                 │
              └─────────────────────────────────────────────────┘
                                      │ failure
                                      ▼
                              ┌────────────────┐
                              │ RECOVERING_L1   │
                              │ (retry, ≤3x)    │
                              └───────┬─────────┘
                        succeed│      │ exhausted
                    (back to ARMED)   ▼
                              ┌────────────────┐
                              │ RECOVERING_L2   │
                              │ (re-init, ≤3x)  │
                              └───────┬─────────┘
                        succeed│      │ exhausted
                    (back to ARMED)   ▼
                              ┌────────────────┐
                              │   SUSPENDED     │
                              │ (terminal —     │
                              │  vTaskSuspend)  │
                              └────────────────┘
```

## 11.3 Notable Properties of This State Machine

- **No state is re-entrant into itself via a different failure class.**
  Whether a failure is a NACK, a timeout, or a bad read, it collapses into
  the same `RECOVERING_L1` state — the state machine doesn't distinguish
  *why* the read failed, only *that* it failed. This is a simplification,
  not an oversight: distinguishing fault type would require exposing more
  of `i2c_status_t` up through `bma423_status_t`, which the layer boundary
  in Section 8/10.4 deliberately does not do.
- **`SUSPENDED` is terminal by design.** There is no automatic path back to
  `ARMED` from `SUSPENDED` — recovery from a fully-exhausted fault requires
  external intervention (reset). This is a deliberate stop, not a missing
  transition — see Section 23 (Design Decisions) for why unbounded
  auto-recovery was rejected.
- **`INIT FAILED` and `SUSPENDED` are architecturally distinct**, even
  though both represent "the sensor is unusable." `INIT FAILED` happens
  before the ISR/task pipeline ever exists (system aborts in `main.c`,
  never reaches `ARMED`); `SUSPENDED` happens after the pipeline was
  successfully running and later failed. Conflating the two would obscure
  *when* in the system's life the failure occurred, which matters for
  debugging (Section 29).

