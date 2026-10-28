// Программная модель: исполняет настоящий код C, но часы, USB и GPIO подставные.
// Это не эмулятор RP2040 и не аппаратные измерения.
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "platform/platform.h"
#include "api/api.h"
#include "button-task/button-task.h"
#include "led-task/led-task.h"
#include "profiling/profiling.h"
#include "systime/systime.h"
#include "telemetry-task/telemetry-task.h"
// Белый ящик только для проверки последней порции возле UINT32_MAX.
#include "../../311-adc/logic/pi-task/pi-task.c"

#undef printf
#undef vprintf
#undef putchar

uint32_t host_chip_id = 0x20002927;
static uint64_t now_us;
static int input_symbol = PICO_ERROR_TIMEOUT;
static bool pins[30];
static bool outputs[30];
static bool pulls[30];
static bool adc_enabled;
static bool sensor_enabled;
static uint analog_pin;
static uint adc_channel;
static uint16_t adc_codes[5] = {2048, 0, 0, 0, 876};
static uint32_t adc_reads;
static bool model_inputs;
static bool rc_model;
static double capacitor_v;
static uint64_t capacitor_updated_us;
static bool record_lines;
static bool charge_output;
static char received[16384];
static size_t received_length;
static char pending[2048];
static size_t pending_length;

uint64_t time_us_64(void) { return now_us; }
uint32_t time_us_32(void) { return (uint32_t)now_us; }
void stdio_init_all(void) {}
void gpio_init(uint pin) { assert(pin < 30); }
void gpio_set_dir(uint pin, bool out) { outputs[pin] = out; }
static void update_capacitor(void)
{
    double target_v = pins[22] ? 3.3 : 0.0;
    double elapsed_s = (now_us - capacitor_updated_us) / 1e6;
    // Идеальная модель R=10 кОм, C=100 мкФ, tau=1 с; это не опыт.
    capacitor_v = target_v + (capacitor_v - target_v) * exp(-elapsed_s);
    capacitor_updated_us = now_us;
}

void gpio_put(uint pin, bool on)
{
    if (pin == 22 && rc_model) update_capacitor();
    pins[pin] = on;
}
bool gpio_get(uint pin) { return pins[pin]; }
void gpio_pull_up(uint pin) { pulls[pin] = true; pins[pin] = true; }
void adc_init(void) { adc_enabled = true; }
void adc_gpio_init(uint pin) { assert(pin == 26); analog_pin = pin; }
void adc_set_temp_sensor_enabled(bool enabled) { sensor_enabled = enabled; }
void adc_select_input(uint input) { assert(input < 5); adc_channel = input; }
uint16_t adc_read(void)
{
    assert(adc_enabled);
    adc_reads++;
    if (adc_channel == 4)
    {
        assert(sensor_enabled);
        return model_inputs ? (uint16_t)(876 + (now_us / 100000) % 2) : adc_codes[4];
    }
    assert(adc_channel == 0 && analog_pin == 26);
    if (rc_model)
    {
        update_capacitor();
        double code = floor(capacitor_v * 4096 / 3.3);
        if (code < 0) code = 0;
        if (code > 4095) code = 4095;
        return (uint16_t)code;
    }
    return model_inputs ? (uint16_t)(2048 + 8 * sin(now_us / 1e6)) : adc_codes[0];
}
int getchar_timeout_us(uint32_t timeout)
{
    assert(timeout == 0);
    int symbol = input_symbol;
    input_symbol = PICO_ERROR_TIMEOUT;
    return symbol;
}
void pico_get_unique_board_id_string(char *serial, size_t size)
{
    assert(size >= 17);
    memcpy(serial, "0000000000000000", 17);
}

static void print_hex(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        fprintf(stdout, "%02x", (unsigned char)text[i]);
    }
}

int host_putchar(int symbol)
{
    if (charge_output) now_us++; // Явная модель: 1 мкс на выводимый символ.
    assert(received_length + 1 < sizeof(received));
    received[received_length++] = (char)symbol;
    received[received_length] = '\0';
    if (record_lines)
    {
        if (symbol == '\n')
        {
            fprintf(stdout, "LINE %llu ", (unsigned long long)now_us);
            print_hex(pending, pending_length);
            fputc('\n', stdout);
            pending_length = 0;
            received_length = 0;
        }
        else
        {
            assert(pending_length + 1 < sizeof(pending));
            pending[pending_length++] = (char)symbol;
        }
    }
    return symbol;
}

int host_vprintf(const char *format, va_list arguments)
{
    char text[2048];
    int size = vsnprintf(text, sizeof(text), format, arguments);
    assert(size >= 0 && (size_t)size < sizeof(text));
    for (int i = 0; i < size; i++) host_putchar(text[i]);
    return size;
}

int host_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int size = host_vprintf(format, arguments);
    va_end(arguments);
    return size;
}

static void initialize(void)
{
    platform_init();
    stdio_text_protocol_init();
    led_task_init();
#ifdef TWO_BUTTONS
    button_task_init(PLATFORM_BUTTON_FIRST, led_task_next_state, led_task_blink);
    button_task_init(PLATFORM_BUTTON_SECOND, led_task_next_period, NULL);
#else
    button_task_init(led_task_next_state);
#endif
    pi_task_init();
    telemetry_task_init(api_print_telemetry);
    profiling_stopwatch_init(0, "loop");
    profiling_stopwatch_init(1, "command");
    profiling_stopwatch_init(2, "led");
    profiling_stopwatch_init(3, "button");
    profiling_stopwatch_init(4, "pi");
    profiling_stopwatch_init(5, "telemetry");
}

static const command_t *feed(int symbol)
{
    input_symbol = symbol;
    return stdio_text_protocol_handle();
}

static const command_t *frame(const char *text)
{
    const command_t *command = NULL;
    for (; *text; text++)
    {
        command = feed((unsigned char)*text);
    }
    return command;
}

static void execute(const char *text, const char *expected)
{
    received_length = 0;
    const command_t *command = frame(text);
    assert(command != NULL);
    api_handle(command);
    if (strstr(received, expected) == NULL)
    {
        fprintf(stderr, "Unexpected response to %s: %s\n", text, received);
        abort();
    }
}

static void press(uint pin, uint64_t interval)
{
    pins[pin] = false;
    button_task_handle();
    now_us += interval;
    button_task_handle();
}

static void release(uint pin)
{
    pins[pin] = true;
    button_task_handle();
    now_us += 20000;
    button_task_handle();
}

static void test(void)
{
    initialize();
    assert(outputs[25] && !outputs[15] && pulls[15]);
    assert(feed(PICO_ERROR_TIMEOUT) == NULL);
    assert(frame("  \r") == NULL);
    const command_t *command = frame("  led_period   500  \r");
    assert(command && !command->truncated && command->argc == 1);
    assert(strcmp(command->name, "led_period") == 0);
    assert(strcmp(command->argv[0], "500") == 0);
    assert(feed('\n') == NULL);
    command = frame("upx\x7ftime\r");
    assert(command && strcmp(command->name, "uptime") == 0);
    command = frame("upx\btime\r");
    assert(command && strcmp(command->name, "uptime") == 0);
    command = frame("a 1 2 3 4\r");
    assert(command && command->argc == 4 && !command->truncated);
    command = frame("a 1 2 3 4 5\r");
    assert(command && command->truncated);
    for (int i = 0; i < 64; i++) assert(feed('x') == NULL);
    command = feed('\r');
    assert(command && command->truncated && strlen(command->name) == 63);
    for (int i = 0; i < 64; i++) feed('x');
    feed('\b');
    command = feed('\r');
    assert(command && !command->truncated && strlen(command->name) == 62);

    execute("led_period 1\r", "period 1 ms");
    uint64_t baseline = now_us;
    now_us = baseline + 499;
    led_task_handle(); assert(pins[25]);
    now_us++; led_task_handle(); assert(!pins[25]);
    execute("led_period 4294967295\r", "period 4294967295 ms");
    const char *invalid[] = {"", "-1", "+5", "5x", "0x10", "4294967296",
                            "18446744073709551616", "5 6"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        char text[128];
        snprintf(text, sizeof(text), "led_period %s\r", invalid[i]);
        execute(text, "error: usage led_period <period_ms>");
        assert(led_task_get_period_ms() == UINT32_MAX);
    }
    execute("led_period 0\r", "error: period_ms must be greater than 0");
    execute("led_enable ignored args\r", "led: on");
    assert(pins[25]);
    execute("led_blink\r", "led: blink");
    assert(!pins[25]); // вход из ON сразу переключает физический уровень
    execute("led_enable\r", "led: on");
    execute("blink\r", "error: unknown command 'blink', try help");
    execute("led_period 1 2 3 4 5\r", "error: command longer than 63 characters or 4 arguments");
    assert(led_task_get_state() == LED_STATE_ON);
    execute("pi_start -1\r", "error: usage pi_start [<terms>]");
    execute("pi_start 0\r", "error: terms must be greater than 0");
    execute("pi_start 1 2\r", "error: usage pi_start [<terms>]");

    uint64_t last = 0;
    now_us = 1050;
    assert(systime_period_elapsed(&last, 1000) && last == 1000);
    now_us = 1999; assert(!systime_period_elapsed(&last, 1000));
    now_us = 2010; assert(systime_period_elapsed(&last, 1000) && last == 2000);
    now_us = 5100; assert(systime_period_elapsed(&last, 1000) && last == 5100);
    assert(!systime_period_elapsed(&last, 1000));
    last = UINT64_MAX - 9; now_us = 10;
    assert(systime_period_elapsed(&last, 20) && last == 10);
    assert(!systime_period_elapsed(&last, 0));
    led_task_init();

    press(15, 19999);
    assert(led_task_get_state() == LED_STATE_BLINK);
    now_us++; button_task_handle();
#ifdef TWO_BUTTONS
    assert(led_task_get_state() == LED_STATE_BLINK);
    release(15); // Короткое действие только после подтверждённого отпускания.
#endif
    assert(led_task_get_state() == LED_STATE_OFF);
#ifdef TWO_BUTTONS
    press(15, 20000);
    now_us += 999999; button_task_handle();
    assert(led_task_get_state() == LED_STATE_OFF);
    now_us++; button_task_handle();
    assert(led_task_get_state() == LED_STATE_BLINK);
    led_task_set_state(LED_STATE_OFF);
#endif
    now_us += 2000000; button_task_handle();
    assert(led_task_get_state() == LED_STATE_OFF);
    pins[15] = true; button_task_handle(); now_us += 10000;
    pins[15] = false; button_task_handle(); // дребезг отпускания, не новое нажатие
    release(15);
    press(15, 10000); pins[15] = true; button_task_handle();
    assert(led_task_get_state() == LED_STATE_OFF); // короткая помеха
    press(15, 20000);
#ifdef TWO_BUTTONS
    assert(led_task_get_state() == LED_STATE_OFF);
    release(15); assert(led_task_get_state() == LED_STATE_ON);
    press(15, 20000); // удерживаем первую, вторая всё равно работает
    assert(button_task_get_press_count(PLATFORM_BUTTON_FIRST) == 4);
    assert(pulls[14]);
    press(14, 20000); assert(led_task_get_period_ms() == 250);
    release(14); press(14, 20000); assert(led_task_get_period_ms() == 500);
    release(14); press(14, 20000); assert(led_task_get_period_ms() == 1000);
    release(15);
    assert(led_task_get_state() == LED_STATE_BLINK);
    assert(button_task_is_pressed(PLATFORM_BUTTON_SECOND));
    assert(button_task_get_press_count(PLATFORM_BUTTON_SECOND) == 3);
#else
    assert(led_task_get_state() == LED_STATE_ON);
    assert(button_task_get_press_count() == 2);
    assert(button_task_is_pressed());
    release(15); assert(!button_task_is_pressed());
#endif

    assert(!pi_task_start(0));
    execute("pi\r", "pi: not started");
    execute("pi_start\r", "pi: started, 1000000 terms");
    execute("pi\r", "pi: running");
    execute("pi_start 10\r", "error: pi is busy");
    uint32_t calls = 0;
    while (pi_task_get_state() == PI_RUNNING)
    {
        now_us += 300;
        pi_task_handle();
        calls++;
    }
    assert(calls == 15625);
    double value; uint64_t elapsed;
    assert(pi_task_get_result(&value, &elapsed));
    assert(fabs(value - 3.141592653589793) <= 1.000001e-6);
    assert(pi_task_start(1)); pi_task_handle();
    assert(pi_task_get_result(&value, &elapsed) && value == 4.0);
    assert(pi_task_start(UINT32_MAX));
    pi_k = UINT32_MAX - 10; // проверяем лишь последнюю порцию, не 4 млрд операций
    pi_task_handle(); assert(pi_task_get_state() == PI_READY);

#if PROFILING_ENABLED
    profiling_stopwatch_init(7, "test");
    now_us = UINT32_MAX - 4; profiling_start(7);
    now_us += 10; profiling_stop(7);
    profiling_result_t result;
    assert(profiling_get(7, &result) && result.count == 1 && result.max_us == 10);
    profiling_start(7); now_us += 30; profiling_stop(7);
    assert(profiling_get(7, &result) && result.count == 2 && result.mean_us == 20.0);
    assert(result.max_us == 30);
    profiling_start(7); now_us += 100; profiling_reset();
    now_us += 7; profiling_stop(7);
    assert(profiling_get(7, &result) && result.count == 1 && result.max_us == 7);
    assert(!profiling_get(99, &result));
#else
    execute("profiling\r", "profiling is disabled in this build");
#endif
    fputs("Protocol, API, LED, debounce, pi, overflow and profiling tests passed.\n", stdout);
}

static telemetry_record_t samples[16];
static uint32_t sample_count;

static void remember_record(const telemetry_record_t *record)
{
    samples[sample_count++ % 16] = *record;
}

static void test_adc_and_telemetry(void)
{
    now_us = 0;
    initialize();
    assert(adc_enabled && sensor_enabled && analog_pin == 26);
    assert(outputs[22] && !pins[22]);
    adc_codes[0] = 0;
    assert(platform_voltage_read() == 0.0f && adc_channel == 0);
    adc_codes[0] = 4095;
    assert(fabs(platform_voltage_read() - 3.3 * 4095 / 4096) < 1e-6);
    adc_codes[0] = 2048;
    assert(fabs(platform_voltage_read() - 1.65) < 1e-6);
    float temp = platform_temp_read();
    assert(adc_channel == 4 && fabs(temp - (27 - (876 * 3.3 / 4096 - .706) / .001721)) < 1e-4);
    assert(fabs(platform_voltage_read() - 1.65) < 1e-6 && adc_channel == 0);
    platform_charge_set(true); assert(pins[22]);
    platform_charge_set(false); assert(!pins[22]);
    execute("get_voltage\r", "voltage: 1.650 V");
    execute("get_temp\r", "temp: 27.1 C");
    execute("charge_on\r", "charge: on"); assert(pins[22]);
    execute("charge_off\r", "charge: off"); assert(!pins[22]);

    telemetry_task_init(remember_record);
    assert(telemetry_task_get_state() == TELEMETRY_STATE_IDLE);
    assert(!telemetry_task_start(9));
    telemetry_task_handle(); assert(sample_count == 0);
    assert(telemetry_task_start(50));
    telemetry_task_handle();
    assert(sample_count == 1 && samples[0].time_us == 0);
    assert(fabs(samples[0].voltage_v - 1.65) < 1e-6);
    now_us = 49999; telemetry_task_handle(); assert(sample_count == 1);
    now_us = 50010; telemetry_task_handle();
    assert(sample_count == 2 && samples[1].time_us == 50010);
    now_us = 100000; telemetry_task_handle();
    assert(sample_count == 3 && samples[2].time_us == 100000);
    assert(!telemetry_task_start(0) && telemetry_task_get_period_ms() == 50);
    assert(telemetry_task_get_state() == TELEMETRY_STATE_RUN);
    now_us = 125000;
    assert(telemetry_task_start(100)); telemetry_task_handle();
    assert(sample_count == 4 && samples[3].time_us == 125000);
    now_us = 224999; telemetry_task_handle(); assert(sample_count == 4);
    now_us++; telemetry_task_handle(); assert(sample_count == 5);
    now_us = 0; assert(telemetry_task_start(UINT32_MAX));
    telemetry_task_handle(); assert(sample_count == 6);
    now_us = (uint64_t)UINT32_MAX * 1000 - 1;
    telemetry_task_handle(); assert(sample_count == 6);
    now_us++; telemetry_task_handle(); assert(sample_count == 7);
    telemetry_task_stop();
    uint32_t reads = adc_reads;
    now_us += 1000000; telemetry_task_handle();
    assert(sample_count == 7 && adc_reads == reads);

    telemetry_task_init(api_print_telemetry);
    execute("tm_start\r", "telemetry: started, period 100 ms");
    const char *invalid[] = {"5x", "-1", "+10", "4294967296", "10 20"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        char text[100];
        snprintf(text, sizeof(text), "tm_start %s\r", invalid[i]);
        execute(text, "error: usage tm_start [<period_ms>]");
        assert(telemetry_task_get_period_ms() == 100);
        assert(telemetry_task_get_state() == TELEMETRY_STATE_RUN);
    }
    execute("tm_start 5\r", "error: period_ms must be at least 10");
    assert(telemetry_task_get_period_ms() == 100);
    execute("tm_start 0\r", "error: period_ms must be at least 10");
    execute("tm_start 10\r", "telemetry: started, period 10 ms");
    execute("tm_start 4294967295\r", "telemetry: started, period 4294967295 ms");
    execute("tm_stop\r", "telemetry: stopped");
    assert(telemetry_task_get_state() == TELEMETRY_STATE_IDLE);
    telemetry_task_init(NULL); assert(telemetry_task_start(10)); telemetry_task_handle();
    telemetry_task_stop();

    // Телеметрия продолжает работать, пока миллион членов считается по порциям.
    sample_count = 0;
    telemetry_task_init(remember_record);
    assert(telemetry_task_start(50));
    pi_task_init(); assert(pi_task_start(1000000));
    while (pi_task_get_state() == PI_RUNNING)
    {
        now_us += 300;
        pi_task_handle();
        telemetry_task_handle();
    }
    assert(sample_count >= 90);
    telemetry_task_stop();
    fputs("ADC conversion, channel switching, GPIO, telemetry timing/restart/stop and pi concurrency passed.\n", stdout);
}

static void iteration(void)
{
    // Тот же порядок, что в main.c; добавленные длительности — модель,
    // не замеры компьютера или RP2040. Цена члена π взята из прикидки задания.
    profiling_start(0);
    profiling_start(1);
    const command_t *command = stdio_text_protocol_handle();
    if (command) api_handle(command);
    now_us += 3; profiling_stop(1);
    profiling_start(2); led_task_handle(); now_us++; profiling_stop(2);
    profiling_start(3); button_task_handle(); now_us++; profiling_stop(3);
    profiling_start(4);
    uint32_t portion = pi_state == PI_RUNNING ? pi_terms - pi_k : 0;
    if (portion > PI_TERMS_PER_CALL) portion = PI_TERMS_PER_CALL;
    now_us += portion ? (portion * 585 + 124) / 125 : 1;
    pi_task_handle(); profiling_stop(4);
    profiling_start(5); telemetry_task_handle(); now_us++; profiling_stop(5);
    now_us += 4; profiling_stop(0);
}

static void model(void)
{
    initialize();
    record_lines = charge_output = true;
    char input[4096];
    while (fgets(input, sizeof(input), stdin))
    {
        char *end;
        uint64_t listen_us = strtoull(input, &end, 10);
        while (*end == ' ') end++;
        size_t length = strcspn(end, "\r\n");
        assert(length % 2 == 0);
        fprintf(stdout, "SEND %llu ", (unsigned long long)now_us);
        fwrite(end, 1, length, stdout); fputc('\n', stdout);
        uint64_t deadline = now_us + listen_us;
        for (size_t i = 0; i < length; i += 2)
        {
            char hex[3] = {end[i], end[i + 1], 0};
            input_symbol = (int)strtoul(hex, NULL, 16);
            iteration();
        }
        while (now_us < deadline) iteration();
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "--model") == 0 || strcmp(argv[1], "--model-rc") == 0))
    {
        model_inputs = true;
        rc_model = strcmp(argv[1], "--model-rc") == 0;
        model();
    }
    else
    {
        test();
        test_adc_and_telemetry();
    }
    return 0;
}
