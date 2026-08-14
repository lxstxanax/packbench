#ifndef STM32G4XX_HAL_H
#define STM32G4XX_HAL_H
#include <stdint.h>
#include <stddef.h>
typedef enum { HAL_OK=0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
#define HAL_MAX_DELAY 0xFFFFFFFFU
typedef struct { uint32_t dummy; } I2C_TypeDef;
typedef struct { volatile uint32_t RDR; volatile uint32_t ISR; volatile uint32_t ICR; } USART_TypeDef;
typedef struct { I2C_TypeDef *Instance; } I2C_HandleTypeDef;
typedef struct { USART_TypeDef *Instance; } UART_HandleTypeDef;
typedef struct { uint32_t d; } GPIO_TypeDef;
#define UART_FLAG_ORE (1u<<3)
#define UART_FLAG_RXNE (1u<<5)
#define __HAL_UART_GET_FLAG(h,f) (((h)->Instance->ISR & (f)) != 0u)
#define __HAL_UART_CLEAR_OREFLAG(h) ((h)->Instance->ICR = (1u<<3))
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef*, uint16_t, uint8_t*, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef*, uint16_t, uint8_t*, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef*, uint16_t, uint32_t, uint32_t);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef*, const uint8_t*, uint16_t, uint32_t);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t);
void HAL_GPIO_TogglePin(GPIO_TypeDef*, uint16_t);
#endif
