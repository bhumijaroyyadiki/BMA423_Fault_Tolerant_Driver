#include "i2c.h"
#include "driver/i2c.h"
#include "freertos/semphr.h"
#include <stdio.h>

#define I2C_MASTER_NUM                I2C_NUM_0
#define I2C_MASTER_SDA_IO             21
#define I2C_MASTER_SCL_IO             22
#define I2C_MASTER_FREQ_HZ            400000
#define I2C_TRANSACTION_TIMEOUT_MS    10 
#define I2C_MUTEX_TIMEOUT_MS          50
static SemaphoreHandle_t i2c_mutex = NULL;

i2c_status_t i2c_init(void)
{
    i2c_mutex = xSemaphoreCreateMutex();
    if (i2c_mutex == NULL) return I2C_ERR_BUS;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };

    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return I2C_ERR_BUS;

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK) return I2C_ERR_BUS;

    return I2C_OK;
}

i2c_status_t i2c_write(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        printf("[DEBUG] mutex timeout inside i2c_write\n");
        return I2C_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);

    if (len > 0 && data != NULL) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
    
    if (err != ESP_OK) {
        printf("[DEBUG] i2c_master_cmd_begin error in i2c_write: 0x%x\n", err);
    }

    i2c_cmd_link_delete(cmd);
    i2c_status_t result = (err != ESP_OK) ? I2C_ERR_BUS : I2C_OK;
    xSemaphoreGive(i2c_mutex);
    
    return result;
}

i2c_status_t i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        printf("[DEBUG] mutex timeout inside i2c_read\n");
        return I2C_ERR_TIMEOUT;
    }
    
    if (len == 0 || data == NULL) {
        xSemaphoreGive(i2c_mutex);
        return I2C_ERR_INVALID_ARG; 
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);

    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));

    if (err != ESP_OK) {
        printf("[DEBUG] i2c_master_cmd_begin error in i2c_read: 0x%x\n", err);
    }

    i2c_cmd_link_delete(cmd);
    i2c_status_t result = (err != ESP_OK) ? I2C_ERR_BUS : I2C_OK;
    xSemaphoreGive(i2c_mutex);
    
    return result;
}

/******************************************************************************
 * @brief Write one or more bytes to a device register.
 *
 * Performs a standard I2C register write transaction:
 *
 *      START
 *          ↓
 *  Device Address (Write)
 *          ↓
 *     Register Address
 *          ↓
 *       Data Byte(s)
 *          ↓
 *          STOP
 *
 * Access to the shared I2C bus is serialized using a mutex to prevent
 * concurrent transactions from multiple FreeRTOS tasks.
 *
 * @param dev_addr 7-bit I2C device address.
 * @param reg      Register address to write.
 * @param data     Pointer to transmit buffer.
 * @param len      Number of bytes to write.
 *
 * @return
 *      - I2C_OK
 *      - I2C_ERR_TIMEOUT
 *      - I2C_ERR_BUS
 ******************************************************************************/
i2c_status_t i2c_write(uint8_t dev_addr,
                       uint8_t reg,
                       uint8_t *data,
                       size_t len)
{
    /*
     * Acquire exclusive ownership of the I2C bus.
     *
     * The timeout prevents a permanently blocked task from holding the
     * bus indefinitely if another task fails to release the mutex.
     */
    if (xSemaphoreTake(i2c_mutex,
                       pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        printf("[I2C] Failed to acquire bus mutex\n");
        return I2C_ERR_TIMEOUT;
    }

    /*
     * Allocate a temporary command link used by the ESP-IDF I2C driver.
     * The command link records the transaction before it is executed by
     * the hardware controller.
     */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    if (cmd == NULL)
    {
        xSemaphoreGive(i2c_mutex);
        return I2C_ERR_BUS;
    }

    /*
     * Construct the write transaction.
     *
     * START
     * Device Address (Write)
     * Register Address
     * Data...
     * STOP
     */
    i2c_master_start(cmd);

    i2c_master_write_byte(
        cmd,
        (dev_addr << 1) | I2C_MASTER_WRITE,
        true);

    i2c_master_write_byte(
        cmd,
        reg,
        true);

    /*
     * Support both register-only writes and multi-byte register writes.
     */
    if ((data != NULL) && (len > 0))
    {
        i2c_master_write(
            cmd,
            data,
            len,
            true);
    }

    i2c_master_stop(cmd);

    /*
     * Execute the queued transaction.
     *
     * A successful I2C ACK only confirms that the target device accepted
     * the transmitted bytes on the bus. It does not guarantee that the
     * device internally applied the requested configuration. Higher
     * layers (such as bma423.c) perform register read-back verification
     * where configuration integrity is critical.
     */
    esp_err_t err = i2c_master_cmd_begin(
        I2C_MASTER_NUM,
        cmd,
        pdMS_TO_TICKS(I2C_TRANSACTION_TIMEOUT_MS));

    if (err != ESP_OK)
    {
        printf("[I2C] Write transaction failed (ESP_ERR = 0x%x)\n", err);
    }

    /*
     * Release resources regardless of transaction outcome.
     */
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_mutex);

    return (err == ESP_OK) ? I2C_OK : I2C_ERR_BUS;
}

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

