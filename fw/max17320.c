#include "max17320.h"

static uint8_t addr7_for(uint16_t reg_addr)
{
    return (reg_addr >= 0x180u) ? MAX17320_ADDR7_NV : MAX17320_ADDR7_MAIN;
}

const char *max17320_status_str(max17320_status_t st)
{
    switch (st) {
    case MAX17320_OK:              return "OK";
    case MAX17320_ERR_NOT_PRESENT: return "NOT PRESENT";
    case MAX17320_ERR_I2C:         return "I2C ERROR";
    case MAX17320_ERR_ARG:         return "BAD ARG";
    case MAX17320_ERR_MISMATCH:    return "READBACK MISMATCH";
    case MAX17320_ERR_NOT_CONFIRMED: return "NOT CONFIRMED";
    case MAX17320_ERR_NVM_ERROR:   return "NVM ERROR";
    case MAX17320_ERR_NOT_IMPLEMENTED: return "NOT BUILT IN";
    default:                       return "?";
    }
}

max17320_status_t max17320_probe(I2C_HandleTypeDef *hi2c)
{
    if (HAL_I2C_IsDeviceReady(hi2c, MAX17320_HAL_ADDR_MAIN, 2, MAX17320_I2C_TIMEOUT_MS) != HAL_OK) {
        return MAX17320_ERR_NOT_PRESENT;
    }
    if (HAL_I2C_IsDeviceReady(hi2c, MAX17320_HAL_ADDR_NV, 2, MAX17320_I2C_TIMEOUT_MS) != HAL_OK) {
        return MAX17320_ERR_NOT_PRESENT;
    }
    return MAX17320_OK;
}

max17320_status_t max17320_read_reg(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t *value)
{
    uint16_t hal_addr = (uint16_t)(addr7_for(addr) << 1);
    uint8_t  reg8     = (uint8_t)(addr & 0xFFu); /* Table 116: NV byte = addr - 0x100 == addr & 0xFF */
    uint8_t  rx[2]    = {0};

    if (value == NULL) {
        return MAX17320_ERR_ARG;
    }

    if (HAL_I2C_Master_Transmit(hi2c, hal_addr, &reg8, 1, MAX17320_I2C_TIMEOUT_MS) != HAL_OK) {
        return MAX17320_ERR_I2C;
    }
    if (HAL_I2C_Master_Receive(hi2c, hal_addr, rx, 2, MAX17320_I2C_TIMEOUT_MS) != HAL_OK) {
        return MAX17320_ERR_I2C;
    }

    *value = (uint16_t)(rx[0] | ((uint16_t)rx[1] << 8)); /* LSB first on the wire */
    return MAX17320_OK;
}

max17320_status_t max17320_write_reg(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t value)
{
    uint16_t hal_addr = (uint16_t)(addr7_for(addr) << 1);
    uint8_t  tx[3];

    tx[0] = (uint8_t)(addr & 0xFFu);
    tx[1] = (uint8_t)(value & 0xFFu);
    tx[2] = (uint8_t)(value >> 8);

    if (HAL_I2C_Master_Transmit(hi2c, hal_addr, tx, 3, MAX17320_I2C_TIMEOUT_MS) != HAL_OK) {
        return MAX17320_ERR_I2C;
    }
    return MAX17320_OK;
}

max17320_status_t max17320_read_block(I2C_HandleTypeDef *hi2c, uint16_t addr,
                                      uint16_t *out, size_t count)
{
    if (out == NULL) {
        return MAX17320_ERR_ARG;
    }

    for (size_t i = 0; i < count; i++) {
        max17320_status_t st = max17320_read_reg(hi2c, (uint16_t)(addr + i), &out[i]);
        if (st != MAX17320_OK) {
            return st;
        }
    }
    return MAX17320_OK;
}
