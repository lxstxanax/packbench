#include "i2c_bus.h"

#define I2C_BUS_TIMEOUT_MS   100
#define I2C_BUS_RETRIES      3

static uint16_t I2C_Bus_MakeHalAddress(uint8_t dev_addr_7bit)
{
    return (uint16_t)(dev_addr_7bit << 1);
}

HAL_StatusTypeDef I2C_Bus_IsDeviceReady(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit
)
{
    if (hi2c == NULL)
    {
        return HAL_ERROR;
    }

    return HAL_I2C_IsDeviceReady(
        hi2c,
        I2C_Bus_MakeHalAddress(dev_addr_7bit),
        I2C_BUS_RETRIES,
        I2C_BUS_TIMEOUT_MS
    );
}

HAL_StatusTypeDef I2C_Bus_WriteReg16(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint16_t value
)
{
    if (hi2c == NULL)
    {
        return HAL_ERROR;
    }

    uint8_t data[2];

    // Big-endian: старший байт первым
    data[0] = (uint8_t)((value >> 8) & 0xFF);
    data[1] = (uint8_t)(value & 0xFF);

    return HAL_I2C_Mem_Write(
        hi2c,
        I2C_Bus_MakeHalAddress(dev_addr_7bit),
        reg_addr,
        reg_addr_size,
        data,
        2,
        I2C_BUS_TIMEOUT_MS
    );
}

HAL_StatusTypeDef I2C_Bus_ReadReg16(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint16_t *value
)
{
    if (hi2c == NULL || value == NULL)
    {
        return HAL_ERROR;
    }

    uint8_t data[2];

    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(
        hi2c,
        I2C_Bus_MakeHalAddress(dev_addr_7bit),
        reg_addr,
        reg_addr_size,
        data,
        2,
        I2C_BUS_TIMEOUT_MS
    );

    if (status != HAL_OK)
    {
        return status;
    }

    // Big-endian: старший байт первым
    *value = ((uint16_t)data[0] << 8) |
             ((uint16_t)data[1]);

    return HAL_OK;
}

HAL_StatusTypeDef I2C_Bus_WriteReg32(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint32_t value
)
{
    if (hi2c == NULL)
    {
        return HAL_ERROR;
    }

    uint8_t data[4];

    // Big-endian: старший байт первым
    data[0] = (uint8_t)((value >> 24) & 0xFF);
    data[1] = (uint8_t)((value >> 16) & 0xFF);
    data[2] = (uint8_t)((value >> 8)  & 0xFF);
    data[3] = (uint8_t)(value & 0xFF);

    return HAL_I2C_Mem_Write(
        hi2c,
        I2C_Bus_MakeHalAddress(dev_addr_7bit),
        reg_addr,
        reg_addr_size,
        data,
        4,
        I2C_BUS_TIMEOUT_MS
    );
}

HAL_StatusTypeDef I2C_Bus_ReadReg32(
    I2C_HandleTypeDef *hi2c,
    uint8_t dev_addr_7bit,
    uint16_t reg_addr,
    I2C_RegAddrSize_t reg_addr_size,
    uint32_t *value
)
{
    if (hi2c == NULL || value == NULL)
    {
        return HAL_ERROR;
    }

    uint8_t data[4];

    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(
        hi2c,
        I2C_Bus_MakeHalAddress(dev_addr_7bit),
        reg_addr,
        reg_addr_size,
        data,
        4,
        I2C_BUS_TIMEOUT_MS
    );

    if (status != HAL_OK)
    {
        return status;
    }

    // Big-endian: старший байт первым
    *value = ((uint32_t)data[0] << 24) |
             ((uint32_t)data[1] << 16) |
             ((uint32_t)data[2] << 8)  |
             ((uint32_t)data[3]);

    return HAL_OK;
}