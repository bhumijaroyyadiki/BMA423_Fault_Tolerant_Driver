#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c.h"
#include "power.h"
#include "bma423.h"
#include "bma423_isr.h"

void app_main(void)
{
    if (i2c_init() != I2C_OK) { return; }
    if (power_init() != ESP_OK) { return; }

    i2c_scan();

    /* 1. Initialize BMA423 Hardware Configuration First */
    printf("[MAIN] Calling bma423_init()\n");
    bma423_status_t result = bma423_init();

    if (result != BMA423_OK) {
        printf("[MAIN] BMA423 hardware init failed: %d\n", result);
        return;
    }
   
    /* 2. Now that hardware is ready, safe to enable ESP32 ISR listening pipeline */
    printf("[MAIN] Calling bma423_isr_init()\n");
    if (bma423_isr_init() != BMA423_OK) {
        printf("[MAIN] BMA423 ISR subsystem initialization failed\n");
        return;
    }

    printf("[MAIN] Driver pipeline fully operational!\n");

   
}

