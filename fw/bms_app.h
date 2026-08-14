#ifndef BMS_APP_H
#define BMS_APP_H

/*
 * Cooperative superloop for the MAX17320 monitor: three periodic jobs
 * driven off HAL_GetTick(), no RTOS.
 *
 *   poll    250 ms  -- one I2C pass over the gauge (it refreshes its own
 *                      outputs every 351 ms, so this is comfortably fast
 *                      enough without hammering the bus)
 *   render  500 ms  -- repaint the ANSI dashboard, or emit one JSON line
 *   cli     every iteration -- non-blocking key handling
 *
 * Nothing here blocks longer than one UART frame, so keys stay responsive
 * even while the gauge is unreachable.
 *
 * Wiring this into a CubeMX project:
 *
 *   main.c, USER CODE BEGIN Includes:  #include "bms_app.h"
 *   main.c, USER CODE BEGIN WHILE:     bms_app_init(&hi2c1, &hcom_uart[COM1]);
 *                                      ... then bms_app_run_once() in the loop
 *
 * The console handle comes from the Nucleo BSP rather than from a CubeMX
 * LPUART1 block: BSP_COM_Init(COM1, ...) in the generated main.c already
 * configures LPUART1 on PA2/PA3 at 115200 8N1, which is the pair wired to
 * the ST-Link VCP on this board. Init must therefore run after that call,
 * hence USER CODE BEGIN WHILE rather than USER CODE BEGIN 2.
 */

#include "max17320_monitor.h"
#include "bms_io.h"

void bms_app_init(I2C_HandleTypeDef *hi2c, UART_HandleTypeDef *huart);
void bms_app_run_once(void);

#endif /* BMS_APP_H */
