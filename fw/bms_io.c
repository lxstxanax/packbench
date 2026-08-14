#include "bms_io.h"
#include <stdio.h>
#include <string.h>

static UART_HandleTypeDef *s_huart = NULL;

/* One frame of the dashboard is emitted line by line, so this only has to
 * hold the longest single line. */
static char s_buf[256];

/*
 * Receive ring, filled from the RXNE interrupt.
 *
 * Polling the receive register from the main loop cannot work here: the
 * loop blocks for ~100 ms while a dashboard frame goes out at 115200, and
 * the register holds exactly one byte. Two characters sent back to back
 * arrive 87 us apart, so the second overwrites the first and one of them
 * is simply gone. That silently ate the 'j' in a host tool's "ESC then j"
 * handshake, leaving the JSON stream never started and the GUI blank.
 */
#define RX_RING_SIZE 64u
static volatile uint8_t  s_rx[RX_RING_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

void bms_io_init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
    s_rx_head = 0u;
    s_rx_tail = 0u;

    if (huart == NULL) {
        return;
    }

    /* The generated project never enables this interrupt -- the console
     * UART comes from the board BSP, which only sets up polled transfers.
     * The vector itself is weak in the startup file, so defining the
     * handler below claims it without touching any generated code. */
    __HAL_UART_ENABLE_IT(huart, UART_IT_RXNE);
    HAL_NVIC_SetPriority(LPUART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(LPUART1_IRQn);
}

void LPUART1_IRQHandler(void)
{
    UART_HandleTypeDef *huart = s_huart;
    uint16_t next;

    if (huart == NULL) {
        return;
    }

    /* An overrun latches ORE and stops RXNE from ever setting again, so
     * it has to be cleared even though the byte behind it is lost. */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(huart);
    }

    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE)) {
        uint8_t byte = (uint8_t)(huart->Instance->RDR & 0xFFu);

        next = (uint16_t)((s_rx_head + 1u) % RX_RING_SIZE);
        if (next != s_rx_tail) {        /* full ring drops, never overwrites */
            s_rx[s_rx_head] = byte;
            s_rx_head = next;
        }
    }
}

void bms_print(const char *s)
{
    if ((s_huart == NULL) || (s == NULL)) {
        return;
    }
    (void)HAL_UART_Transmit(s_huart, (const uint8_t *)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

void bms_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (s_huart == NULL) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(s_buf, sizeof(s_buf), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(s_buf)) {
        n = (int)sizeof(s_buf) - 1;   /* truncated, send what fits */
    }
    (void)HAL_UART_Transmit(s_huart, (const uint8_t *)s_buf, (uint16_t)n, HAL_MAX_DELAY);
}

bool bms_getc(char *out)
{
    if (out == NULL) {
        return false;
    }
    if (s_rx_head == s_rx_tail) {
        return false;
    }

    *out = (char)s_rx[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) % RX_RING_SIZE);
    return true;
}

const char *bms_fixed(char *buf, size_t buf_len, int32_t value,
                      int32_t unit_div, uint8_t decimals)
{
    bool neg;
    int32_t magnitude, int_part, frac_part, power;
    uint8_t i;

    if ((buf == NULL) || (buf_len == 0u)) {
        return "";
    }
    if (unit_div <= 0) {
        (void)snprintf(buf, buf_len, "?");
        return buf;
    }

    neg       = (value < 0);
    magnitude = neg ? -value : value;
    int_part  = magnitude / unit_div;
    frac_part = magnitude % unit_div;

    power = 1;
    for (i = 0u; i < decimals; i++) {
        power *= 10;
    }
    frac_part = (frac_part * power) / unit_div;

    if (decimals == 0u) {
        (void)snprintf(buf, buf_len, "%s%ld", neg ? "-" : "", (long)int_part);
    } else {
        (void)snprintf(buf, buf_len, "%s%ld.%0*ld", neg ? "-" : "",
                       (long)int_part, (int)decimals, (long)frac_part);
    }
    return buf;
}
