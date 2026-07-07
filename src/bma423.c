/**
 * bma423.c
 *
 * Register-level protocol driver for the Bosch BMA423 accelerometer.
 *
 * This file has zero knowledge of ESP-IDF, FreeRTOS tasks, GPIO, or
 * interrupts (aside from the platform delay macro). It only calls
 * i2c_read()/i2c_write() and BMA423_DELAY_US(). This is deliberate: the
 * sensor protocol layer is designed to be portable to another MCU by
 * replacing only the I2C transport layer (i2c.c) and the platform delay
 * header — see docs/08_Software_Architecture.md.
 *
 * Every configuration register write is followed by a read-back and an
 * explicit comparison before the driver trusts the write took effect.
 * An I2C ACK only confirms a byte was accepted on the wire — it does not
 * confirm the device's internal register logic actually applied it.
 * See docs/22_Defensive_Programming_Practices.md.
 */

#include "bma423.h"
#include "bma423_regs.h"
#include <stdint.h>
#include <stdio.h>
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bma423_platform.h"

/* -------------------------------------------------------------------------
 * Shared write-then-verify primitive.
 *
 * Every configuration register in bma423_init() follows the same pattern:
 * write a value, read it back, and fail loudly if the read-back doesn't
 * match. This helper exists so that pattern is implemented once, not
 * copy-pasted five times with room for one copy to quietly skip a check
 * (which is exactly what happened with PWR_CONF before this cleanup).
 * ---------------------------------------------------------------------- */
static bma423_status_t write_and_verify_reg(uint8_t reg, uint8_t value, const char *label)
{
    if (i2c_write(BMA423_ADDR, reg, &value, 1) != I2C_OK) {
        return BMA423_ERR_BUS;
    }

    uint8_t readback = 0;
    if (i2c_read(BMA423_ADDR, reg, &readback, 1) != I2C_OK) {
        return BMA423_ERR_BUS;
    }

    if (readback != value) {
        printf("[%s] Verification failed (wrote 0x%02X, read back 0x%02X)\n",
               label, value, readback);
        return BMA423_ERR_CONFIG;
    }

    return BMA423_OK;
}

bma423_status_t bma423_init(void)
{
    bma423_status_t status;
    uint8_t chip_id = 0;

    /* ---- Step 1: Soft reset ----------------------------------------
     * Forces the sensor to a known power-on-equivalent state regardless
     * of what state it was left in by a previous boot cycle or a prior
     * Tier-2 recovery re-init. This is not optional boilerplate — every
     * step after this one assumes the sensor started from this known
     * state. */
    uint8_t reset_cmd = BMA423_SOFT_RESET_CMD;
    if (i2c_write(BMA423_ADDR, BMA423_CMD_REG, &reset_cmd, 1) != I2C_OK) {
        return BMA423_ERR_BUS;
    }

    /* ---- Step 2: Post-reset settle ---------------------------------
     * Datasheet-specified reboot time is on the order of a few ms; 50 ms
     * is used as a conservative safety margin. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- Step 3: Dummy chip-ID read ---------------------------------
     * The first chip-ID read immediately after a soft reset has been
     * observed to occasionally return garbage while the sensor's
     * internal state machine finishes its post-reset NVM load. This
     * throwaway read clears that transient state before the real
     * chip-ID check below. Its result is intentionally discarded. */
    i2c_read(BMA423_ADDR, BMA423_CHIP_ID_REG, &chip_id, 1);

    /* ---- Step 4: Chip-ID verification ------------------------------
     * Confirms the driver is actually talking to a BMA423 at the
     * expected address, not a different device or an unresponsive bus. */
    if (i2c_read(BMA423_ADDR, BMA423_CHIP_ID_REG, &chip_id, 1) != I2C_OK) {
        printf("[CHIP_ID] Read failed\n");
        return BMA423_ERR_BUS;
    }

    printf("[CHIP_ID] Read = 0x%02X\n", chip_id);

    if (chip_id != BMA423_EXPECTED_CHIP_ID) {
        printf("[CHIP_ID] Unexpected CHIP_ID\n");
        return BMA423_ERR_CHIP_ID;
    }

    printf("[CHIP_ID] BMA423 detected correctly\n");

    /* ---- Step 5: ERR_REG check --------------------------------------
     * Distinguishes a fatal internal sensor error, a command error, and
     * a configuration error from each other, rather than treating any
     * non-zero ERR_REG as one generic failure. */
    uint8_t err_reg = 0;
    if (i2c_read(BMA423_ADDR, BMA423_ERR_REG, &err_reg, 1) != I2C_OK) {
        return BMA423_ERR_BUS;
    }

    printf("[ERR_REG] = 0x%02X\n", err_reg);

    if (err_reg & ERR_REG_FATAL_ERR_MASK) {
        return BMA423_ERR_FATAL;
    }
    if (err_reg & ERR_REG_CMD_ERR_MASK) {
        return BMA423_ERR_CMD;
    }
    if (err_reg & ERR_REG_ERROR_CODE_MASK) {
        return BMA423_ERR_CONFIG;
    }

    /* ---- Step 6: ACC_CONF ------------------------------------------
     * Output data rate, bandwidth, and performance mode. Values are
     * chosen for step-counting-class motion sensing on a wearable, not
     * arbitrary — see docs/23_Design_Decisions.md (23.9) for the
     * Nyquist/power reasoning behind 25 Hz / avg4 / power mode. */
    uint8_t acc_conf =
        ((ACC_PERF_MODE_AVG & 0x01) << ACC_CONF_PERF_MODE_POS) |
        ((ACC_BWP_AVG_4     & 0x07) << ACC_CONF_BWP_POS)       |
        ((ACC_ODR_25HZ      & 0x0F) << ACC_CONF_ODR_POS);

    status = write_and_verify_reg(BMA423_ACC_CONF_REG, acc_conf, "ACC_CONF");
    if (status != BMA423_OK) {
        return status;
    }
    BMA423_DELAY_US(1000); /* BMA423 datasheet: min 1000us inter-write delay */

    /* ---- Step 7: ACC_RANGE ------------------------------------------
     * +/-4g. Written explicitly even though it matches the sensor's
     * reset default — relying on a reset default silently assumes the
     * sensor was freshly reset and nothing else touched this register.
     * See docs/23_Design_Decisions.md (23.8). */
    uint8_t acc_range = ACC_RANGE_4G;

    status = write_and_verify_reg(BMA423_ACC_RANGE_REG, acc_range, "ACC_RANGE");
    if (status != BMA423_OK) {
        return status;
    }
    BMA423_DELAY_US(1000);

    /* ---- Step 8: INT1_IO_CTRL ---------------------------------------
     * Push-pull, active-high, edge-triggered output — deliberately
     * matched to the ESP32 GPIO39 edge-triggered interrupt configuration
     * in bma423_isr.c. A mismatch here is a classic source of missed or
     * duplicated interrupts. */
    uint8_t int1_io_ctrl =
        (INT1_INPUT_DIS   << INT1_IO_CTRL_INPUT_EN_POS)  |
        (INT1_OUTPUT_EN   << INT1_IO_CTRL_OUTPUT_EN_POS) |
        (INT1_PUSH_PULL   << INT1_IO_CTRL_OD_POS)        |
        (INT1_ACTIVE_HIGH << INT1_IO_CTRL_LVL_POS)       |
        (INT1_EDGE_TR     << INT1_IO_CTRL_EDGE_CTRL_POS);

    status = write_and_verify_reg(BMA423_INT1_IO_CTRL_REG, int1_io_ctrl, "INT1_IO_CTRL");
    if (status != BMA423_OK) {
        return status;
    }
    BMA423_DELAY_US(1000);

    /* ---- Step 9: INT_MAP --------------------------------------------
     * Routes the data-ready interrupt to INT1 — the only interrupt line
     * actually wired to the MCU in this design. */
    uint8_t int_map = (INT_MAP_DRDY_INT1 << INT_MAP_DRDY_INT1_POS);

    status = write_and_verify_reg(BMA423_INT_MAP_REG, int_map, "INT_MAP");
    if (status != BMA423_OK) {
        return status;
    }
    printf("[INT_MAP] = 0x%02X\n", int_map);
    BMA423_DELAY_US(1000);

    /* ---- Step 10: PWR_CONF ------------------------------------------
     * Disables advanced power save. Keeps interrupt-to-data latency
     * predictable during this development phase rather than optimizing
     * for power yet — see docs/20_Power_Considerations.md. Now goes
     * through the same write-verify path as every other register here;
     * previously this write's result was not checked at all. */
    status = write_and_verify_reg(BMA423_PWR_CONF_REG, 0x00, "PWR_CONF");
    if (status != BMA423_OK) {
        return status;
    }
    BMA423_DELAY_US(1000);

    /* ---- Step 11: PWR_CTRL ------------------------------------------
     * Enables the accelerometer. Reset default leaves the accelerometer
     * DISABLED — without this explicit write, every step above would
     * report success and every data register would silently read zero.
     * This was discovered empirically during development (see
     * docs/26_Implementation_Walkthrough.md, 26.4) and is the reason
     * this write is verified rather than assumed. */
    uint8_t pwr_ctrl = (PWR_CTRL_ACC_EN << PWR_CTRL_ACC_EN_POS);

    status = write_and_verify_reg(BMA423_PWR_CTRL_REG, pwr_ctrl, "PWR_CTRL");
    if (status != BMA423_OK) {
        return status;
    }
    BMA423_DELAY_US(1000);

    printf("[PWR_CTRL] Accelerometer enabled\n");

    return BMA423_OK;
}

/**
 * bma423_read_accel()
 *
 * Reads the six acceleration data registers in a single I2C burst and
 * reconstructs three signed 12-bit values with correct sign extension.
 * See docs/25_API_Reference.md for the full register layout and bit
 * manipulation explanation.
 */
bma423_status_t bma423_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t buf[6];

    if (x == NULL || y == NULL || z == NULL) {
        return BMA423_ERR_INVALID_ARG;
    }

    /* Settling delay: an implementation-specific safeguard, not a
     * datasheet-mandated requirement (the datasheet specifies the
     * interrupt only asserts once a full sample set is available, but
     * gives no explicit post-interrupt delay figure). This 100us guard
     * was introduced as a conservative margin against that assumption.
     * See docs/17_Interrupt_Handling.md (17.4) for the full reasoning
     * and the cost of removing it. */
    BMA423_DELAY_US(100);

    i2c_status_t i2c_result = i2c_read(BMA423_ADDR, BMA423_ACC_X_LSB_REG, buf, 6);
    if (i2c_result != I2C_OK) {
        printf("[BMA423] Burst read failed (i2c_status=%d)\n", i2c_result);
        return BMA423_ERR_BUS;
    }

    /* Each axis is a 12-bit signed value packed across an LSB/MSB
     * register pair (4 don't-care bits in the LSB register). Byte-
     * concatenating naively would place the 12 significant bits in the
     * wrong position and would not sign-extend correctly for negative
     * acceleration. The left-shift realigns bits [11:0] into position;
     * the arithmetic right-shift then sign-extends from bit 11. This
     * only works correctly because x/y/z are int16_t (signed), which
     * guarantees an arithmetic (not logical) right shift. */
    uint16_t raw_x = ((uint16_t)buf[1] << 4) | (buf[0] >> 4);
    uint16_t raw_y = ((uint16_t)buf[3] << 4) | (buf[2] >> 4);
    uint16_t raw_z = ((uint16_t)buf[5] << 4) | (buf[4] >> 4);

    *x = ((int16_t)(raw_x << 4)) >> 4;
    *y = ((int16_t)(raw_y << 4)) >> 4;
    *z = ((int16_t)(raw_z << 4)) >> 4;

    return BMA423_OK;
}