#include "bma423_isr.h"
#include "bma423.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>

#define BMA423_INT1_GPIO     39
#define ACCEL_QUEUE_DEPTH    10

#define MAX_RETRY_COUNT     3
#define RETRY_DELAY_MS      10

#define MAX_REINIT_COUNT    3
#define REINIT_DELAY_MS     50

static QueueHandle_t accel_event_queue = NULL;
static TaskHandle_t accel_task_handle = NULL;

// 1. ISR HANDLER
void IRAM_ATTR bma423_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t dummy_val = 1; // Optimized to 1 byte
    
    xQueueSendFromISR(accel_event_queue, &dummy_val, &xHigherPriorityTaskWoken);

    // Correct ESP32 macro with argument
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// 2. DEFERRED PROCESSING TASK
static void bma423_task(void *pvArg) {
    uint8_t dummy_val;
    int16_t x, y, z;
    
    while(1) {
        if (xQueueReceive(accel_event_queue, &dummy_val, portMAX_DELAY) == pdTRUE) {
            bma423_status_t status = bma423_read_accel(&x, &y, &z);
            if (status == BMA423_OK) {
                printf("X=%d Y=%d Z=%d\n", x, y, z);
                continue; // Skip the reinit logic if read was successful
            } 
            // Level 1: Retry up to 3 times with 10ms delay
            for (int retry_count = 0; retry_count < MAX_RETRY_COUNT; retry_count++) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            status = bma423_read_accel(&x, &y, &z);
            if (status == BMA423_OK) {
                printf("X=%d Y=%d Z=%d\n", x, y, z);
                printf("[RECOVERY L1] Retry %d succeeded\n", retry_count + 1);
                break;
            }
            printf("[RECOVERY L1] Retry %d failed\n", retry_count + 1);
        }

        if (status != BMA423_OK) {
            printf("[RECOVERY L1] All retries exhausted\n");
            // Level 2 goes here
            printf("[RECOVERY L2] Read retries exhausted\n");
            printf("[RECOVERY L2] Attempting sensor reinitialization...\n");

            for (int reinit = 0; reinit < MAX_REINIT_COUNT; reinit++)
            {
                vTaskDelay(pdMS_TO_TICKS(REINIT_DELAY_MS));

                bma423_status_t status = bma423_init();

                if (status == BMA423_OK)
                {
                    printf("[RECOVERY L2] Re-init %d/%d succeeded\n",
                        reinit + 1,
                        MAX_REINIT_COUNT);

                    /* Verify communication immediately */
                    status = bma423_read_accel(&x, &y, &z);

                    if (status == BMA423_OK)
                    {
                        printf("[RECOVERY L2] Sensor communication restored\n");
                        //return BMA423_OK;
                        printf("X=%d Y=%d Z=%d\n", x, y, z);
                        continue;

                    }

                    printf("[RECOVERY L2] Verification read failed\n");
                }
                else
                {
                    printf("[RECOVERY L2] Re-init %d/%d failed\n",
                        reinit + 1,
                        MAX_REINIT_COUNT);
                }
            }
            printf("\n");
            printf("=====================================\n");
            printf("[CRITICAL] BMA423 recovery failed\n");
            printf("[CRITICAL] Sensor subsystem offline\n");
            printf("[CRITICAL] Suspending BMA423 task\n");
            printf("=====================================\n");

            //return BMA423_ERR_BUS;
            vTaskSuspend(NULL);
        }
           
            // Level 3: If re-init fails, suspend task
            // YOUR CODE HERE
        }
    }
}

// 3. INITIALIZATION SUBSYSTEM
bma423_status_t bma423_isr_init(void) {
    // Step A: Create the FreeRTOS Queue
    accel_event_queue = xQueueCreate(ACCEL_QUEUE_DEPTH, sizeof(uint8_t));
    if (accel_event_queue == NULL) {
        return BMA423_ERR_CONFIG;
    }

    // Step B: Configure the GPIO Pin
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BMA423_INT1_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        // Queue exists but no task yet; safe to return or clean up queue here
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    // Step C: Install Global ISR Service (Handle dependency safely)
    esp_err_t svc_err = gpio_install_isr_service(0);
    if (svc_err != ESP_OK && svc_err != ESP_ERR_INVALID_STATE) {
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    // Step D: Attach the ISR Handler
    esp_err_t isr_err = gpio_isr_handler_add(BMA423_INT1_GPIO, bma423_isr_handler, NULL);
    if (isr_err != ESP_OK) {
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    // Step E: Create the Task LAST (Only when everything else is fully functional)
    BaseType_t task_err = xTaskCreate(bma423_task, "bma423_task", 2048, NULL, 5, &accel_task_handle);
    if (task_err != pdPASS) {
        // Clean up hardware hooks before exiting to prevent dangling ISR pointer
        gpio_isr_handler_remove(BMA423_INT1_GPIO);
        vQueueDelete(accel_event_queue);
        accel_event_queue = NULL;
        return BMA423_ERR_CONFIG;
    }

    return BMA423_OK;
}