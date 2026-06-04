#include "i2c.h"
#include "driver/i2c.h"

#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_FREQ_HZ 400000

i2c_status_t i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };

    // Configure I2C parameters
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return I2C_ERR_BUS;

    // Install I2C driver
    err = i2c_driver_install(I2C_MASTER_NUM,conf.mode,0,0,0);
    if (err != ESP_OK)
   {
    return I2C_ERR_BUS;
  }
    return I2C_OK;
}

i2c_status_t i2c_write(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    // Create I2C command link (transaction container)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    // Generate START condition to begin I2C transaction
    i2c_master_start(cmd);

    // Send device address with WRITE bit
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);

    // Send register address to specify target register inside the device
    i2c_master_write_byte(cmd, reg, true);

    // Send data bytes (if any)
    if (len > 0 && data != NULL) {
        i2c_master_write(cmd, data, len, true);
    }

    // Generate STOP condition to end transaction
    i2c_master_stop(cmd);

    // Execute the queued I2C commands with timeout
    esp_err_t err = i2c_master_cmd_begin(
        I2C_MASTER_NUM,
        cmd,
        pdMS_TO_TICKS(10)
    );

    // Free the command link resources
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK)
    {
    return I2C_ERR_BUS;
    }
    return I2C_OK;
}

i2c_status_t i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (len == 0 || data == NULL) return I2C_ERR_INVALID_ARG;

    // Create I2C command link (transaction container)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    // Generate START condition to begin I2C transaction
    i2c_master_start(cmd);

    // Send device address with WRITE bit to set register address
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);

    // Repeated START: switch from write phase to read phase without releasing the bus
    i2c_master_start(cmd);

    // Send device address with READ bit
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);

    // Read data bytes into buffer
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);

    // Generate STOP condition to end transaction
    i2c_master_stop(cmd);

    // Execute the queued I2C commands with timeout
    esp_err_t err = i2c_master_cmd_begin(
        I2C_MASTER_NUM,
        cmd,
        pdMS_TO_TICKS(10)
    );

    // Free the command link resources
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK)
    {
        return I2C_ERR_BUS;
    }
    return I2C_OK;
}
