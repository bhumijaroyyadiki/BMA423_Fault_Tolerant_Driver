# 32_Performance_Analysis.md

This section documents the timing and resource characteristics of the driver — not theoretical calculations alone, but what was measured or can be derived with confidence from known parameters. Where direct measurement was not performed, the method for measuring is described so it can be done.

---

## 32.1 I²C Transaction Timing

All I²C communication runs at 400 kHz (Fast Mode). At this clock rate, one bit takes 2.5 µs. The time for a complete transaction depends on the number of bytes transferred.

**Single byte write (e.g. configuration register):**

```
START condition          →  ~1 bit time
Device address + W bit  →  9 bit times (8 data + 1 ACK)
Register address        →  9 bit times
Data byte               →  9 bit times
STOP condition          →  ~1 bit time
Total                   →  ~29 bit times × 2.5 µs = ~72 µs
```

**Single byte read (chip ID, status, configuration verify):**

```
START                   →  ~1 bit time
Device address + W      →  9 bit times
Register address        →  9 bit times
Repeated START          →  ~1 bit time
Device address + R      →  9 bit times
Data byte + NACK        →  9 bit times
STOP                    →  ~1 bit time
Total                   →  ~39 bit times × 2.5 µs = ~97 µs
```

**Six-byte burst read (acceleration data):**

```
START                   →  ~1 bit time
Device address + W      →  9 bit times
Register address        →  9 bit times
Repeated START          →  ~1 bit time
Device address + R      →  9 bit times
5 bytes + ACK each      →  45 bit times
1 byte + NACK           →  9 bit times
STOP                    →  ~1 bit time
Total                   →  ~84 bit times × 2.5 µs = ~210 µs
```

---

## 32.2 Initialization Timing

`bma423_init()` performs the following I²C transactions in sequence:

| Step | Transaction type | Approximate time |
|---|---|---|
| Soft reset write | Single byte write | ~72 µs |
| Boot delay | `vTaskDelay(50ms)` | 50 ms |
| Dummy read | Single byte read | ~97 µs |
| Chip ID read | Single byte read | ~97 µs |
| ERR_REG read | Single byte read | ~97 µs |
| ACC_CONF write | Single byte write | ~72 µs |
| ACC_CONF verify | Single byte read | ~97 µs |
| Inter-write delay | `esp_rom_delay_us(1000)` | 1 ms |
| ACC_RANGE write | Single byte write | ~72 µs |
| ACC_RANGE verify | Single byte read | ~97 µs |
| Inter-write delay | `esp_rom_delay_us(1000)` | 1 ms |
| INT1_IO_CTRL write | Single byte write | ~72 µs |
| INT1_IO_CTRL verify | Single byte read | ~97 µs |
| Inter-write delay | `esp_rom_delay_us(1000)` | 1 ms |
| INT_MAP write | Single byte write | ~72 µs |
| INT_MAP verify | Single byte read | ~97 µs |
| Inter-write delay | `esp_rom_delay_us(1000)` | 1 ms |
| PWR_CONF write | Single byte write | ~72 µs |
| Inter-write delay | `esp_rom_delay_us(1000)` | 1 ms |
| PWR_CTRL write | Single byte write | ~72 µs |
| PWR_CTRL verify | Single byte read | ~97 µs |

**Total I²C transaction time:** approximately 1.8 ms
**Total delay time:** 50 ms boot + 5 × 1 ms inter-write = 55 ms
**Total initialization time:** approximately **57 ms**

The initialization is dominated entirely by the 50 ms post-reset boot delay. The I²C transactions themselves are negligible. This is expected — the boot delay is a hardware constraint, not a software cost.

---

## 32.3 Per-Sample Read Latency

At 25 Hz, one sample is produced every 40 ms. The path from interrupt assertion to processed data involves:

```
INT1 pin asserts (BMA423 drives high)
→ ESP32 GPIO interrupt fires (~1 µs latency from edge to ISR entry)
→ ISR executes: xQueueSendFromISR + portYIELD (~2–5 µs)
→ FreeRTOS scheduler context switch to bma423_task (~5–10 µs)
→ 100 µs settling delay (busy-wait)
→ Six-byte burst I²C read (~210 µs)
→ Sign extension calculation (~1 µs)
→ printf output (~100–500 µs depending on UART buffer state)
```

**Total latency from interrupt to processed data:** approximately **320–830 µs**

The dominant costs are the 100 µs settling delay and the I²C burst read. Both are fixed and predictable. The FreeRTOS scheduling jitter is small relative to the 40 ms sample window — the task processes each sample well within the next sample period.

**CPU time consumed per sample:**

```
Active processing (ISR + scheduling + delay + I²C + calculation): ~320 µs
Sample period: 40,000 µs
CPU utilization from BMA423 driver: ~0.8%
```

The driver consumes less than 1% of CPU time at 25 Hz. The remaining 99%+ is available for other tasks or idle (which on ESP32 maps to light sleep, reducing power consumption).

---

## 32.4 Recovery Timing

**Level 1 — single retry cycle:**

```
Failure detected → vTaskDelay(10ms) → retry read (~210 µs)
Total: ~10.2 ms per retry
Maximum Level 1 time (3 retries, all fail): ~30.6 ms
Typical Level 1 time (retry 1 succeeds): ~10.2 ms
```

During Level 1 retry, the task is blocked in `vTaskDelay` — not consuming CPU. Other tasks run normally. The I²C bus is available to AXP202 and PCF8563 during the delay.

**Level 2 — sensor re-initialization:**

```
vTaskDelay(50ms) + bma423_init() (~57ms) + verify read (~210µs)
Total per attempt: ~107 ms
Maximum Level 2 time (3 attempts, all fail): ~321 ms
```

During Level 2, the sensor is soft-reset and reconfigured. The I²C bus is active during `bma423_init()` but released between steps. AXP202 and PCF8563 can interleave transactions during the inter-write delays.

**Level 3 — task suspension:**

Immediate. `vTaskSuspend(NULL)` executes in one scheduler tick. No further CPU time is consumed by the BMA423 task after this point.

---

## 32.5 Memory Usage

**Stack:**

`bma423_task` was created with a stack size of 2048 words (8192 bytes on ESP32, where a word is 4 bytes). The actual stack usage depends on the deepest call chain during execution:

```
bma423_task stack frame
→ bma423_read_accel stack frame
   → i2c_read stack frame
      → i2c_cmd_handle operations (heap, not stack)
```

Estimated actual stack usage: 512–1024 bytes. The 2048 word allocation provides approximately 4–8× headroom. Stack usage was not directly measured with `uxTaskGetStackHighWaterMark()` — this is documented as a gap and recommended as a future measurement.

**Heap:**

| Resource | Allocation | Size |
|---|---|---|
| FreeRTOS queue (depth 5, uint8_t) | Heap | ~80 bytes |
| FreeRTOS mutex | Heap | ~88 bytes |
| bma423_task stack | Heap | 8192 bytes |
| I²C command links | Heap, transient | ~200 bytes per transaction, freed immediately |

Total persistent heap allocation from this driver: approximately **8360 bytes**.

The ESP32 has 520 KB of SRAM with approximately 300 KB available for heap after ESP-IDF overhead. The driver's 8360 bytes represents approximately 2.8% of available heap.

---

## 32.6 Interrupt Frequency and Timing Budget

At 25 Hz ODR:

```
Interrupt rate:          25 per second
ISR execution time:      ~3–5 µs
ISR CPU overhead:        25 × 5 µs = 125 µs/second = 0.0125% CPU
Task wake frequency:     25 per second
Task processing time:    ~320 µs per wake (excluding printf)
Task CPU overhead:       25 × 320 µs = 8 ms/second = 0.5% CPU
```

Total driver CPU overhead at 25 Hz: approximately **0.5% CPU**.

This leaves 99.5% of CPU time for other tasks — display rendering, touch handling, power management, RTC updates, and idle sleep. The driver is not a CPU bottleneck at this sample rate.

**At higher ODRs:**

| ODR | Interrupts/sec | Task CPU overhead | Notes |
|---|---|---|---|
| 25 Hz | 25 | ~0.5% | Current configuration |
| 50 Hz | 50 | ~1.0% | Tested — two failures per ESP-IDF glitch window |
| 100 Hz | 100 | ~2.0% | Untested — would require queue depth review |
| 200 Hz | 200 | ~4.0% | Approaching meaningful CPU cost |

The I²C bus becomes the bottleneck before CPU does — at 200 Hz, a 210 µs burst read every 5 ms means I²C is active 4.2% of the time for BMA423 reads alone, plus contention with AXP202 and PCF8563 transactions.

---

## 32.7 How to Measure What Was Not Measured

Two measurements were not performed during this project and are recommended for any production deployment:

**Actual stack high-water mark:**

```c
// Add after system is running for several seconds
UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(accel_task_handle);
printf("[PERF] BMA423 task stack remaining: %u words\n", stack_remaining);
```

If `stack_remaining` is below 64 words, increase the stack allocation. If it is above 512 words, the allocation can be safely reduced.

**Actual ISR-to-data latency:**

Toggle a spare GPIO pin at ISR entry and at data reconstruction completion. Measure the pulse width with an oscilloscope or logic analyzer. This gives a hardware-verified latency figure rather than the calculated estimate in Section 32.3.

```c
// In ISR:
gpio_set_level(DEBUG_PIN, 1);
xQueueSendFromISR(...);
// In bma423_task after sign extension:
gpio_set_level(DEBUG_PIN, 0);
```

The pulse width on `DEBUG_PIN` is the true end-to-end latency.
