#pragma once

#include "platform/platform.h"

#define UART_TASK_MAX 64u
typedef void (*uart_task_callback_t)(const uint8_t *data, size_t size, uint32_t errors);

bool uart_task_start(platform_uart_t tx, platform_uart_t rx, const uint8_t *data, size_t size,
                     uart_task_callback_t on_done);
bool uart_task_busy(void);
void uart_task_handle(void);
