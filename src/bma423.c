#include "bma423.h"
#include "bma423_regs.h"
#include <stdint.h>
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    printf("[DEBUG] Soft reset sent\n");
    /* Step 2: Wait for device reboot */

    vTaskDelay(pdMS_TO_TICKS(1));
    printf("[DEBUG] Starting chip ID read\n");

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
    esp_rom_delay_us(1000);  // BMA423 datasheet: min 1000µs inter-write delay
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
    esp_rom_delay_us(1000);  // BMA423 datasheet: min 1000µs inter-write delay
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
    esp_rom_delay_us(1000);  // BMA423 datasheet: min 1000µs inter-write delay
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
    // Step 10: Verify configuration (read back at least one register)
    /* Configuration already verified through per-register readback checks */

    return BMA423_OK;
}