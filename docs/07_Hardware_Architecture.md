# 07 — Hardware Architecture

This section describes the physical/electrical layer this firmware sits on
top of — separate from Section 6, which described logical bus topology and
task concurrency. Details here are constrained to what the code and register
configuration imply; anything not directly evidenced (e.g. exact pull-up
resistor values, PCB trace lengths) is stated as an assumption, not a fact.

## 7.1 MCU

- **ESP32** (Xtensa dual-core), FreeRTOS-based firmware.
- I2C peripheral: `I2C_NUM_0`, configured as master, 400 kHz (Fast Mode).
- GPIO39 used as the BMA423 interrupt input. On the ESP32, GPIO34–39 are
  **input-only** pins with no internal pull-up/pull-down capability — this is
  why `bma423_isr_init()` explicitly disables both pull-up and pull-down in
  the `gpio_config_t` struct rather than relying on an internal pull. The
  BMA423's INT1 is configured push-pull, active-high (`INT1_PUSH_PULL`,
  `INT1_ACTIVE_HIGH` in `bma423_regs.h`), so no external or internal pull
  resistor is required on that line — the sensor drives both logic levels.

## 7.2 I2C Bus

- **SDA**: GPIO21, **SCL**: GPIO22 — the ESP32's default/most common I2C pin
  assignment on TTGO T-Watch boards.
- **Speed**: 400 kHz (Fast Mode), configured in `i2c_init()`. Fast Mode is
  the appropriate choice here: the BMA423 datasheet supports up to 400 kHz/1
  MHz depending on variant, and 400 kHz keeps burst-read latency for the
  6-byte XYZ transaction low without stressing bus integrity on a
  multi-drop bus with several devices.
- **Pull-ups**: enabled via `sda_pullup_en` / `scl_pullup_en` in the ESP-IDF
  config. On most ESP32 dev boards these are internal weak pull-ups; TTGO
  T-Watch hardware may also have onboard pull-ups on the shared bus. This
  firmware does not assume a specific resistor value — it assumes only that
  *some* pull-up is present and enables the ESP32's internal one as a
  safety net.
- **Multi-drop bus**: four addressable devices share this bus (AXP202, BMA423,
  PCF8563, FT6236). This is the electrical justification for the mutex in
  Section 6.3 — bus contention is a hardware reality here, not a theoretical
  concern.

## 7.3 Power Architecture

- **AXP202 PMIC** is the sole power management IC. It supplies `DC3` to the
  ESP32 itself — this is why `power.c` treats disabling `DC3` as a
  category of error the firmware must never attempt (FR-5.2 / NFR
  discussed in Section 20).
- **BMA423 supply rail**: on the always-on rail (tied to `DC3` per the
  project notes), meaning the sensor loses power only if the ESP32 itself
  loses power. This is *why* `power_init()` contains no explicit
  "enable BMA423 power" step — there is nothing to switch.
- **LDO2 / LDO3**: backlight and audio/backplane rails respectively. Not
  required for the accelerometer driver's function; implemented in
  `power.c` as forward-looking primitives for when display/audio subsystems
  are integrated (explicitly scoped as "stub for future phases" in code
  comments).

## 7.4 Interrupt Line

- **BMA423 INT1 → ESP32 GPIO39**, positive-edge triggered
  (`GPIO_INTR_POSEDGE`).
- Configured as **push-pull, active-high, edge-triggered** at the sensor
  side (`INT1_IO_CTRL` register) — matched deliberately to the
  edge-triggered GPIO interrupt type on the ESP32 side. A mismatch here
  (e.g. sensor configured level-triggered, MCU configured edge-triggered)
  is a classic source of missed or duplicate interrupts; this is called out
  explicitly because it's a real class of bug this design avoids by
  construction, not by luck.

## 7.5 Physical Constraints Assumed But Not Independently Verified

Stated honestly rather than glossed over:

- Bus trace length and capacitance on the TTGO T-Watch PCB are assumed
  within spec for 400 kHz operation — not independently measured with an
  oscilloscope in this project.
- Actual pull-up resistor values on SDA/SCL are unknown (vendor board
  design, not user-controlled) — the firmware compensates by also enabling
  ESP32-internal pull-ups as a safety margin, not as a substitute for
  correct board-level design.
- INT1 signal integrity (rise/fall time under real bus loading) was not
  measured with a scope; correctness of edge detection was inferred from
  observed correct/incorrect read behavior in software (see Section 29,
  Debugging Journey), which is a weaker validation method than direct
  electrical measurement and is acknowledged as such.
