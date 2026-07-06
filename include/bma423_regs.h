#ifndef BMA423_REGS_H
#define BMA423_REGS_H

#define BMA423_ADDR               0x19
#define BMA423_CHIP_ID_REG        0x00
#define BMA423_DATA_X_LSB_REG     0x0A
#define BMA423_CMD_REG            0x7E
#define BMA423_SOFT_RESET_CMD     0xB6
#define BMA423_ACC_CONF_REG       0x40
#define BMA423_ACC_RANGE_REG      0x41
#define BMA423_PWR_CTRL_REG       0x7D
#define BMA423_PWR_CONF_REG       0x7C
#define BMA423_INT_MAP_DATA_REG   0x58
#define BMA423_INT1_IO_CTRL_REG   0x53
#define BMA423_INT_STATUS_1_REG   0x1D
#define BMA423_EXPECTED_CHIP_ID   0x13
#define BMA423_ERR_REG            0x02
#define BMA423_INT_MAP_REG        0x58
#define BMA423_STATUS_REG         0x03

//ACC_CONF register bit definitions
// perf_mode (bit 7)
#define ACC_PERF_MODE_AVG      0x00   // perf_mode = 0 → averaging / power mode
// acc_bwp (bits 6:4)
#define ACC_BWP_AVG_4          0x02   // bwp = 0x02 → avg 4 samples (with perf_mode=0)
// acc_odr (bits 3:0)
#define ACC_ODR_25HZ           0x06   // odr = 0x06 → 25 Hz
#define ACC_ODR_50HZ           0x07   // odr = 0x07 → 50 Hz
#define ACC_CONF_PERF_MODE_POS   7
#define ACC_CONF_PERF_MODE_MASK  (0x01 << ACC_CONF_PERF_MODE_POS)
#define ACC_CONF_BWP_POS         4
#define ACC_CONF_BWP_MASK        (0x07 << ACC_CONF_BWP_POS)
#define ACC_CONF_ODR_POS         0
#define ACC_CONF_ODR_MASK        (0x0F << ACC_CONF_ODR_POS)

// ACC_RANGE register bit definitions
#define ACC_RANGE_2G          0x00   // acc_range = 0x00 → ±2g
#define ACC_RANGE_4G          0x01   // acc_range = 0x01 → ±4g
#define ACC_RANGE_8G          0x02   // acc_range = 0x02 → ±8g
#define ACC_RANGE_16G         0x03   // acc_range = 0x03 → ±16g

//INT_IO_CTRL register bit definitions

/* input_en (bit 4) */
#define INT1_INPUT_DIS              0x00   // Input disabled
#define INT1_INPUT_EN               0x01   // Input enabled

/* output_en (bit 3) */
#define INT1_OUTPUT_DIS             0x00   // Output disabled
#define INT1_OUTPUT_EN              0x01   // Output enabled

/* od (bit 2) */
#define INT1_PUSH_PULL              0x00   // Push-pull output
#define INT1_OPEN_DRAIN             0x01   // Open-drain output

/* lvl (bit 1) */
#define INT1_ACTIVE_LOW             0x00   // Active low
#define INT1_ACTIVE_HIGH            0x01   // Active high

/* edge_ctrl (bit 0) */
#define INT1_LEVEL_TR               0x00   // Level-triggered
#define INT1_EDGE_TR                0x01   // Edge-triggered

/* --------------------------------------------------------------------------
 * Bit Positions
 * -------------------------------------------------------------------------- */

#define INT1_IO_CTRL_INPUT_EN_POS    4
#define INT1_IO_CTRL_OUTPUT_EN_POS   3
#define INT1_IO_CTRL_OD_POS          2
#define INT1_IO_CTRL_LVL_POS         1
#define INT1_IO_CTRL_EDGE_CTRL_POS   0

/* --------------------------------------------------------------------------
 * Bit Masks
 * -------------------------------------------------------------------------- */

#define INT1_IO_CTRL_INPUT_EN_MASK   (0x01 << INT1_IO_CTRL_INPUT_EN_POS)
#define INT1_IO_CTRL_OUTPUT_EN_MASK  (0x01 << INT1_IO_CTRL_OUTPUT_EN_POS)
#define INT1_IO_CTRL_OD_MASK         (0x01 << INT1_IO_CTRL_OD_POS)
#define INT1_IO_CTRL_LVL_MASK        (0x01 << INT1_IO_CTRL_LVL_POS)
#define INT1_IO_CTRL_EDGE_CTRL_MASK  (0x01 << INT1_IO_CTRL_EDGE_CTRL_POS)

// INT_MAP_DATA_REG bit definitions (mapping interrupt sources to INT1 pin)

/* Data Ready Interrupt Mapping */

#define INT_MAP_DRDY_INT1          0x01    // bit2 = 1 -> DRDY mapped to INT1
#define INT_MAP_DRDY_INT2          0x01    // bit6 = 1 -> DRDY mapped to INT2

/* Bit Positions */

#define INT_MAP_DRDY_INT1_POS      2
#define INT_MAP_DRDY_INT2_POS      6

/* Bit Masks */

#define INT_MAP_DRDY_INT1_MASK     (0x01 << INT_MAP_DRDY_INT1_POS)
#define INT_MAP_DRDY_INT2_MASK     (0x01 << INT_MAP_DRDY_INT2_POS)


/* --------------------------------------------------------------------------
 * ERR_REG (0x02)
 * -------------------------------------------------------------------------- */

/* Bit Positions */

#define ERR_REG_FATAL_ERR_POS      0
#define ERR_REG_CMD_ERR_POS        1
#define ERR_REG_ERROR_CODE_POS     2
#define ERR_REG_FIFO_ERR_POS       6
#define ERR_REG_AUX_ERR_POS        7

/* Bit Masks */

#define ERR_REG_FATAL_ERR_MASK     (0x01 << ERR_REG_FATAL_ERR_POS)
#define ERR_REG_CMD_ERR_MASK       (0x01 << ERR_REG_CMD_ERR_POS)
#define ERR_REG_ERROR_CODE_MASK    (0x07 << ERR_REG_ERROR_CODE_POS)
#define ERR_REG_FIFO_ERR_MASK      (0x01 << ERR_REG_FIFO_ERR_POS)
#define ERR_REG_AUX_ERR_MASK       (0x01 << ERR_REG_AUX_ERR_POS)

/* Error Code Values (bits [4:2]) */

#define ERR_CODE_NO_ERROR          0x00
#define ERR_CODE_ACC_ERR           0x01

//PWR_CTRL register bit definitions
#define BMA423_PWR_CTRL_REG     0x7D
#define PWR_CTRL_ACC_EN_POS     2
#define PWR_CTRL_ACC_EN_MASK    (0x01 << PWR_CTRL_ACC_EN_POS)
#define PWR_CTRL_ACC_EN         0x01

//Status Register bit definitions
// drdy_acc (bit 7)
#define STATUS_DRDY_ACC_NOT_READY  0x00   // Data not ready for accelerometer
#define STATUS_DRDY_ACC_READY      0x01   // Data ready for accelerometer (resets on read)

// drdy_aux (bit 5)
#define STATUS_DRDY_AUX_NOT_READY  0x00   // Data not ready for auxiliary sensor
#define STATUS_DRDY_AUX_READY      0x01   // Data ready for auxiliary sensor (resets on read)

// cmd_rdy (bit 4)
#define STATUS_CMD_IN_PROGRESS     0x00   // Command in progress
#define STATUS_CMD_READY           0x01   // Command decoder is ready to accept a new command

// aux_man_op (bit 2)
#define STATUS_AUX_MAN_OP_IDLE     0x00   // No manual auxiliary interface operation is ongoing
#define STATUS_AUX_MAN_OP_ONGOING  0x01   // Manual auxiliary interface operation is ongoing

// Bit Positions
#define STATUS_DRDY_ACC_POS        7
#define STATUS_DRDY_AUX_POS        5
#define STATUS_CMD_RDY_POS         4
#define STATUS_AUX_MAN_OP_POS      2

// Bit Masks
#define STATUS_DRDY_ACC_MASK       (0x01 << STATUS_DRDY_ACC_POS)
#define STATUS_DRDY_AUX_MASK       (0x01 << STATUS_DRDY_AUX_POS)
#define STATUS_CMD_RDY_MASK        (0x01 << STATUS_CMD_RDY_POS)
#define STATUS_AUX_MAN_OP_MASK     (0x01 << STATUS_AUX_MAN_OP_POS)

//Axes data registers
#define BMA423_ACC_X_LSB_REG        0x12
#define BMA423_ACC_X_MSB_REG        0x13
#define BMA423_ACC_Y_LSB_REG        0x14
#define BMA423_ACC_Y_MSB_REG        0x15
#define BMA423_ACC_Z_LSB_REG        0x16
#define BMA423_ACC_Z_MSB_REG        0x17

#endif // BMA423_REGS_H