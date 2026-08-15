#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "stm32g4xx_hal.h"
#include <stdint.h>

typedef enum
{
    I2C_REG_ADDR_8BIT  = I2C_MEMADD_SIZE_8BIT,
    I2C_REG_ADDR_16BIT = I2C_MEMADD_SIZE_16BIT
} I2C_RegAddrSize_t;

HAL_StatusTypeDef I2C_Bus_WriteReg16(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint16_t value
);

HAL_StatusTypeDef I2C_Bus_ReadReg16(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint16_t *value
);

HAL_StatusTypeDef I2C_Bus_WriteReg32(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint32_t value
);

HAL_StatusTypeDef I2C_Bus_ReadReg32(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint32_t *value
);

HAL_StatusTypeDef I2C_Bus_IsDeviceReady(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit
);

#endif