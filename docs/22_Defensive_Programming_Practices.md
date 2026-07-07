# 22 — Defensive Programming Practices

This section catalogs concrete defensive patterns actually present in the
code — not a generic "always check your inputs" list. Each entry below
points to a real instance and explains what specific failure it guards
against.

## 22.1 Input Validation

```c
if (x == NULL || y == NULL || z == NULL)
{
    return BMA423_ERR_INVALID_ARG;
}
```
`bma423_read_accel()` validates all three output pointers before doing any
I2C work. This guards against a caller passing a NULL pointer and the
driver later dereferencing it during the sign-extension step — a crash
that would otherwise occur deep inside the driver rather than at the
caller's mistake.

```c
if (len == 0 || data == NULL) {
    xSemaphoreGive(i2c_mutex);
    return I2C_ERR_INVALID_ARG;
}
```
`i2c_read()` in `i2c.c` similarly guards against a zero-length or NULL
read request — and notably **releases the mutex before returning** in this
early-exit path, which matters (see 22.4).

## 22.2 Write Verification (Trust but Verify)

Every configuration register write in `bma423_init()` — `ACC_CONF`,
`ACC_RANGE`, `INT1_IO_CTRL`, `INT_MAP`, `PWR_CTRL` — is immediately
followed by a read-back and an explicit comparison:

```c
if (acc_conf_readback != acc_conf)
{
    printf("[ACC_CONF] Verification failed\n");
    return BMA423_ERR_CONFIG;
}
```

This guards against a specific, real class of I2C bug: a write that the
bus layer reports as "successful" (ACKed) but that didn't actually take
effect at the device — for instance, due to the device being mid-reset,
in an unexpected internal state, or a marginal signal-integrity issue that
corrupts data without producing a NACK. An ACK only confirms the device
accepted the byte on the wire; it does not confirm the device's internal
register logic applied it. This driver treats those as two separate facts
and checks both.

## 22.3 Bounded Everything — No Unbounded Waits or Loops

| Mechanism | Bound | Guards against |
|---|---|---|
| Mutex acquisition | 50 ms timeout (`xSemaphoreTake`) | A stuck bus or a task that never releases the mutex hanging every other caller forever |
| I2C transaction | 10 ms timeout (`i2c_master_cmd_begin`) | A hung bus (e.g. a slave holding SDA low) blocking the calling task indefinitely |
| Retry ladder Tier 1 | `MAX_RETRY_COUNT = 3` | Retrying a permanently-failing operation forever, starving the rest of the system |
| Retry ladder Tier 2 | `MAX_REINIT_COUNT = 3` | Same, at the more expensive re-init level |

There is no `while(1)` anywhere in this codebase that isn't either a
task's intended top-level loop (`bma423_task`) or explicitly and
deliberately terminated via `vTaskSuspend` (Tier 3). This is a deliberate
absence, not an oversight — every wait in this system either completes,
times out, or is a bounded retry.

## 22.4 Resource Cleanup on Partial Initialization Failure

`bma423_isr_init()` is the clearest example of this in the codebase — it
allocates four resources in sequence (queue, GPIO config, ISR service,
ISR handler, task) and **unwinds every resource it already allocated** if
a later step fails:

```c
if (gpio_config(&io_conf) != ESP_OK) {
    vQueueDelete(accel_event_queue);
    accel_event_queue = NULL;
    return BMA423_ERR_CONFIG;
}
...
if (task_err != pdPASS) {
    gpio_isr_handler_remove(BMA423_INT1_GPIO);
    vQueueDelete(accel_event_queue);
    accel_event_queue = NULL;
    return BMA423_ERR_CONFIG;
}
```

This guards against two specific problems: a resource leak (a queue
allocated but never freed if a later step fails), and a **dangling ISR
pointer** — the comment in the code itself flags this concern directly
("Clean up hardware hooks before exiting to prevent dangling ISR
pointer"). Without this cleanup, a failed `bma423_isr_init()` call could
leave a GPIO interrupt armed with a handler pointing at a task that was
never successfully created — a genuinely dangerous state that could crash
the system the next time the interrupt fires.

## 22.5 Mutex Release on Every Exit Path

Both `i2c_read()` and `i2c_write()` guarantee `xSemaphoreGive(i2c_mutex)`
is called on every code path that acquired it — including the early
validation-failure exit in `i2c_read()` (22.1). This guards against the
classic "took the mutex, hit an early-return, forgot to release it" bug
class, which would otherwise permanently deadlock every subsequent I2C
call in the system after a single invalid-argument call.

## 22.6 Dummy Read After Reset

```c
i2c_read(BMA423_ADDR, BMA423_CHIP_ID_REG, &chip_id, 1);  // dummy, result discarded
```

This is a deliberate, slightly unusual pattern worth explaining rather
than looking like dead code: a throwaway read is performed and its result
intentionally discarded, before the real chip-ID read that's actually
evaluated. This guards against residual reset-state artifacts on the bus
or in the device's internal state machine immediately following a soft
reset — the comment in the code (`Dummy Read to clear reset state and
start internal processes`) documents this as an intentional stabilization
step, not an accidental leftover call.

## 22.7 IRAM Placement for ISR Safety

```c
void IRAM_ATTR bma423_isr_handler(void *arg)
```

Already covered in Section 17.2, included here because it is itself a
defensive practice: this guards against a class of bug specific to
flash-based MCUs where an ISR located in flash-mapped memory can fail or
hang if it executes during a window when flash access is temporarily
unavailable (e.g. concurrent SPI flash writes elsewhere in the system).

## 22.8 What's Notably *Not* Defended Against (Honest Gaps)

Consistent with not overselling this section:

- `xQueueSendFromISR`'s return value is not checked (Section 17.5) — a
  full queue silently drops the event with no counter or log.
- No stack canary / overflow detection beyond FreeRTOS's optional built-in
  stack-overflow checking (not confirmed enabled/configured in this
  project — I don't have `sdkconfig` to verify this, so I'm not claiming
  it either way).
- No watchdog timer integration is present in the shared code — if
  `bma423_task` were to hang in a way the recovery ladder doesn't cover,
  nothing external would catch it.

These gaps are catalogued properly in Section 34 (Limitations) rather than
repeated in full here.

