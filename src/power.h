#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include "i2c.h"

#define AXP202_LDO2_BIT  2
#define AXP202_LDO3_BIT  3

i2c_status_t power_init(void);
i2c_status_t axp202_read_reg(uint8_t reg, uint8_t *data);
i2c_status_t axp202_write_reg(uint8_t reg, uint8_t data);
i2c_status_t axp202_enable_dcdc3(void);

void i2c_scan(void);

#endif