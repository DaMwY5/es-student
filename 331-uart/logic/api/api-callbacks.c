#include "api/api-commands.h"

#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware.h"
#include "button-task/button-task.h"
#include "led-task/led-task.h"
#include "pi-task/pi-task.h"
#include "device/device.h"
#include "profiling/profiling.h"
#include "systime/systime.h"
#include "block-task/block-task.h"
#include "edge-task/edge-task.h"
#include "uart-task/uart-task.h"

#define TEXT_MAX 32u
#define SCOPE_TEXT_MAX 4u
#define SCOPE_BAUD_MIN 9600u
#define SCOPE_PRETRIGGER_US 24u

static const uint8_t text_line[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
static uint8_t block[UART_TASK_MAX];
static bool block_irq;
static bool internal_loop;
static platform_parity_t configured_parities[2];
static struct {
    bool active, tx_started, rx_done;
    uint32_t baud, errors;
    uint64_t trigger_us;
    uint8_t text[SCOPE_TEXT_MAX], received[SCOPE_TEXT_MAX];
    size_t text_size, received_size;
    const uint16_t *samples;
    uint32_t sample_count;
} scope;

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

static void print_bytes(const uint8_t *data, size_t size)
{
    if (!size)
    {
        printf("(empty)");
        return;
    }
    char printable[UART_TASK_MAX + 1];
    for (size_t i = 0; i < size; ++i)
        printable[i] = isprint(data[i]) ? (char)data[i] : '.';
    printable[size] = '\0';
    printf("%s", printable);
}

static void print_errors(uint32_t errors)
{
    printf("err:");
    if (!errors) printf(" none");
    if (errors & PLATFORM_UART_FE) printf(" FE");
    if (errors & PLATFORM_UART_PE) printf(" PE");
    if (errors & PLATFORM_UART_BE) printf(" BE");
    if (errors & PLATFORM_UART_OE) printf(" OE");
    printf("\r\n");
}

static bool check_uart_idle(void)
{
    if (!scope.active && !uart_task_busy()) return true;
    printf("error: uart is busy\r\n");
    return false;
}

static void print_edge(platform_line_mode_t mode, bool ok, uint32_t rise_us, uint32_t fall_us)
{
    const char *label = mode == PLATFORM_LINE_PUSH_PULL ? "pp" : "od";
    if (ok)
        printf("edge %s: rise %.1f ms, fall %.1f ms\r\n", label, rise_us / 1000.0, fall_us / 1000.0);
    else
        printf("edge %s: timeout\r\n", label);
}

static void cmd_edge(const command_t *command)
{
    if (command->argc != 1 ||
        (strcmp(command->argv[0], "pp") != 0 && strcmp(command->argv[0], "od") != 0))
    {
        printf("error: usage edge pp|od\r\n");
        return;
    }
    if (scope.active)
    {
        printf("error: adc busy\r\n");
        return;
    }
    platform_line_mode_t mode = strcmp(command->argv[0], "pp") == 0
        ? PLATFORM_LINE_PUSH_PULL : PLATFORM_LINE_OPEN_DRAIN;
    if (!edge_task_start(mode, print_edge)) printf("error: edge is busy\r\n");
}

static void cmd_loop(const command_t *command)
{
    if (command->argc != 1 ||
        (strcmp(command->argv[0], "internal") != 0 && strcmp(command->argv[0], "pins") != 0))
    {
        printf("error: usage loop internal|pins\r\n");
        return;
    }
    if (!check_uart_idle()) return;
    internal_loop = strcmp(command->argv[0], "internal") == 0;
    platform_uart_loop_set(internal_loop);
    printf("loop: %s\r\n", command->argv[0]);
}

static void print_uart(const char *prefix, const uint8_t *data, size_t size, uint32_t errors)
{
    printf("%s", prefix);
    print_bytes(data, size);
    printf(" ");
    print_errors(errors);
}

static void print_echo(const uint8_t *data, size_t size, uint32_t errors)
{
    print_uart("rx: ", data, size, errors);
}

static void print_link_back(const uint8_t *data, size_t size, uint32_t errors)
{
    print_uart("1->0: ", data, size, errors);
}

static void print_link_forth(const uint8_t *data, size_t size, uint32_t errors)
{
    print_uart("0->1: ", data, size, errors);
    if (size) uart_task_start(PLATFORM_UART1, PLATFORM_UART0, data, size, print_link_back);
}

static void start_text(const command_t *command, const char *usage, platform_uart_t rx,
                       uart_task_callback_t report)
{
    if (command->argc != 1)
    {
        printf("error: usage %s\r\n", usage);
        return;
    }
    const char *text = command->argv[0];
    size_t size = strlen(text);
    if (size > TEXT_MAX)
    {
        printf("error: text longer than 32\r\n");
        return;
    }
    if (check_uart_idle())
        uart_task_start(PLATFORM_UART0, rx, (const uint8_t *)text, size, report);
}

static void cmd_echo(const command_t *command)
{
    start_text(command, "echo <text>", PLATFORM_UART0, print_echo);
}

static void cmd_link(const command_t *command)
{
    start_text(command, "link <text>", PLATFORM_UART1, print_link_forth);
}

static void cmd_baud_shift(const command_t *command)
{
    uint32_t magnitude;
    const char *arg = command->argc == 1 ? command->argv[0] : "";
    bool negative = *arg == '-';
    if (!parse_u32(arg + (negative || *arg == '+'), &magnitude))
    {
        printf("error: usage baud_shift <percent>\r\n");
        return;
    }
    if (magnitude > 50)
    {
        printf("error: percent must be -50..50\r\n");
        return;
    }
    if (!check_uart_idle()) return;
    uint32_t percent = negative ? 100 - magnitude : 100 + magnitude;
    uint32_t actual = platform_uart_baud_set(PLATFORM_UART1, PLATFORM_UART_BAUD * percent / 100);
    printf("baud_shift: uart1 %u baud\r\n", (unsigned)actual);
}

static void print_text(const uint8_t *data, size_t size, uint32_t errors)
{
    printf("text: %u chars ", (unsigned)size);
    print_uart("", data, size, errors);
}

static void cmd_text(const command_t *command)
{
    (void)command;
    if (check_uart_idle())
        uart_task_start(PLATFORM_UART1, PLATFORM_UART0, text_line, sizeof text_line - 1, print_text);
}

static bool parse_parity(const char *name, platform_parity_t *parity)
{
    if (strcmp(name, "none") == 0) *parity = PLATFORM_PARITY_NONE;
    else if (strcmp(name, "even") == 0) *parity = PLATFORM_PARITY_EVEN;
    else if (strcmp(name, "odd") == 0) *parity = PLATFORM_PARITY_ODD;
    else return false;
    return true;
}

static void cmd_parity(const command_t *command)
{
    platform_parity_t p0, p1;
    if (command->argc != 2 || !parse_parity(command->argv[0], &p0) ||
        !parse_parity(command->argv[1], &p1))
    {
        printf("error: usage parity none|even|odd none|even|odd\r\n");
        return;
    }
    if (!check_uart_idle()) return;
    platform_uart_parity_set(PLATFORM_UART0, p0);
    platform_uart_parity_set(PLATFORM_UART1, p1);
    configured_parities[PLATFORM_UART0] = p0;
    configured_parities[PLATFORM_UART1] = p1;
    printf("parity: uart0 %s, uart1 %s\r\n", command->argv[0], command->argv[1]);
}

static void print_block(const uint8_t *data, size_t size, uint32_t errors)
{
    platform_uart_rx_irq_set(false);
    bool match = size != 0 && memcmp(data, block, size) == 0;
    printf("block %s: %u/%u %s, ", block_irq ? "irq" : "poll",
           (unsigned)size, (unsigned)sizeof block, match ? "match" : "mismatch");
    print_errors(errors);
}

static void cmd_block(const command_t *command)
{
    if (command->argc != 1 ||
        (strcmp(command->argv[0], "irq") != 0 && strcmp(command->argv[0], "poll") != 0))
    {
        printf("error: usage block irq|poll\r\n");
        return;
    }
    if (!check_uart_idle()) return;
    block_irq = strcmp(command->argv[0], "irq") == 0;
    for (size_t i = 0; i < sizeof block; ++i) block[i] = (uint8_t)i;
    if (uart_task_start(PLATFORM_UART1, PLATFORM_UART0, block, sizeof block, print_block))
        platform_uart_rx_irq_set(block_irq);
}

static void save_scope_rx(const uint8_t *data, size_t size, uint32_t errors)
{
    scope.received_size = size < sizeof scope.received ? size : sizeof scope.received;
    memcpy(scope.received, data, scope.received_size);
    scope.errors = errors;
    scope.rx_done = true;
}

static void save_scope_block(const uint16_t *samples, uint32_t count, uint32_t elapsed_us)
{
    (void)elapsed_us;
    scope.samples = samples;
    scope.sample_count = count;
}

static void restore_uart_after_scope(void)
{
    platform_uart_baud_set(PLATFORM_UART0, PLATFORM_UART_BAUD);
    platform_uart_baud_set(PLATFORM_UART1, PLATFORM_UART_BAUD);
    platform_uart_parity_set(PLATFORM_UART0, configured_parities[PLATFORM_UART0]);
    platform_uart_parity_set(PLATFORM_UART1, configured_parities[PLATFORM_UART1]);
    platform_uart_loop_set(internal_loop);
}

void api_scope_handle(void)
{
    if (!scope.active) return;
    if (!scope.tx_started && systime_us() >= scope.trigger_us)
    {
        scope.tx_started = uart_task_start(PLATFORM_UART0, PLATFORM_UART1,
                                          scope.text, scope.text_size, save_scope_rx);
    }
    if (!scope.samples || !scope.rx_done) return;

    restore_uart_after_scope();
    printf("scope: %u samples, %u Hz, %u baud, uart1 ", (unsigned)scope.sample_count,
           (unsigned)PLATFORM_SAMPLE_RATE_HZ, (unsigned)scope.baud);
    print_uart("", scope.received, scope.received_size, scope.errors);
    for (uint32_t offset = 0; offset < scope.sample_count; offset += 16)
    {
        char line[16 * 5 + 1];
        size_t length = 0;
        for (uint32_t i = 0; i < 16 && offset + i < scope.sample_count; ++i)
        {
            length += (size_t)snprintf(line + length, sizeof line - length,
                                       i ? " %u" : "%u", (unsigned)scope.samples[offset + i]);
        }
        printf("%s\r\n", line);
    }
    printf("scope: end\r\n");
    scope.active = false;
}

static void cmd_scope(const command_t *command)
{
    uint32_t baud;
    if (command->argc != 2 || !parse_u32(command->argv[0], &baud) ||
        baud < SCOPE_BAUD_MIN || baud > PLATFORM_UART_BAUD ||
        strlen(command->argv[1]) > SCOPE_TEXT_MAX)
    {
        printf("error: usage scope <baud> <text>\r\n");
        return;
    }
    if (!check_uart_idle()) return;
    if (edge_task_busy())
    {
        printf("error: adc busy\r\n");
        return;
    }
    platform_uart_loop_set(false);
    platform_uart_baud_set(PLATFORM_UART0, baud);
    platform_uart_baud_set(PLATFORM_UART1, baud);
    platform_uart_parity_set(PLATFORM_UART0, PLATFORM_PARITY_NONE);
    platform_uart_parity_set(PLATFORM_UART1, PLATFORM_PARITY_NONE);
    if (!block_task_start(PLATFORM_BLOCK_MAX, save_scope_block))
    {
        restore_uart_after_scope();
        printf("error: adc busy\r\n");
        return;
    }
    scope.active = true;
    scope.tx_started = scope.rx_done = false;
    scope.samples = NULL;
    scope.baud = baud;
    scope.text_size = strlen(command->argv[1]);
    memcpy(scope.text, command->argv[1], scope.text_size);
    scope.trigger_us = systime_us() + SCOPE_PRETRIGGER_US;
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
    {"edge", "rise and fall of the RC line: edge pp|od", cmd_edge},
    {"loop", "UART0 loopback: loop internal|pins", cmd_loop},
    {"echo", "send and receive with UART0: echo <text>", cmd_echo},
    {"link", "UART0 to UART1 and back: link <text>", cmd_link},
    {"baud_shift", "UART1 baud offset: baud_shift <percent>", cmd_baud_shift},
    {"text", "send 32 characters from UART1 to UART0", cmd_text},
    {"parity", "UART0 and UART1 parity: none, even, odd", cmd_parity},
    {"block", "receive 64 bytes: block irq|poll", cmd_block},
    {"scope", "UART0 line on ADC: scope <baud> <text>", cmd_scope},
};

const uint32_t api_command_count = sizeof(api_commands) / sizeof(api_commands[0]);
