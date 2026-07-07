# 02 — Problem Statement

## 2.1 Context

Modern wearables rely on always-on motion sensors — accelerometers being the
baseline sensor for step counting, gesture wake, and activity classification.
That sensor sits on a shared I2C bus alongside a PMIC, an RTC, and a touch
controller, and it is expected to produce continuous, reliable data for as
long as the watch is worn.

## 2.2 The Problem

Most hobbyist and even some production wearable firmware treats the
accelerometer as a "read register, get data" peripheral. This works on the
bench, on a clean bus, on first boot. It does not describe what a shipped
device actually has to survive:

- **I2C bus faults.** NACKs, bus-busy conditions, and transient noise on a bus
  shared with a PMIC that is actively switching power rails.
- **Sensor-side faults.** The BMA423 can enter an error state (`ERR_REG`
  non-zero), fail a configuration write silently, or simply not respond after
  a brown-out or PMIC rail glitch.
- **Interrupt-driven acquisition assumptions that don't hold in practice.** An
  edge-triggered data-ready interrupt can fire before the sensor's internal
  ADC conversion has actually latched new values into the output registers —
  read too early and you get stale or partially-updated data, not an error
  code.
- **Polling as a workaround.** The common fallback — poll the status register
  in a loop until data is ready — burns CPU cycles and defeats the purpose of
  having a data-ready interrupt line in the first place. On a battery-powered
  wearable this is a real power cost, not a style preference.

None of these are exotic failure modes. They are the normal operating
conditions of an I2C peripheral on a multi-device bus in a battery-powered
product. A driver that doesn't define behavior for them isn't incomplete in a
minor way — it's a driver that only works until the first bus glitch, and then
fails silently or hangs the read path with no defined recovery.

## 2.3 Why Existing Approaches Fall Short

| Approach | Failure mode it leaves unhandled |
|---|---|
| Vendor SDK / Bosch Sensor API wrapper | Abstracts away the exact point of failure — a NACK or bad chip ID surfaces as a generic error, with no visibility into *which* transaction failed or why |
| Polling-based read loop | Works, but wastes CPU/power continuously waiting on a status register instead of sleeping until data-ready fires |
| "Happy path" interrupt-driven read | Assumes the interrupt always means valid, complete data — breaks the moment the ISR fires early relative to internal ADC settling |
| No recovery on I2C error | A single dropped NACK on a shared bus permanently stalls acquisition until manual reset |

## 2.4 The Problem This Driver Solves

Build a BMA423 driver where:

1. Every I2C transaction result is checked, not assumed.
2. Acquisition is interrupt-driven, with zero polling in the steady-state read
   path.
3. A missed, early, or spurious interrupt does not corrupt or silently pass
   through bad data.
4. Bus and sensor failures have a defined, bounded, three-tier response
   (retry → re-init → controlled shutdown) instead of undefined behavior.
5. The failure handling is demonstrated under actual injected faults, not just
   claimed in comments.

This is the problem this documentation set exists to describe the solution
to.
