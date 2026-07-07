/**
 * bma423_isr.c
 *
 * Interrupt-driven acquisition pipeline for the BMA423 accelerometer.
 *
 * Architecture:
 *   BMA423 INT1 (GPIO39, rising edge)
 *        -> ISR (signal-only, no I2C work)
 *        -> FreeRTOS queue
 *        -> bma423_task (blocks on queue, performs the actual read)
 *
 * On read failure, a bounded three-tier recovery ladder runs:
 *   Tier 1: retry the read directly (assumes a transient bus glitch)
 *   Tier 2: re-initialize the sensor, then verify with a read
 *           (assumes sensor-side state was disturbed)
 *   Tier 3: give up and suspend the task in a controlled way
 *           (assumes the fault is not transient/recoverable)
 *
 * Each tier is strictly bounded — no unbounded retry anywhere in this
 * pipeline. See docs/21_Error_Handling_Strategy.md and
 * docs/23_Design_Decisions.md for why this specific structure was chosen
 * over the alternatives that were considered.
 */

#include "bma423_isr.h"
#include "bma423.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>

#define BMA423_INT1_GPIO     39
#define ACCEL_QUEUE_DEPTH    10

#define MAX_RETRY_COUNT      3
#define RETRY_DELAY_MS       10

#define MAX_REINIT_COUNT     3
#define REINIT_DELAY_MS      50

static QueueHandle_t accel_event_queue = NULL;
static TaskHandle_t  accel_task_handle = NULL;

/* ---------------------------------------------------------------------
 * ISR — signal only, no I2C work.
 *
 * i2c_read()/i2c_write() acquire a FreeRTOS mutex internally. Mutex
 * acquisition from ISR context is illegal in FreeRTOS and will corrupt
 * scheduler state. The ISR therefore does the absolute minimum: post
 * one byte to the event queue and yield if a higher-priority task was
 * woken. The actual sensor read happens later, in task context.
 * -------------------------------------------------------------------- */
void IRAM_ATTR bma423_isr_handler(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    uint8_t event = 1; /* payload is a signal, not data */

    xQueueSendFromISR(accel_event_queue, &event, &higher_priority_task_woken);

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/* ---------------------------------------------------------------------
 * Tier 1 — bounded retry of the read itself.
 *
 * Assumes the failure was a transient bus event (e.g. a NACK caused by
 * bus contention with the AXP202 driver) and that the sensor's internal
 * configuration state is still valid. This is the cheapest recovery
 * action available, so it's tried first.
 * -------------------------------------------------------------------- */
static bma423_status_t recover_tier1_retry(int16_t *x, int16_t *y, int16_t *z)
{
    bma423_status_t status = BMA423_ERR_BUS;

    for (int attempt = 1; attempt <= MAX_RETRY_COUNT; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        status = bma423_read_accel(x, y, z);

        if (status == BMA423_OK) {
            printf("[RECOVERY L1] Retry %d succeeded\n", attempt);
            return BMA423_OK;
        }
        printf("[RECOVERY L1] Retry %d failed\n", attempt);
    }

    printf("[RECOVERY L1] All retries exhausted\n");
    return status;
}

/* ---------------------------------------------------------------------
 * Tier 2 — sensor re-initialization.
 *
 * Reached only after Tier 1 is exhausted. Assumes the sensor's internal
 * configuration state was disturbed (e.g. by a brown-out or bus glitch)
 * rather than assuming the bus itself is permanently broken. Re-runs the
 * full init sequence and requires an immediate, successful verification
 * read before declaring recovery successful — a re-init that reports
 * success but still can't produce data is not a real recovery.
 * -------------------------------------------------------------------- */
static bma423_status_t recover_tier2_reinit(int16_t *x, int16_t *y, int16_t *z)
{
    printf("[RECOVERY L2] Read retries exhausted\n");
    printf("[RECOVERY L2] Attempting sensor reinitialization...\n");

    for (int attempt = 1; attempt <= MAX_REINIT_COUNT; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(REINIT_DELAY_MS));

        bma423_status_t init_status = bma423_init();
        if (init_status != BMA423_OK) {
            printf("[RECOVERY L2] Re-init %d/%d failed\n", attempt, MAX_REINIT_COUNT);
            continue;
        }

        printf("[RECOVERY L2] Re-init %d/%d succeeded\n", attempt, MAX_REINIT_COUNT);

        /* Re-init reporting success is not sufficient on its own —
         * verify the sensor can actually produce data before trusting
         * this as a real recovery. */
        bma423_status_t verify_status = bma423_read_accel(x, y, z);
        if (verify_status == BMA423_OK) {
            printf("[RECOVERY L2] Sensor communication restored\n");
            return BMA423_OK;
        }

        printf("[RECOVERY L2] Verification read failed\n");
    }

    return BMA423_ERR_BUS;
}

/* ---------------------------------------------------------------------
 * Tier 3 — controlled shutdown.
 *
 * Reached only after both Tier 1 and Tier 2 are exhausted. At this point
 * the fault is treated as non-transient, and further automated attempts
 * are assumed unproductive. The task suspends itself rather than
 * looping forever or leaving the system in an undefined state — the
 * rest of the firmware keeps running; only accelerometer acquisition
 * stops. This is a terminal state by design: there is no automatic path
 * back to normal operation without external intervention.
 * -------------------------------------------------------------------- */
static void recover_tier3_suspend(void)
{
    printf("\n");
    printf("=====================================\n");
    printf("[CRITICAL] BMA423 recovery failed\n");
    printf("[CRITICAL] Sensor subsystem offline\n");
    printf("[CRITICAL] Suspending BMA423 task\n");
    printf("=====================================\n");

    vTaskSuspend(NULL);
}

/* ---------------------------------------------------------------------
 * Deferred processing task.
 *
 * Blocks on the event queue (woken only by the ISR) and performs the
 * actual accelerometer read. On failure, escalates through the recovery
 * ladder above, one tier at a time. Recovery *policy* lives here, not in
 * bma423_read_accel() — that function only reports success/failure, it
 * doesn't decide what to do about a failure.
 * -------------------------------------------------------------------- */
static void bma423_task(void *pvArg)
{
    uint8_t event;
    int16_t x, y, z;

    while (1) {
        if (xQueueReceive(accel_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bma423_status_t status = bma423_read_accel(&x, &y, &z);

        if (status != BMA423_OK) {
            status = recover_tier1_retry(&x, &y, &z);
        }

        if (status != BMA423_OK) {
            status = recover_tier2_reinit(&x, &y, &z);
        }

        if (status != BMA423_OK) {
            recover_tier3_suspend();
            /* Task is suspended above; this point is only reached again
             * if something external explicitly resumes the task. */
            continue;
        }

        printf("X=%d Y=%d Z=%d\n", x, y, z);
    }
}

/* ---------------------------------------------------------------------
 * Initialization pipeline.
 *
 * Resources are allocated in strict order — queue, then GPIO, then ISR
 * service, then ISR handler, then task — and any failure unwinds every
 * resource allocated so far before returning. This specifically avoids
 * leaving a GPIO interrupt armed with a handler pointing at a task that
 * was never successfully created, which would be a dangling-ISR state
 * capable of crashing the system on the next interrupt.
 *
 * The task is created LAST, deliberately: the caller (main.c) is
 * required to have already confirmed bma423_init() fully succeeded
 * before calling this function, so no interrupt can ever fire against
 * an unconfigured or partially-configured sensor.
 * -------------------------------------------------------------------- */
bma423_status_t bma423_isr_init(void)
{
    accel_event_queue = xQueueCreate(ACCEL_QUEUE_DEPTH, sizeof(uint8_t));
    if (accel_event_queue == NULL) {
        return BMA423_ERR_CONFIG;
    }

    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_POSEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BMA423_INT1_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    /* ESP_ERR_INVALID_STATE means the global ISR service is already
     * installed by something else in the system — that's fine, not a
     * failure condition for this driver. */
    esp_err_t svc_err = gpio_install_isr_service(0);
    if (svc_err != ESP_OK && svc_err != ESP_ERR_INVALID_STATE) {
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    esp_err_t isr_err = gpio_isr_handler_add(BMA423_INT1_GPIO, bma423_isr_handler, NULL);
    if (isr_err != ESP_OK) {
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    BaseType_t task_err = xTaskCreate(bma423_task, "bma423_task", 2048, NULL, 5, &accel_task_handle);
    if (task_err != pdPASS) {
        /* Unwind the ISR hook before returning — otherwise a stale
         * handler is left pointing at a task that doesn't exist. */
        gpio_isr_handler_remove(BMA423_INT1_GPIO);
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    return BMA423_OK;
}