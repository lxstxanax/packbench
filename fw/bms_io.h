#ifndef BMS_IO_H
#define BMS_IO_H

/*
 * Minimal blocking console over the ST-Link Virtual COM Port.
 *
 * On NUCLEO-G474RE the VCP is wired to LPUART1 (PA2/PA3, AF12) -- NOT to
 * USART2 as on the older Nucleo-64 boards, and NOT to USART1, which the
 * board routes to the Arduino D0/D1 pins (PC4/PC5) instead. See UM2505
 * section 6.6.5. Pass whichever handle CubeMX generated for LPUART1.
 *
 * Transmit is blocking on purpose: at 115200 baud the ~1.2 kB dashboard
 * frame takes ~100 ms, which is well inside the 500 ms redraw period, and
 * a blocking write cannot tear a frame or drop a JSON line the way a
 * ring buffer overrun would.
 */

#include <stdarg.h>
#include <stdbool.h>
#include "stm32g4xx_hal.h"

void bms_io_init(UART_HandleTypeDef *huart);

void bms_print(const char *s);
void bms_printf(const char *fmt, ...);

/* Non-blocking single-character read straight from the receive data
 * register. Returns false when nothing has arrived. Also clears the
 * overrun flag, which otherwise latches after the first burst of typing
 * and wedges the receiver for good. */
bool bms_getc(char *out);

/* Formats a scaled integer as a decimal string, e.g. value 41170 with
 * unit_div 10000 and 3 decimals -> "4.117". Returns buf for convenient
 * use inside a printf argument list. Avoids pulling %f (and several kB of
 * newlib float printf) into the image. */
const char *bms_fixed(char *buf, size_t buf_len, int32_t value,
                      int32_t unit_div, uint8_t decimals);

#endif /* BMS_IO_H */
