#include "i2c.h"
#include "driver/i2c.h"
#include "freertos/semphr.h"
#include <stdio.h>

#define I2C_MASTER_NUM       I2C_NUM_0
#define I2C_MASTER_SDA_IO    21
#define I2C_MASTER_SCL_IO    22
#define I2C_MASTER_FREQ_HZ   400000

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