#include "uart-task/uart-task.h"
#include "systime/systime.h"
#include <string.h>

#define QUIET_US 1000u

typedef enum { UART_STATE_IDLE, UART_STATE_SEND, UART_STATE_QUIET } uart_state_t;
static uart_state_t state;
static platform_uart_t tx_uart, rx_uart;
static uint8_t tx_data[UART_TASK_MAX];
static size_t tx_size, tx_sent;
static uint64_t quiet_start_us;
static uart_task_callback_t callback;

bool uart_task_start(platform_uart_t tx, platform_uart_t rx, const uint8_t *data, size_t size,
                     uart_task_callback_t on_done)
{
    if (state != UART_STATE_IDLE || !data || !size || size > sizeof tx_data || !on_done)
        return false;
    platform_uart_clear(rx);
    tx_uart = tx;
    rx_uart = rx;
    memcpy(tx_data, data, size);
    tx_size = size;
    tx_sent = 0;
    callback = on_done;
    state = UART_STATE_SEND;
    return true;
}

bool uart_task_busy(void) { return state != UART_STATE_IDLE; }

void uart_task_handle(void)
{
    switch (state)
    {
    case UART_STATE_IDLE:
        break;
    case UART_STATE_SEND:
        while (tx_sent < tx_size && platform_uart_put(tx_uart, tx_data[tx_sent]))
            ++tx_sent;
        if (tx_sent == tx_size && !platform_uart_busy(tx_uart))
        {
            quiet_start_us = systime_us();
            state = UART_STATE_QUIET;
        }
        break;
    case UART_STATE_QUIET:
        if (systime_us() - quiet_start_us >= QUIET_US)
        {
            uint8_t received[UART_TASK_MAX];
            size_t size = platform_uart_read(rx_uart, received, sizeof received);
            uint32_t errors = platform_uart_errors(rx_uart);
            state = UART_STATE_IDLE;
            callback(received, size, errors);
        }
        break;
    }
}
