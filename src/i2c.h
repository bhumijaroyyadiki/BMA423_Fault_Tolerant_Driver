#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    I2C_OK = 0,
    I2C_ERR_BUS,
    I2C_ERR_TIMEOUT,
    I2C_ERR_INVALID_ARG
} i2c_status_t;

i2c_status_t i2c_init(void);
i2c_status_t i2c_write(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len);
i2c_status_t i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len);

#endif // I2C_H