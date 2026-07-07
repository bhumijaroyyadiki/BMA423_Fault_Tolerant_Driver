# 10 — Layered Software Design

Section 8 showed the *static* layer structure. This section shows the
*dynamic* view — how a single unit of work moves through those layers,
which is a different and equally necessary thing to document.

## 10.1 Call Flow: Initialization Sequence

```
main.c : app_main()
   │
   ├─▶ i2c.c        : i2c_init()
   │                    creates i2c_mutex, configures I2C_NUM_0
   │
   ├─▶ power.c      : power_init()
   │       │
   │       └─▶ i2c.c : axp202_read_reg() → i2c_read()
   │                    (via i2c.c, mutex-protected)
   │
   ├─▶ bma423.c     : bma423_init()
   │       │
   │       ├─▶ i2c.c : i2c_write()   soft reset
   │       ├─▶ i2c.c : i2c_read()    dummy read
   │       ├─▶ i2c.c : i2c_read()    CHIP_ID
   │       ├─▶ i2c.c : i2c_read()    ERR_REG
   │       ├─▶ i2c.c : i2c_write() + i2c_read()   ACC_CONF (write+verify)
   │       ├─▶ bma423_platform.h : BMA423_DELAY_US(1000)
   │       ├─▶ i2c.c : i2c_write() + i2c_read()   ACC_RANGE (write+verify)
   │       ├─▶ bma423_platform.h : BMA423_DELAY_US(1000)
   │       ├─▶ i2c.c : i2c_write() + i2c_read()   INT1_IO_CTRL (write+verify)
   │       ├─▶ i2c.c : i2c_write() + i2c_read()   INT_MAP (write+verify)
   │       ├─▶ i2c.c : i2c_write()   PWR_CONF
   │       ├─▶ bma423_platform.h : BMA423_DELAY_US(1000)
   │       └─▶ i2c.c : i2c_write() + i2c_read()   PWR_CTRL (write+verify)
   │
   └─▶ bma423_isr.c : bma423_isr_init()
           │
           ├─▶ xQueueCreate()            → accel_event_queue
           ├─▶ gpio_config()             → GPIO39 as edge-triggered input
           ├─▶ gpio_install_isr_service()
           ├─▶ gpio_isr_handler_add()    → bma423_isr_handler bound to GPIO39
           └─▶ xTaskCreate()             → bma423_task spawned last
```

**Why this ordering is the load-bearing part of this diagram, not just
sequence:** every step in `bma423_init()` is complete and verified *before*
`bma423_isr_init()` runs. If the ISR were installed first, a spurious or
early interrupt could fire against a sensor that hasn't been reset,
chip-ID-verified, or configured yet — the recovery ladder in
`bma423_isr.c` has no defined behavior for "interrupt fired against an
unconfigured device," because that state is architecturally prevented
from occurring, not handled after the fact.

## 10.2 Call Flow: Steady-State Read Cycle

```
Hardware: BMA423 INT1 pin rises
   │
   ▼
bma423_isr.c : bma423_isr_handler()          [ISR context]
   │  xQueueSendFromISR(accel_event_queue, &dummy_val, ...)
   │  portYIELD_FROM_ISR() if higher-priority task woken
   ▼
FreeRTOS scheduler wakes bma423_task           [task context]
   │
   ▼
bma423_isr.c : bma423_task()
   │  xQueueReceive() unblocks
   ▼
bma423.c : bma423_read_accel(&x, &y, &z)
   │
   ├─▶ bma423_platform.h : BMA423_DELAY_US(100)   settling delay
   ├─▶ i2c.c : i2c_read(BMA423_ADDR, ACC_X_LSB_REG, buf, 6)
   │       └─▶ ESP-IDF driver/i2c.h : i2c_master_cmd_begin()
   │
   ▼ (on success)
bma423.c : reconstruct 12-bit signed X/Y/Z from buf[0..5]
   ▼
bma423_isr.c : printf("X=%d Y=%d Z=%d\n", ...)
   ▼
back to xQueueReceive(), task blocks again until next interrupt
```

## 10.3 Call Flow: Failure Path Through the Same Cycle

This is the same diagram as 10.2, but with the fault branch expanded — this
is the path that Sections 21 (Interrupt Handling) and 29 (Debugging
Journey) reference directly.

```
bma423.c : bma423_read_accel() returns BMA423_ERR_BUS
   │
   ▼
bma423_isr.c : bma423_task()
   │
   ├─▶ Tier 1: retry loop (≤3 attempts, 10ms apart)
   │       └─▶ bma423.c : bma423_read_accel()  [retried in-place]
   │
   │   if still failing ↓
   │
   ├─▶ Tier 2: re-init loop (≤3 attempts, 50ms apart)
   │       ├─▶ bma423.c : bma423_init()          full re-config sequence
   │       └─▶ bma423.c : bma423_read_accel()    verification read
   │
   │   if still failing ↓
   │
   └─▶ Tier 3: printf("[CRITICAL] ...") + vTaskSuspend(NULL)
              acquisition task halts; system remains up,
              subsystem reported offline
```

## 10.4 Layer Coupling Summary

| Call direction | Allowed? | Example |
|---|---|---|
| Concurrency layer → Protocol layer | Yes | `bma423_task()` calls `bma423_read_accel()` |
| Protocol layer → Transport layer | Yes | `bma423_init()` calls `i2c_write()` |
| Protocol layer → Concurrency layer | **No** | `bma423.c` never touches the queue, task handle, or GPIO |
| Transport layer → Protocol layer | **No** | `i2c.c` has no knowledge of BMA423 registers |
| Power subsystem → Sensor subsystem | **No** | `power.c` and `bma423.c` never call each other directly |

The absence of upward or sideways calls is what keeps the failure-path
diagram in 10.3 legible — a fault at the transport layer surfaces as one
error code at the protocol boundary, and the recovery policy for it lives
in exactly one place (`bma423_isr.c`), not duplicated across layers.

