#include "api/api-commands.h"
#include "api/api.h"
#include <math.h>
#include "block-task/block-task.h"

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
#include "platform/platform.h"

#define TELEMETRY_DEFAULT_PERIOD_MS 100
#define STREAM_BLINK_PERIOD_MS 250
#define MIC_DEFAULT_WINDOW_MS 100

static bool superloop_halted;
static uint32_t mic_window_ms;

void api_init(void) { superloop_halted = false; }
bool api_superloop_halted(void) { return superloop_halted; }

static bool input_reserved(void)
{
    return platform_adc_busy() || telemetry_task_get_state() == TELEMETRY_STATE_RUN;
}

static void print_busy(void) { printf("error: adc busy\r\n"); }

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

static void cmd_get_voltage(const command_t *command)
{
    (void)command;
    if (platform_adc_busy()) { print_busy(); return; }
    printf("voltage: %.3f V\r\n", (double)platform_voltage_read());
}

static void cmd_get_temp(const command_t *command)
{
    (void)command;
    if (platform_adc_busy()) { print_busy(); return; }
    printf("temp: %.1f C\r\n", (double)platform_temp_read());
}

void api_print_telemetry(const telemetry_record_t *record)
{
    if (record != NULL)
    {
        printf("tm: %llu %.3f %.1f\r\n", (unsigned long long)record->time_us,
               (double)record->voltage_v, (double)record->temp_c);
    }
}

static void cmd_tm_start(const command_t *command)
{
    uint32_t period_ms = TELEMETRY_DEFAULT_PERIOD_MS;
    if (command->argc > 1 ||
        (command->argc == 1 && !parse_u32(command->argv[0], &period_ms)))
    {
        printf("error: usage tm_start [<period_ms>]\r\n");
        return;
    }
    if (platform_adc_busy()) { print_busy(); return; }
    if (!telemetry_task_start(period_ms))
    {
        printf("error: period_ms must be at least 10\r\n");
        return;
    }
    printf("telemetry: started, period %u ms\r\n", (unsigned)period_ms);
}

static void cmd_tm_stop(const command_t *command)
{
    (void)command;
    telemetry_task_stop();
    printf("telemetry: stopped\r\n");
}

static void cmd_charge_on(const command_t *command)
{
    (void)command;
    platform_charge_set(true);
    printf("charge: on\r\n");
}

static void cmd_charge_off(const command_t *command)
{
    (void)command;
    platform_charge_set(false);
    printf("charge: off\r\n");
}

static void cmd_timer_blink_start(const command_t *command)
{
    uint32_t period_ms;
    if (command->argc != 1 || !parse_u32(command->argv[0], &period_ms))
    {
        printf("error: usage timer_blink_start <period_ms>\r\n");
        return;
    }
    if (period_ms < 10 || period_ms > 60000)
    {
        printf("error: period_ms must be 10..60000\r\n");
        return;
    }
    if (stream_task_get_state() == STREAM_STATE_RUN)
    {
        printf("error: recording indicator in use\r\n");
        return;
    }
    if (!platform_blink_start(period_ms))
    {
        printf("error: timer unavailable\r\n");
        return;
    }
    led_task_set_state(LED_STATE_OFF);
    printf("timer_blink: started, period %u ms\r\n", (unsigned)period_ms);
}

static void cmd_timer_blink_stop(const command_t *command)
{
    (void)command;
    if (stream_task_get_state() == STREAM_STATE_RUN)
    {
        printf("error: recording indicator in use\r\n");
        return;
    }
    printf("timer_blink: stopped, %u toggles\r\n", (unsigned)platform_blink_stop());
}

static void cmd_cap_start(const command_t *command)
{
    uint32_t count, period_us;
    if (command->argc != 2 || !parse_u32(command->argv[0], &count) ||
        !parse_u32(command->argv[1], &period_us))
    {
        printf("error: usage cap_start <N> <period_us>\r\n");
        return;
    }
    if (!count || count > PLATFORM_CAPTURE_MAX || period_us < 10)
    {
        printf("error: N must be 1..2000, period_us at least 10\r\n");
        return;
    }
    if (input_reserved() || !platform_capture_start(count, period_us)) { print_busy(); return; }
    printf("capture: started, %u samples, period %u us\r\n",
           (unsigned)count, (unsigned)period_us);
}

static void cmd_cap_dump(const command_t *command)
{
    (void)command;
    if (stream_task_get_state() == STREAM_STATE_RUN) { print_busy(); return; }
    if (!platform_capture_done())
    {
        printf("error: no capture\r\n");
        return;
    }
    platform_capture_t capture;
    platform_capture_get(&capture);
    printf("capture: %u samples, period %u us, interval %u..%u us, handler %.1f %%\r\n",
           (unsigned)capture.count, (unsigned)capture.period_us,
           (unsigned)capture.interval_min_us, (unsigned)capture.interval_max_us,
           (double)capture.handler_percent);
    for (uint32_t i = 0; i < capture.count; ++i)
    {
        if (i % 16 == 0) printf("cap:");
        printf(" %u", (unsigned)capture.samples[i]);
        if (i % 16 == 15 || i + 1 == capture.count) printf("\r\n");
    }
}

static void report_block(const uint16_t *samples, uint32_t count, uint32_t elapsed)
{
    uint32_t sum = 0;
    uint16_t min = samples[0], max = min;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint16_t code = samples[i];
        if (code < min) min = code;
        if (code > max) max = code;
        sum += code;
    }
    uint32_t expected = (uint32_t)((uint64_t)count * 1000000 / PLATFORM_SAMPLE_RATE_HZ);
    printf("dma_cap: %u samples in %u us, expected %u us, min %u max %u mean %.1f\r\n",
           (unsigned)count, (unsigned)elapsed, (unsigned)expected,
           (unsigned)min, (unsigned)max, (double)sum / count);
}

static void cmd_dma_cap(const command_t *command)
{
    uint32_t count;
    if (command->argc != 1 || !parse_u32(command->argv[0], &count))
    {
        printf("error: usage dma_cap <N>\r\n");
        return;
    }
    if (count == 0 || count > PLATFORM_BLOCK_MAX)
    {
        printf("error: N must be 1..8192\r\n");
        return;
    }
    if (input_reserved() || !block_task_start(count, report_block)) print_busy();
}

void api_send_frame(const stream_header_t *header, const uint8_t *data, uint32_t bytes)
{
    platform_write(header, sizeof(*header));
    platform_write(data, bytes);
}

static void cmd_stream_start(const command_t *command)
{
    uint32_t bits = 16;
    if (command->argc > 1 ||
        (command->argc == 1 && !parse_u32(command->argv[0], &bits)) ||
        (bits != 8 && bits != 16))
    {
        printf("error: usage stream_start [8|16]\r\n");
        return;
    }
    if (input_reserved() || !stream_task_start((uint8_t)bits)) { print_busy(); return; }
    if (!platform_blink_start(STREAM_BLINK_PERIOD_MS))
    {
        stream_task_stop();
        printf("error: timer unavailable\r\n");
        return;
    }
    led_task_set_state(LED_STATE_OFF);
    printf("stream: started, 44100 Hz, %u bit\r\n", (unsigned)bits);
}

static void cmd_stream_stop(const command_t *command)
{
    (void)command;
    if (stream_task_get_state() == STREAM_STATE_RUN)
    {
        stream_task_stop();
        platform_blink_stop();
    }
    printf("stream: stopped, %u frames, %u samples, %u overruns\r\n",
           (unsigned)stream_task_get_frames(), (unsigned)stream_task_get_samples(),
           (unsigned)stream_task_get_overruns());
}

static void cmd_generate_tone(const command_t *command)
{
    uint32_t freq;
    if (command->argc != 1 || !parse_u32(command->argv[0], &freq))
        printf("error: usage generate_tone <Hz>\r\n");
    else if (!platform_tone_set(freq))
        printf("error: Hz must be 0 or 20..20000\r\n");
    else if (freq)
        printf("tone: %u Hz\r\n", (unsigned)freq);
    else
        printf("tone: off\r\n");
}

static void cmd_wdt_enable(const command_t *command)
{
    uint32_t timeout_ms;
    if (command->argc != 1 || !parse_u32(command->argv[0], &timeout_ms))
        printf("error: usage wdt_enable <timeout_ms>\r\n");
    else if (!platform_watchdog_enable(timeout_ms))
        printf("error: timeout_ms must be 1..8388\r\n");
    else
        printf("wdt: enabled, timeout %u ms\r\n", (unsigned)timeout_ms);
}

static void cmd_hang(const command_t *command)
{
    (void)command;
    printf("hang: superloop stopped\r\n");
    superloop_halted = true;
}

static void cmd_reset_reason(const command_t *command)
{
    (void)command;
    uint32_t restarts;
    bool watchdog = platform_watchdog_caused_reset(&restarts);
    printf("reset: %s, restarts %u\r\n", watchdog ? "watchdog" : "other",
           (unsigned)restarts);
}

static void report_bias(const uint16_t *samples, uint32_t count, uint32_t elapsed)
{
    (void)elapsed;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) sum += samples[i];
    printf("mic: bias %.3f V\r\n", (double)platform_adc_volts((float)sum / count));
}

static void report_level(const uint16_t *samples, uint32_t count, uint32_t elapsed)
{
    (void)elapsed;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) sum += samples[i];
    float mean = (float)sum / count;
    float squares = 0, peak = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        float magnitude = fabsf(samples[i] - mean);
        squares += magnitude * magnitude;
        if (magnitude > peak) peak = magnitude;
    }
    float rms = sqrtf(squares / count);
    printf("mic: level %.1f mV rms, peak %.1f mV, %u ms\r\n",
           1000.0 * platform_adc_volts(rms), 1000.0 * platform_adc_volts(peak),
           (unsigned)mic_window_ms);
}

static void cmd_mic_bias(const command_t *command)
{
    (void)command;
    if (input_reserved() ||
        !block_task_start(PLATFORM_SAMPLE_RATE_HZ * MIC_DEFAULT_WINDOW_MS / 1000,
                          report_bias)) print_busy();
}

static void cmd_mic_level(const command_t *command)
{
    uint32_t ms = MIC_DEFAULT_WINDOW_MS;
    if (command->argc > 1 ||
        (command->argc == 1 && !parse_u32(command->argv[0], &ms)))
    {
        printf("error: usage mic_level [<ms>]\r\n");
        return;
    }
    if (ms < 1 || ms > 185)
    {
        printf("error: ms must be 1..185\r\n");
        return;
    }
    if (input_reserved() || !block_task_start(PLATFORM_SAMPLE_RATE_HZ * ms / 1000, report_level))
    {
        print_busy();
        return;
    }
    mic_window_ms = ms;
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
    {"get_voltage", "voltage on GP26, ADC input 0", cmd_get_voltage},
    {"get_temp", "chip temperature, degrees Celsius", cmd_get_temp},
    {"tm_start", "start telemetry: tm_start [<period_ms>]", cmd_tm_start},
    {"tm_stop", "stop telemetry", cmd_tm_stop},
    {"charge_on", "charge RC circuit from GP22", cmd_charge_on},
    {"charge_off", "discharge RC circuit through GP22", cmd_charge_off},
    {"timer_blink_start", "recording indicator period in ms", cmd_timer_blink_start},
    {"timer_blink_stop", "stop timer indicator and count toggles", cmd_timer_blink_stop},
    {"cap_start", "timer capture: cap_start <N> <period_us>", cmd_cap_start},
    {"cap_dump", "dump timer capture and timing statistics", cmd_cap_dump},
    {"dma_cap", "capture a block at 44100 Hz: dma_cap <N>", cmd_dma_cap},
    {"stream_start", "start audio stream: stream_start [8|16]", cmd_stream_start},
    {"stream_stop", "stop audio stream and report counters", cmd_stream_stop},
    {"generate_tone", "GP20 PWM tone: generate_tone <Hz>", cmd_generate_tone},
    {"wdt_enable", "enable watchdog: wdt_enable <timeout_ms>", cmd_wdt_enable},
    {"hang", "stop superloop to test the watchdog", cmd_hang},
    {"reset_reason", "reason and consecutive watchdog restarts", cmd_reset_reason},
    {"mic_bias", "microphone DC bias in volts", cmd_mic_bias},
    {"mic_level", "microphone level: mic_level [<ms>]", cmd_mic_level},
};

const uint32_t api_command_count = sizeof(api_commands) / sizeof(api_commands[0]);
