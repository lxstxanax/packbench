#ifndef MAX17320_H
#define MAX17320_H

/*
 * MAX17320 I2C transport layer for STM32G4 (read-only monitoring build).
 *
 * Ported from the F7 provisioning driver in
 * github.com/lxstxanax/stm32f767zi-max17320-bms: the I2C primitives
 * (probe / read_reg / write_reg) are carried over verbatim, the NVM
 * provisioning half (shadow config write, verify, commit_nvm,
 * post-commit reset) is deliberately NOT ported here.
 *
 * This build must never consume one of the part's 7 lifetime NVM writes,
 * so the only code that could do so is simply absent rather than gated.
 * To provision a board, use the original F7 driver.
 *
 * Addresses per MAX17320 datasheet Table 116 "2-Wire Slave Addresses":
 *   - slave 6Ch (8-bit) / 0x36 (7-bit) maps memory 000h-0FFh 1:1;
 *   - slave 16h (8-bit) / 0x0B (7-bit) maps memory 100h-1FFh with the
 *     on-wire address byte = (memory address - 0x100), i.e. addr & 0xFF.
 */

#include <stdint.h>
#include <stddef.h>
#include "stm32g4xx_hal.h"

/* ---- I2C addresses: 7-bit vs HAL's 8-bit (shifted, R/W-bit-in-bit0) ----
 * HAL_I2C_Master_* / HAL_I2C_IsDeviceReady all want the 8-bit form
 * (7-bit address already shifted left by 1); keep the two representations
 * distinct so callers never have to remember to shift. */
#define MAX17320_ADDR7_MAIN        (0x36u)  /* datasheet Table 116: slave 6Ch */
#define MAX17320_ADDR7_NV          (0x0Bu)  /* datasheet Table 116: slave 16h */
#define MAX17320_HAL_ADDR_MAIN     ((uint16_t)(MAX17320_ADDR7_MAIN << 1))
#define MAX17320_HAL_ADDR_NV       ((uint16_t)(MAX17320_ADDR7_NV   << 1))

/* Per-transfer I2C timeout. The gauge is a simple register device with no
 * clock stretching worth speaking of; anything slower than this means the
 * bus is stuck (missing pull-up, held-low SDA) rather than busy. */
#define MAX17320_I2C_TIMEOUT_MS    (50u)

typedef enum {
    MAX17320_OK = 0,
    MAX17320_ERR_NOT_PRESENT,
    MAX17320_ERR_I2C,
    MAX17320_ERR_ARG,
    /* Provisioning-only outcomes (max17320_provision.c). Kept in this
     * shared enum so one status type covers the whole driver. */
    MAX17320_ERR_MISMATCH,        /* readback did not match what was written */
    MAX17320_ERR_NOT_CONFIRMED,   /* commit refused: no token, or supply too low */
    MAX17320_ERR_NVM_ERROR,       /* CommStat.NVError set after Copy NV Block */
    MAX17320_ERR_NOT_IMPLEMENTED, /* built without MAX17320_I_KNOW_THIS_BURNS_NVM */
} max17320_status_t;

/* Human-readable name for a status code, for the dashboard/log. */
const char *max17320_status_str(max17320_status_t st);

/* Checks both the main and NV I2C addresses respond.
 * NOTE: the gauge is powered from the pack (BATTP -> 10R -> IN), so with no
 * battery attached this correctly reports MAX17320_ERR_NOT_PRESENT. A
 * missing bus pull-up looks identical -- check the pull-ups first. */
max17320_status_t max17320_probe(I2C_HandleTypeDef *hi2c);

/* Word (16-bit) register read/write. addr selects which I2C address/byte
 * mapping is used automatically (>= 0x180 -> NV slave, address byte =
 * addr & 0xFF per Table 116; else main slave, address byte = addr). */
max17320_status_t max17320_read_reg(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t *value);
max17320_status_t max17320_write_reg(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t value);

/* Reads count consecutive words starting at addr into out[]. Used by the
 * monitor to grab a contiguous block in one pass; falls back to
 * word-at-a-time reads so it stays valid across the 0x0FF/0x180 slave
 * boundary. */
max17320_status_t max17320_read_block(I2C_HandleTypeDef *hi2c, uint16_t addr,
                                      uint16_t *out, size_t count);

#endif /* MAX17320_H */
