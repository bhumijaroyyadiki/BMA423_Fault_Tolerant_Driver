# 09 — Driver Architecture

## 9.1 Driver Classification

This is a **register-level, polling-free, interrupt-driven peripheral
driver** operating over I2C. It is structured as two cooperating modules:

- **Protocol module** (`bma423.c`): stateless, synchronous register
  sequencing — init, configure, read. No knowledge of interrupts, tasks, or
  scheduling.
- **Concurrency module** (`bma423_isr.c`): owns the ISR, the event queue,
  the acquisition task, and the fault recovery ladder. Calls into the
  protocol module but is not called by it — the dependency is one-directional.

This split exists so the "what the sensor needs" logic and the "how the
system schedules access to it" logic can change independently. A port to a
bare-metal superloop with no RTOS would only require rewriting the
concurrency module.

## 9.2 Public API Surface

```c
// Protocol layer (bma423.c / bma423.h)
bma423_status_t bma423_init(void);
bma423_status_t bma423_read_accel(int16_t *x, int16_t *y, int16_t *z);

// Concurrency layer (bma423_isr.c / bma423_isr.h)
bma423_status_t bma423_isr_init(void);
```

Three functions total. This is deliberately minimal — the driver exposes
only what a caller in `main.c` needs to bring the subsystem up. Internal
helpers (ISR handler, task function) are `static` and not part of the public
surface.

## 9.3 Single-Instance Driver Design

**Design Decision**

The driver was intentionally designed to support a single BMA423 sensor
instance. The smartwatch hardware contains only one BMA423 accelerometer
connected to a dedicated I²C bus. Supporting multiple sensor instances would
have introduced complexity with no functional benefit for the target
platform.

**Implementation**

The driver maintains no device handles or dynamically allocated driver
objects. Configuration parameters — I²C address, register definitions — are
fixed at compile time (`bma423_regs.h`). All driver APIs operate directly on
the single sensor instance:

```c
bma423_init();
bma423_read_accel(&x, &y, &z);
```

rather than:

```c
bma423_init(&sensor);
bma423_read_accel(&sensor, &x, &y, &z);
```

**Advantages**

- Simpler API surface
- Smaller RAM footprint — no driver context struct to allocate or manage
- No dynamic memory allocation
- Lower implementation complexity
- Matches the hardware architecture: one accelerometer, one driver instance

**Trade-offs**

The current implementation cannot manage multiple BMA423 devices
simultaneously. Supporting multiple sensors would require introducing a
driver context or device handle carrying, at minimum:

- I²C device address
- Bus instance
- Runtime configuration
- Driver state (e.g. last-known-good config, retry counters)

**Future Extension**

If future hardware required multiple accelerometers, the driver could be
refactored into a context-based architecture:

```c
typedef struct {
    uint8_t i2c_addr;
    // bus instance, runtime config, state, etc.
} bma423_t;

bma423_init(&sensor);
bma423_read_accel(&sensor, &x, &y, &z);
```

This would preserve the existing register-sequencing logic while extending
the design to support multiple instances — the protocol layer's internal
logic doesn't change, only how it's parameterized and invoked.

The reasoning here is not "handles were too complicated to implement." It's
that the hardware has exactly one accelerometer, and a single-instance
design minimizes code size and complexity while matching the target system.
The handle-based path is identified as a future extension, not avoided out
of difficulty.

## 9.4 Error Code Design

```c
typedef enum {
    BMA423_OK,
    BMA423_ERR_BUS,
    BMA423_ERR_CHIP_ID,
    BMA423_ERR_FATAL,
    BMA423_ERR_CMD,
    BMA423_ERR_CONFIG,
    BMA423_ERR_INVALID_ARG
} bma423_status_t;
