# 19 — Memory Considerations

## 19.1 Static / Compile-Time Memory

| Item | Size | Source |
|---|---|---|
| `bma423_regs.h` constants | 0 bytes (all `#define`, no `const` storage) | Preprocessor-level, no runtime footprint |
| `i2c_mutex` (FreeRTOS mutex handle) | Handle is a pointer (4 bytes); underlying mutex object allocated from FreeRTOS heap at `i2c_init()` time | `static SemaphoreHandle_t i2c_mutex` in `i2c.c` |
| `accel_event_queue` (FreeRTOS queue handle) | Handle is a pointer (4 bytes); underlying queue storage = `ACCEL_QUEUE_DEPTH (10) × sizeof(uint8_t)` = 10 bytes, plus FreeRTOS queue control block overhead | `static QueueHandle_t accel_event_queue` in `bma423_isr.c` |
| `accel_task_handle` | Pointer (4 bytes) | `static TaskHandle_t accel_task_handle` in `bma423_isr.c` |

## 19.2 Task Stack

| Item | Size | Source |
|---|---|---|
| `bma423_task` stack | 2048 (bytes, on ESP-IDF — unlike vanilla FreeRTOS where `usStackDepth` is in words, ESP-IDF's `xTaskCreate` takes stack size in **bytes**) | `xTaskCreate(bma423_task, "bma423_task", 2048, NULL, 5, &accel_task_handle)` |

**Not measured, flagged rather than estimated:** actual stack high-water
mark (how much of the 2048-byte allocation is actually used at runtime)
was not captured via `uxTaskGetStackHighWaterMark()` in this project. The
2048-byte figure is the *allocated* size, not a verified *sufficient or
tightly-sized* figure — the local variables inside `bma423_task` and
`bma423_read_accel` (a handful of `int16_t`, `uint8_t buf[6]`, loop
counters) are small, so 2048 bytes is very likely generous headroom rather
than a tight fit, but "very likely" is not the same as measured. This is
worth actually checking with `uxTaskGetStackHighWaterMark()` before
calling the sizing final — noted in Section 34/36 as a validation gap
rather than presented as already confirmed.

## 19.3 Heap Usage

- **No `malloc`/`free` calls anywhere in the driver code itself**
  (`bma423.c`, `bma423_isr.c`, `i2c.c`, `power.c`). All buffers are
  stack-allocated (`uint8_t buf[6]`, local `uint8_t` register value
  variables).
- **FreeRTOS primitives do allocate from the heap internally**, but only
  once, at init time, not on any recurring/steady-state code path:
  - `xSemaphoreCreateMutex()` (once, in `i2c_init()`)
  - `xQueueCreate()` (once, in `bma423_isr_init()`)
  - `xTaskCreate()` (once, allocates the task's TCB + stack, in
    `bma423_isr_init()`)
- **No dynamic allocation occurs in the steady-state read path**
  (`bma423_isr_handler` → `bma423_task` → `bma423_read_accel`). This
  matters for a real-time embedded system: heap fragmentation and
  allocation-time jitter are eliminated from the hot path by construction,
  not by careful avoidance at each call site — there's simply nothing to
  allocate once init completes.

## 19.4 No Dynamic Sensor Data Buffering

There is no FIFO buffer, no circular buffer, no ring buffer of historical
samples anywhere in this codebase — each read produces exactly one X/Y/Z
sample set, consumed immediately (currently just `printf`'d) and then
discarded. This is consistent with the project's explicit scope decision
to defer FIFO support (Section 1.3, Section 3.3) — there is no
intermediate memory structure to analyze here because none exists yet.

## 19.5 Global/Static State Summary (Relevant to Reentrancy and Memory Safety)

| Variable | Scope | Reentrancy consideration |
|---|---|---|
| `i2c_mutex` | file-static in `i2c.c` | Single mutex instance protects all bus access — no per-device mutex, by design (Section 9.3's single-instance rationale extends here: only one bus, one mutex needed) |
| `accel_event_queue`, `accel_task_handle` | file-static in `bma423_isr.c` | Set once at `bma423_isr_init()` and never reassigned; safe under the single-instance design, but would need to become per-instance fields in the Section 9.3 "future extension" struct if multi-sensor support were added |
| `g_inject_fault`, `g_fault_count` (Section 9, since removed) | were `extern` globals in `bma423.h` | Not reentrancy-safe in the sense that any code anywhere could flip `g_inject_fault` — acceptable for a temporary manual test hook, would not be acceptable as permanent production instrumentation without at minimum being `static` and access-controlled |

## 19.6 Memory Footprint: What This Project Does *Not* Claim

Explicitly, since precise numbers weren't gathered: this document does not
state a total RAM/flash footprint (e.g. "driver uses X KB total") because
that would require a linker map file analysis (`.map` file / `idf.py size`
output) that wasn't performed as part of this project. Stating a total
figure without having actually run that analysis would be exactly the kind
of unverified specific this documentation is trying to avoid. If you have
a `idf.py size` or `.map` output from a build, I can incorporate real
figures here instead of leaving this qualitative.
