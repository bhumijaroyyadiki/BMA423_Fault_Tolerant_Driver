#ifndef BMA423_H
#define BMA423_H

#define MAX_FORCED_FAILURES 20

#include <stdint.h>
#include <stdbool.h>

extern bool g_inject_fault;
extern int g_fault_count;

typedef enum
{
    BMA423_OK = 0,

    BMA423_ERR_BUS,
    BMA423_ERR_CHIP_ID,
    BMA423_ERR_RESET,
    BMA423_ERR_CONFIG,
    BMA423_ERR_FATAL,
    BMA423_ERR_CMD,
    BMA423_ERR_NO_DATA,
    BMA423_ERR_INVALID_ARG
} bma423_status_t;

bma423_status_t bma423_init(void);
bma423_status_t bma423_read_accel(
    int16_t *x,
    int16_t *y,
    int16_t *z);
#endif