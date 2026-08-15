#ifndef BMS_IO_H
#define BMS_IO_H

/*
 * Interrupt-driven console over the ST-Link Virtual COM Port.
 *
 * On NUCLEO-G474RE the VCP is wired to LPUART1 (PA2/PA3, AF12) -- NOT to
 * USART2 as on the older Nucleo-64 boards, and NOT to USART1, which the
 * board routes to the Arduino D0/D1 pins (PC4/PC5) instead. See UM2505
 * section 6.6.5. Pass whichever handle CubeMX generated for LPUART1.
 *
 * Both directions are ring-buffered and drained from LPUART1_IRQHandler
 * (defined in bms_io.c, claiming the weak vector). Transmit used to block,
 * which is fine for a dedicated monitor and not fine sharing a loop with
 * motor control: one ~1.3 kB dashboard frame is over 100 ms at 115200,
 * spent inside HAL_UART_Transmit and not servicing the drive watchdog.
 */

#include <stdarg.h>
#include <stdbool.h>
#include "stm32g4xx_hal.h"

void bms_io_init(UART_HandleTypeDef *huart);

/* Both queue into an interrupt-drained ring and return immediately -- safe
 * to call from a loop that also has real-time work to do. A message that
 * does not fit is dropped whole rather than truncated.
 *
 * bms_printf() formats through a 256-byte buffer and drops the overflow,
 * so keep one call to one screen line or two; longer fixed text belongs in
 * bms_print(), which has no such limit. The format attribute makes the
 * compiler check the arguments -- this console is how the bench is
 * debugged, and a message that lies about a register or a pin costs more
 * time than it saves. */
void bms_print(const char *s);
#if defined(__GNUC__)
void bms_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void bms_printf(const char *fmt, ...);
#endif

/* How many messages have been dropped for want of ring space. Non-zero
 * means the console is being asked to say more than the link can carry. */
uint32_t bms_io_dropped(void);

/* Non-blocking single-character read from the receive ring the RXNE
 * interrupt fills. Returns false when nothing has arrived. The interrupt
 * also clears the overrun flag, which otherwise latches after the first
 * burst of typing and wedges the receiver for good. */
bool bms_getc(char *out);

/* Formats a scaled integer as a decimal string, e.g. value 41170 with
 * unit_div 10000 and 3 decimals -> "4.117". Returns buf for convenient
 * use inside a printf argument list. Avoids pulling %f (and several kB of
 * newlib float printf) into the image. */
const char *bms_fixed(char *buf, size_t buf_len, int32_t value,
                      int32_t unit_div, uint8_t decimals);

#endif /* BMS_IO_H */
