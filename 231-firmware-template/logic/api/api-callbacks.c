#include "api/api-commands.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "firmware.h"
#include "button-task/button-task.h"
#include "led-task/led-task.h"
#include "pi-task/pi-task.h"
#include "device/device.h"
#include "profiling/profiling.h"
#include "systime/systime.h"

static bool parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || *text == '\0')
    {
        return false;
    }
    for (const char *cursor = text; *cursor != '\0'; cursor++)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return false;
        }
    }
    errno = 0;
    char *end;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || parsed > UINT32_MAX)
    {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static void cmd_info(const command_t *command)
{
    (void)command;
    device_info_t info;
    device_get_info(&info);
    printf("name: %s\r\n", FIRMWARE_NAME);
    printf("version: %s\r\n", FIRMWARE_VERSION);
    printf("project: %s\r\n", FIRMWARE_PROJECT);
    printf("repo: %s\r\n", FIRMWARE_REPO);
    printf("board: %s\r\n", info.board);
    printf("serial: %s\r\n", info.serial);
    printf("chip: manufacturer 0x%03x, part 0x%04x, revision %u\r\n",
           (unsigned)info.manufacturer, (unsigned)info.part, (unsigned)info.revision);
    printf("pico-sdk: %s\r\n", info.sdk_version);
}

static void cmd_uptime(const command_t *command)
{
    (void)command;
    printf("uptime: %llu ms\r\n", (unsigned long long)systime_ms());
}

static void print_led(void)
{
    const char *state = "off";
    switch (led_task_get_state())
    {
    case LED_STATE_OFF: break;
    case LED_STATE_ON: state = "on"; break;
    case LED_STATE_BLINK: state = "blink"; break;
    }
    printf("led: %s, period %u ms\r\n", state, (unsigned)led_task_get_period_ms());
}

static void cmd_led_enable(const command_t *command)
{
    (void)command;
    led_task_set_state(LED_STATE_ON);
    print_led();
}

static void cmd_led_disable(const command_t *command)
{
    (void)command;
    led_task_set_state(LED_STATE_OFF);
    print_led();
}

static void cmd_led_blink(const command_t *command)
{
    (void)command;
    led_task_set_state(LED_STATE_BLINK);
    print_led();
}

static void cmd_led_period(const command_t *command)
{
    uint32_t period_ms;
    if (command->argc != 1 || !parse_u32(command->argv[0], &period_ms))
    {
        printf("error: usage led_period <period_ms>\r\n");
        return;
    }
    if (!led_task_set_period_ms(period_ms))
    {
        printf("error: period_ms must be greater than 0\r\n");
        return;
    }
    print_led();
}

static void cmd_button(const command_t *command)
{
    (void)command;
    printf("button: %s, presses %u\r\n",
           button_task_is_pressed() ? "pressed" : "released",
           (unsigned)button_task_get_press_count());
}

static void cmd_pi_start(const command_t *command)
{
    uint32_t terms = 1000000;
    if (command->argc > 1 ||
        (command->argc == 1 && !parse_u32(command->argv[0], &terms)))
    {
        printf("error: usage pi_start [<terms>]\r\n");
        return;
    }
    if (terms == 0)
    {
        printf("error: terms must be greater than 0\r\n");
        return;
    }
    if (!pi_task_start(terms))
    {
        printf("error: pi is busy\r\n");
        return;
    }
    printf("pi: started, %u terms\r\n", (unsigned)terms);
}

static void cmd_pi(const command_t *command)
{
    (void)command;
    double result;
    uint64_t elapsed_ms;
    if (pi_task_get_result(&result, &elapsed_ms))
    {
        printf("pi: %.9f in %llu ms\r\n", result, (unsigned long long)elapsed_ms);
    }
    else if (pi_task_get_state() == PI_RUNNING)
    {
        printf("pi: running\r\n");
    }
    else
    {
        printf("pi: not started\r\n");
    }
}

static void cmd_profiling(const command_t *command)
{
    (void)command;
#if PROFILING_ENABLED
    for (uint32_t id = 0; id < PROFILING_STOPWATCH_COUNT; id++)
    {
        profiling_result_t result;
        if (profiling_get(id, &result))
        {
            printf("%-10s count %-10u mean %.2f us  max %u us\r\n",
                   result.name, (unsigned)result.count, result.mean_us,
                   (unsigned)result.max_us);
        }
    }
#else
    printf("profiling is disabled in this build\r\n");
#endif
}

static void cmd_profiling_reset(const command_t *command)
{
    (void)command;
    profiling_reset();
    printf("profiling reset\r\n");
}

const api_command_t api_commands[] = {
    {"info", "device passport", cmd_info},
    {"uptime", "time since reset", cmd_uptime},
    {"led_enable", "LED on", cmd_led_enable},
    {"led_disable", "LED off", cmd_led_disable},
    {"led_blink", "LED blinking", cmd_led_blink},
    {"led_period", "set period: led_period <period_ms>", cmd_led_period},
    {"button", "button state and press count", cmd_button},
    {"pi_start", "start calculation: pi_start [<terms>]", cmd_pi_start},
    {"pi", "calculation state or result", cmd_pi},
    {"profiling", "stopwatch results", cmd_profiling},
    {"profiling_reset", "reset stopwatches", cmd_profiling_reset},
};

const uint32_t api_command_count = sizeof(api_commands) / sizeof(api_commands[0]);
