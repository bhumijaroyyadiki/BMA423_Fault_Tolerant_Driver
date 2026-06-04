#ifndef BMA423_H
#define BMA423_H

typedef enum
{
    BMA423_OK = 0,

    BMA423_ERR_BUS,
    BMA423_ERR_CHIP_ID,
    BMA423_ERR_RESET,
    BMA423_ERR_CONFIG,
    BMA423_ERR_FATAL,
    BMA423_ERR_CMD
} bma423_status_t;

bma423_status_t bma423_init(void);

#endif