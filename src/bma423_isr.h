/**
 * @file bma423_isr.h
 * @brief Interrupt-driven subsystem driver for the Bosch BMA423 accelerometer.
 * Handles GPIO configuration, ISR registration, and deferred task processing.
 */

#ifndef BMA423_ISR_H
#define BMA423_ISR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bma423.h" // Needed for bma423_status_t definitions

/**
 * @brief Initializes the complete BMA423 interrupt processing pipeline.
 * * @note This function follows a strict dependency-first execution order:
 * 1. Allocates the signaling queue.
 * 2. Configures the physical input GPIO (Pin 39).
 * 3. Installs the global ESP-IDF GPIO ISR service (tolerating pre-existing states).
 * 4. Hooks the custom ISR handler to the pin.
 * 5. Spawns the deferred FreeRTOS worker task.
 * * @return BMA423_OK on total successful pipeline allocation.
 * BMA423_ERR_CONFIG if any OS resource allocation or hardware configuration fails.
 */
bma423_status_t bma423_isr_init(void);

#ifdef __cplusplus
}
#endif

#endif // BMA423_ISR_H