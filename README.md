# Fault-Tolerant Interrupt-Driven BMA423 Accelerometer Driver

Register-level I2C driver for the Bosch BMA423 accelerometer on ESP32 — built to survive real hardware failure, not just report clean data on a bench.

## Why this exists

Most sensor driver code assumes the bus and the sensor will always behave. Real systems on a shared I2C bus see NACKs, bus contention, sensor resets, and interrupts that fire before data is actually ready. This driver was built around those failure modes, not around the happy path.

## What it does

- **Portable sensor logic** — the BMA423 register-level driver (bma423.c) has zero dependency on ESP-IDF types or calls; it only calls through the I2C transport interface (i2c_read/i2c_write) and a platform delay macro. Porting to a different MCU means rewriting the transport layer (i2c.c) and the interrupt/task setup (bma423_isr.c) — the sensor protocol and recovery logic itself is unchanged.
- **Fully event-driven acquisition** — zero polling. A GPIO interrupt on the sensor's data-ready line wakes a FreeRTOS task; the ISR itself does no I2C work.
- **Write-verified configuration** — every register write is read back and checked. An I2C ACK only confirms the byte was accepted on the wire, not that the device's internal register logic applied it.
- **Three-tier bounded fault recovery** — retry → sensor re-initialization → controlled shutdown. No unbounded retries, no silent failures.
- **No vendor sensor SDK** — every register, bit field, and timing constraint is implemented directly against the Bosch datasheet. The I2C transport layer uses ESP-IDF's driver API; the sensor protocol layer does not.


## Hardware

| Component | Part | Interface |
|---|---|---|
| MCU | ESP32 | — |
| Accelerometer | Bosch BMA423 | I2C, addr `0x19` |
| PMIC | AXP202 | I2C, addr `0x35` |

## Documentation

The documentation includes architecture decisions, rejected alternatives, debugging methodology, testing strategy, and known limitations to provide a transparent engineering record.
Start here: [`docs/01_Project_Overview.md`](docs/01_Project_Overview.md)

Notably:
- [`docs/23_Design_Decisions.md`](docs/23_Design_Decisions.md) — every major decision with its rejected alternative and why
- [`docs/26_Implementation_Walkthrough.md`](docs/26_Implementation_Walkthrough.md) — how this was actually built, in order
- [`docs/29_Debugging_Journey.md`](docs/29_Debugging_Journey.md) — a real hypothesis-test-result account of root-causing an intermittent I2C driver failure
- [`docs/34_Limitations.md`](docs/34_Limitations.md) — what this driver honestly does not do yet

## Status

Phase 1 complete: robust driver core with interrupt-driven acquisition and tiered fault recovery, validated under injected I2C faults.

FIFO support and power-mode transitions are scoped as deliberate future work — see [`docs/35_Future_Improvements.md`](docs/35_Future_Improvements.md).

## Design Philosophy

This driver prioritizes predictable behavior over maximum throughput.

The implementation favors explicit state transitions, bounded recovery attempts, and configuration verification instead of optimistic assumptions about hardware behavior.

## License

MIT — see [LICENSE](LICENSE).
