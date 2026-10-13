#include <stddef.h>
#include "firmware.h"
#include "platform/platform.h"
#include "api/api.h"
#include "button-task/button-task.h"
#include "led-task/led-task.h"
#include "pi-task/pi-task.h"
#include "logging/log.h"
#include "profiling/profiling.h"
#include "stdio-text-protocol/stdio-text-protocol.h"

typedef enum
{
    STOPWATCH_LOOP,
    STOPWATCH_COMMAND,
    STOPWATCH_LED,
    STOPWATCH_BUTTON,
    STOPWATCH_PI,
} stopwatch_id_t;

int main(void)
{
    platform_init();
    LOG_INF("%s %s\r\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    profiling_stopwatch_init(STOPWATCH_LOOP, "loop");
    profiling_stopwatch_init(STOPWATCH_COMMAND, "command");
    profiling_stopwatch_init(STOPWATCH_LED, "led");
    profiling_stopwatch_init(STOPWATCH_BUTTON, "button");
    profiling_stopwatch_init(STOPWATCH_PI, "pi");
    stdio_text_protocol_init();
    led_task_init();
    button_task_init(led_task_next_state);
    pi_task_init();

    while (1)
    {
        profiling_start(STOPWATCH_LOOP);
        profiling_start(STOPWATCH_COMMAND);
        const command_t *command = stdio_text_protocol_handle();
        if (command != NULL)
        {
            api_handle(command);
        }
        profiling_stop(STOPWATCH_COMMAND);
        profiling_start(STOPWATCH_LED);
        led_task_handle();
        profiling_stop(STOPWATCH_LED);
        profiling_start(STOPWATCH_BUTTON);
        button_task_handle();
        profiling_stop(STOPWATCH_BUTTON);
        profiling_start(STOPWATCH_PI);
        pi_task_handle();
        profiling_stop(STOPWATCH_PI);
        profiling_stop(STOPWATCH_LOOP);
    }
}
