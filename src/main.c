#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c.h"
#include "power.h"
#include "bma423.h"

void app_main(void)
{
    /* Initialize I2C bus */

    if (i2c_init() != I2C_OK)
    {
        printf("[MAIN] I2C init failed\n");
        return;
    }

    /* Initialize power subsystem */

    if (power_init() != ESP_OK)
    {
        printf("[MAIN] Power init failed\n");
        return;
    }

    /* Scan I2C bus (debug only) */

    i2c_scan();

    /* Initialize BMA423 */

    printf("[MAIN] Calling bma423_init()\n");

    bma423_status_t result = bma423_init();

    switch (result)
    {
        case BMA423_OK:
            printf("[MAIN] BMA423 init successful\n");
            break;

        case BMA423_ERR_CHIP_ID:
            printf("[MAIN] BMA423 CHIP_ID verification failed\n");
            break;

        case BMA423_ERR_BUS:
            printf("[MAIN] BMA423 I2C communication failed\n");
            break;

        case BMA423_ERR_CONFIG:
            printf("[MAIN] BMA423 configuration failed\n");
            break;

        default:
            printf("[MAIN] Unknown BMA423 error: %d\n", result);
            break;
    }
}