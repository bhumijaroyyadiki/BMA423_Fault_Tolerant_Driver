#include "bma423.h"
#include "bma423_regs.h"
#include <stdint.h>
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bma423_platform.h"

bma423_status_t bma423_init(void)
{
    uint8_t cmd = BMA423_SOFT_RESET_CMD;
    uint8_t chip_id = 0;

    /* Step 1: Soft reset */

    if (i2c_write(BMA423_ADDR,
                  BMA423_CMD_REG,
                  &cmd,
                  1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }
    //printf("[DEBUG] Soft reset sent\n");
    
    /* Step 2: Wait for device reboot */
    vTaskDelay(pdMS_TO_TICKS(50));
    //printf("[DEBUG] Starting chip ID read\n");

    //Dummy Read to clear reset state and start internal processes
    /* Dummy read required after soft reset */
    i2c_read(BMA423_ADDR, BMA423_CHIP_ID_REG, &chip_id, 1);
    /* Step 3: Read CHIP_ID */

    if (i2c_read(BMA423_ADDR,
                 BMA423_CHIP_ID_REG,
                 &chip_id,
                 1) != I2C_OK)
    {
        printf("[CHIP_ID] FAIL\n");
        return BMA423_ERR_BUS;
    }

    printf("[CHIP_ID] Read = 0x%02X\n", chip_id);

    /* Step 4: Verify CHIP_ID */

    if (chip_id != BMA423_EXPECTED_CHIP_ID)
    {
        printf("[CHIP_ID] Unexpected CHIP_ID\n");
        return BMA423_ERR_CHIP_ID;
    }

    printf("[CHIP_ID] BMA423 detected correctly\n");

    // Step 5: Check ERR_REG
    uint8_t err_reg = 0;
    if (i2c_read(BMA423_ADDR,
             BMA423_ERR_REG,
             &err_reg,
             1) != I2C_OK)
    {
    return BMA423_ERR_BUS;
    }

    printf("[ERR_REG] = 0x%02X\n", err_reg);

    if (err_reg != 0)
    {
        if (err_reg & ERR_REG_FATAL_ERR_MASK)
        {
            return BMA423_ERR_FATAL;
        }

        if (err_reg & ERR_REG_CMD_ERR_MASK)
        {
            return BMA423_ERR_CMD;
        }

        if (err_reg & ERR_REG_ERROR_CODE_MASK)
        {
            return BMA423_ERR_CONFIG;
        }
    }

    // Step 6a: Configure ACC_CONF
    uint8_t acc_conf =
    ((ACC_PERF_MODE_AVG & 0x01) << ACC_CONF_PERF_MODE_POS) |
    ((ACC_BWP_AVG_4     & 0x07) << ACC_CONF_BWP_POS)       |
    ((ACC_ODR_25HZ      & 0x0F) << ACC_CONF_ODR_POS);

    if (i2c_write(BMA423_ADDR,
              BMA423_ACC_CONF_REG,
              &acc_conf,
              1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }
    // Step 6b: Verify ACC_CONF write by reading back the register
    uint8_t acc_conf_readback = 0;
    if (i2c_read(BMA423_ADDR,
             BMA423_ACC_CONF_REG,
             &acc_conf_readback,
             1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    if (acc_conf_readback != acc_conf)
    {
        printf("[ACC_CONF] Verification failed\n");
        return BMA423_ERR_CONFIG;
    }
    BMA423_DELAY_US(1000);  // BMA423 datasheet: min 1000µs inter-write delay
    // Step 7: Configure ACC_RANGE
    uint8_t acc_range = ACC_RANGE_4G; // ±4g
    if (i2c_write(BMA423_ADDR,
              BMA423_ACC_RANGE_REG,
              &acc_range,
              1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }
    // Step 7b: Verify ACC_RANGE write by reading back the register
    uint8_t acc_range_readback;
    if (i2c_read(BMA423_ADDR,
             BMA423_ACC_RANGE_REG,
             &acc_range_readback,
             1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }
    if (acc_range_readback != acc_range)
    {
        printf("[ACC_RANGE] Verification failed\n");
        return BMA423_ERR_CONFIG;
    }
    BMA423_DELAY_US(1000);  // BMA423 datasheet: min 1000µs inter-write delay
    // Step 8: Configure INT1_IO_CTRL
    uint8_t int1_io_ctrl  =
    (INT1_INPUT_DIS   << INT1_IO_CTRL_INPUT_EN_POS)  |
    (INT1_OUTPUT_EN   << INT1_IO_CTRL_OUTPUT_EN_POS) |
    (INT1_PUSH_PULL   << INT1_IO_CTRL_OD_POS)        |
    (INT1_ACTIVE_HIGH << INT1_IO_CTRL_LVL_POS)       |
    (INT1_EDGE_TR     << INT1_IO_CTRL_EDGE_CTRL_POS);

    if (i2c_write(BMA423_ADDR,
              BMA423_INT1_IO_CTRL_REG,
              &int1_io_ctrl,
              1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    // Step 8b: Verify INT1_IO_CTRL write by reading back the register
    uint8_t int1_io_ctrl_readback = 0;

    if (i2c_read(BMA423_ADDR,
                BMA423_INT1_IO_CTRL_REG,
                &int1_io_ctrl_readback,
                1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    if (int1_io_ctrl_readback != int1_io_ctrl)
    {
        printf("[INT1_IO_CTRL] Verification failed\n");
        return BMA423_ERR_CONFIG;
    }
    BMA423_DELAY_US(1000);  // BMA423 datasheet: min 1000µs inter-write delay
    // Step 9: Configure INT_MAP

    uint8_t int_map =
        (INT_MAP_DRDY_INT1 << INT_MAP_DRDY_INT1_POS);

    if (i2c_write(BMA423_ADDR,
                BMA423_INT_MAP_REG,
                &int_map,
                1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    uint8_t int_map_readback = 0;

    if (i2c_read(BMA423_ADDR,
                BMA423_INT_MAP_REG,
                &int_map_readback,
                1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    if (int_map_readback != int_map)
    {
        printf("[INT_MAP] Verification failed\n");
        return BMA423_ERR_CONFIG;
    }

    printf("[INT_MAP] = 0x%02X\n", int_map_readback);

    uint8_t pwr_conf = 0x00; // disable advanced power save
    i2c_write(BMA423_ADDR, BMA423_PWR_CONF_REG, &pwr_conf, 1);
    BMA423_DELAY_US(1000);
    // Step 10: Enable accelerometer
    // Reset default is 0x00 — accelerometer disabled
    // Must explicitly enable before data reads will work
    uint8_t pwr_ctrl = (PWR_CTRL_ACC_EN << PWR_CTRL_ACC_EN_POS);

    if (i2c_write(BMA423_ADDR,
                BMA423_PWR_CTRL_REG,
                &pwr_ctrl,
                1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    BMA423_DELAY_US(1000); // inter-write delay

    // Verify
    uint8_t pwr_ctrl_readback = 0;
    if (i2c_read(BMA423_ADDR,
                BMA423_PWR_CTRL_REG,
                &pwr_ctrl_readback,
                1) != I2C_OK)
    {
        return BMA423_ERR_BUS;
    }

    if (pwr_ctrl_readback != pwr_ctrl)
    {
        printf("[PWR_CTRL] Verification failed\n");
        return BMA423_ERR_CONFIG;
    }

    printf("[PWR_CTRL] Accelerometer enabled\n");

    return BMA423_OK;
}
// Correct 12-bit construction


bma423_status_t bma423_read_accel(int16_t *x, int16_t *y, int16_t *z)
{   
     
    uint8_t buf[6];

    if (x == NULL || y == NULL || z == NULL)
    {
        return BMA423_ERR_INVALID_ARG;
    }

    /*
     * Hardware Settling Delay:
     * The edge-triggered interrupt can arrive slightly before the internal 
     * sensor data registers are completely populated. A fast 100µs busy-wait 
     * ensures the internal ADC transfer is complete without making extra, 
     * slow I2C status polling transactions.
     */
    BMA423_DELAY_US(100);

    // Step 1: Burst read 6 bytes starting from ACC_X_LSB
    i2c_status_t i2c_result = i2c_read(BMA423_ADDR, 
                                       BMA423_ACC_X_LSB_REG, 
                                       buf, 
                                       6);
    
    // TEMPORARY LOGGING ATTACHMENT
    if (i2c_result != I2C_OK) 
    {
        printf("[DEBUG] i2c_read failed inside bma423_read_accel: %d\n", i2c_result);
        return BMA423_ERR_BUS;
    }

    // Step 2: Reconstruct 12-bit values from LSB/MSB pairs
    uint16_t raw_x = ((uint16_t)buf[1] << 4) | (buf[0] >> 4);
    uint16_t raw_y = ((uint16_t)buf[3] << 4) | (buf[2] >> 4);
    uint16_t raw_z = ((uint16_t)buf[5] << 4) | (buf[4] >> 4);

    // Step 3: Sign extension from 12-bit signed to 16-bit signed variables
    *x = ((int16_t)(raw_x << 4)) >> 4;
    *y = ((int16_t)(raw_y << 4)) >> 4;
    *z = ((int16_t)(raw_z << 4)) >> 4;

    return BMA423_OK;
}