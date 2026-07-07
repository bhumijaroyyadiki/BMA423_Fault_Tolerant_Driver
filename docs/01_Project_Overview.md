# 01 — Project Overview

## 1.1 Summary

This project implements a fault-tolerant, interrupt-driven accelerometer driver
for the Bosch BMA423 6-axis accelerometer, running on an ESP32-based smartwatch
platform (TTGO T-Watch family: ESP32 + AXP202 PMIC + BMA423 + PCF8563 RTC +
FT6236 touch controller on a shared I2C bus).

The deliverable is not "read some XYZ data off a sensor." The deliverable is a
driver subsystem that assumes the I2C bus, the sensor, and the interrupt line
will all fail at some point during normal operation — and defines exactly what
happens when they do.

## 1.2 What This Project Actually Is

- A register-level BMA423 driver written directly against the Bosch datasheet
  register map (no vendor BMA423 SDK, no BSEC, no Bosch sensor API dependency).
- An event-driven acquisition pipeline: a GPIO interrupt on the sensor's
  data-ready line wakes a FreeRTOS task via a queue. There is no polling loop
  anywhere in the read path.
- A three-tier fault recovery ladder for I2C/sensor failures (retry → sensor
  re-init → controlled subsystem suspension), implemented and exercised, not
  theoretical.
- A shared-bus I2C transport layer (mutex-protected, ESP-IDF `driver/i2c.h`
  backed) that also talks to the AXP202 PMIC on the same physical bus.

## 1.3 What This Project Is Not

Stated explicitly, because scope discipline is itself an engineering decision:

- Not a full bare-metal I2C peripheral driver — the bus transaction layer uses
  the ESP-IDF I2C master driver. The BMA423 *protocol* layer above it is fully
  custom.
- Not sensor-fusion firmware. Single sensor, no gyroscope/magnetometer fusion.
- Not a UI or product demo. Display, step-counting logic, and watch-face
  rendering are scaffolding to give the driver something to feed — they are
  not the subject of this documentation.
- Not power-optimized yet. AXP202 rail control exists (`power.c`), but
  MCU/sensor low-power state transitions (BMA423 suspend mode, ESP32 light/deep
  sleep) are **not implemented in this phase** — see Section 34 (Limitations)
  and Section 35 (Future Improvements).
- Not FIFO-based. Single-sample burst reads only; FIFO batch acquisition is
  deferred, not attempted-and-abandoned.

## 1.4 Why This Scope

The project is explicitly scoped around *one* skill: building a driver that
survives real hardware failure modes on a shared I2C bus. Everything else
(display, PMIC rail sequencing, RTC, touch) exists only far enough to prove the
driver operates correctly in a system context, not in isolation on a breakout
board. Expanding scope to power modes or FIFO before the fault-handling core
was solid would have diluted the one thing this project is meant to
demonstrate.

## 1.5 Target Platform

| Component | Part | Interface | Role in this project |
|---|---|---|---|
| MCU | ESP32 | — | Host controller, FreeRTOS |
| Accelerometer | Bosch BMA423 | I2C, addr `0x19` | Primary subject of this driver |
| PMIC | AXP202 | I2C, addr `0x35` | Powers ESP32 (DC3) and peripheral rails |
| RTC (bus peer, not driven) | PCF8563 | I2C, addr `0x51` | Present on bus, referenced in `i2c_scan()` |
| Touch (bus peer, not driven) | FT6236 | I2C, addr `0x38` | Present on bus, referenced in `i2c_scan()` |