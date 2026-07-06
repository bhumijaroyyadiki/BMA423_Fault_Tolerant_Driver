/**
 * @file bma423_platform.h
 * @brief Hardware Abstraction Layer (HAL) for the BMA423 Accelerometer Driver.
 *
 * NOTE TO FUTURE ME: WHY WE USE A COMPILER MACRO FOR MICROSECOND DELAYS
 * -------------------------------------------------------------------------
 * The BMA423 datasheet mandates a strict, hardware-enforced minimum recovery 
 * window of 1000 microseconds (1 millisecond) between consecutive I2C writes.
 *
 * DO NOT replace this with a FreeRTOS function like vTaskDelay(pdMS_TO_TICKS(1)).
 * Here is why that breaks the driver:
 * * 1. Tick Rate Dependency: If the FreeRTOS system tick rate is set to 100Hz, 
 * 1 tick equals 10 milliseconds. Sleeping for 1 tick would introduce massive, 
 * unacceptable delays during initialization.
 * 2. Scheduler Latency: vTaskDelay forces a context switch. If a higher-priority 
 * system task (like display rendering or radio communication) is running, 
 * our initialization task will be starved, blowing past the required 1ms 
 * window into dozens of milliseconds.
 *
 * We use a blocking microsecond delay macro here to force the CPU to execute 
 * a precise, architecture-specific execution loop. This ensures the ASIC inside 
 * the sensor has stabilized before the next data packet hits the bus.
 */

#ifndef BMA423_PLATFORM_H
#define BMA423_PLATFORM_H

#include "esp_rom_sys.h" // Needed for esp_rom_delay_us on ESP-IDF platforms

/**
 * @brief Platform-agnostic microsecond delay wrapper.
 * @param us The number of microseconds to busy-wait.
 */
#define BMA423_DELAY_US(us)    esp_rom_delay_us(us)

#endif // BMA423_PLATFORM_H