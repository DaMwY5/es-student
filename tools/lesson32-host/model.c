/* Virtual SDK for testing the actual recorder C modules, not board measurements. */
#include "sim_sdk.h"
#include "platform/platform.h"
#include "api/api.h"
#include "block-task/block-task.h"
#include "button-task/button-task.h"
#include "led-task/led-task.h"
#include "pi-task/pi-task.h"
#include "stream-task/stream-task.h"
#include "telemetry-task/telemetry-task.h"
#include "stdio-text-protocol/stdio-text-protocol.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef printf
#undef putchar
#undef vprintf

enum { CHANNELS = 3, TIMER_SLOTS = 2 };
typedef struct {
    dma_channel_config cfg;
    uint8_t *base, *write;
    uint32_t count, remaining;
    bool busy, pending, irq[2];
} channel_t;
static channel_t dma[CHANNELS];
static uint claimed;
static void (*irq_handlers[2])(void);
static bool irq_enabled[2], interrupts_disabled;
static repeating_timer_t *timers[TIMER_SLOTS];
static bool pins[30], adc_running, adc_shift, adc_fifo, adc_dreq;
static uint selected, tone_period, tone_level;
static double adc_interval, next_sample;
static uint64_t now, boot_time, tone_start, watchdog_deadline, watchdog_period;
static bool reset_by_watchdog, loopback;
static int input = PICO_ERROR_TIMEOUT;
static adc_hw_t adc_registers;
static watchdog_hw_t watchdog_registers;
adc_hw_t *adc_hw = &adc_registers;
watchdog_hw_t *watchdog_hw = &watchdog_registers;
uint32_t host_chip_id = 0x20002927;

static void boot(void)
{
    memset(dma, 0, sizeof dma);
    memset(timers, 0, sizeof timers);
    memset(pins, 0, sizeof pins);
    memset(irq_enabled, 0, sizeof irq_enabled);
    claimed = 0;
    adc_running = adc_shift = adc_fifo = adc_dreq = interrupts_disabled = false;
    watchdog_deadline = watchdog_period = 0;
    boot_time = now;
    platform_init();
    api_init();
    stdio_text_protocol_init();
    led_task_init();
    button_task_init(led_task_next_state);
    pi_task_init();
    telemetry_task_init(api_print_telemetry);
    block_task_init();
    stream_task_init(api_send_frame);
}

static uint16_t signal_at(double time)
{
    if (selected == 4) return 876;
    assert(selected == 0);
    if (loopback)
    {
        return tone_level && fmod(time - tone_start, tone_period) < tone_level ? 4095 : 0;
    }
    // A deterministic synthetic microphone input, not a recording of a person.
    const double t = time / 1000000.0, pi2 = 6.283185307179586;
    double envelope = 24.0 + 13.0 * sin(pi2 * 2.5 * t);
    return (uint16_t)round(1544.0 + envelope *
        (sin(pi2 * 193.0 * t) + 0.35 * sin(pi2 * 386.0 * t)) +
        3.0 * sin(pi2 * 1391.0 * t));
}

static void dispatch_irqs(void)
{
    if (interrupts_disabled) return;
    for (uint irq = 0; irq < 2; ++irq)
    {
        if (!irq_enabled[irq]) continue;
        for (uint channel = 0; channel < CHANNELS; ++channel)
        {
            if (dma[channel].pending && dma[channel].irq[irq])
            {
                irq_handlers[irq]();
                break;
            }
        }
    }
}

static void sample(void)
{
    uint16_t code = signal_at(next_sample);
    if (adc_shift) code >>= 4;
    next_sample += adc_interval;
    for (uint channel = 0; channel < CHANNELS; ++channel)
    {
        channel_t *c = &dma[channel];
        if (!c->busy) continue;
        assert(adc_fifo && adc_dreq && c->cfg.dreq == DREQ_ADC);
        assert(c->cfg.size == (adc_shift ? 1u : 2u));
        size_t position = (size_t)(c->write - c->base);
        assert(position + c->cfg.size <= c->count * c->cfg.size);
        c->write[0] = (uint8_t)code;
        if (c->cfg.size == 2) c->write[1] = (uint8_t)(code >> 8);
        c->write += c->cfg.size;
        if (c->cfg.ring && c->write - c->base == (1 << c->cfg.ring)) c->write = c->base;
        if (--c->remaining == 0)
        {
            c->busy = false;
            c->pending = true;
            if (c->cfg.chain != channel) dma_channel_start(c->cfg.chain);
            dispatch_irqs();
        }
        break;
    }
}

static void advance(uint64_t target)
{
    for (;;)
    {
        uint64_t deadline = UINT64_MAX;
        int event = -1;
        if (adc_running)
        {
            deadline = (uint64_t)ceil(next_sample);
            event = TIMER_SLOTS;
        }
        for (int i = 0; i < TIMER_SLOTS; ++i)
        {
            if (timers[i] && timers[i]->active && timers[i]->due < deadline)
            {
                deadline = timers[i]->due;
                event = i;
            }
        }
        if (watchdog_deadline && watchdog_deadline <= deadline)
        {
            deadline = watchdog_deadline;
            event = TIMER_SLOTS + 1;
        }
        if (deadline > target) break;
        if (deadline > now) now = deadline;
        if (event < TIMER_SLOTS)
        {
            repeating_timer_t *timer = timers[event];
            timer->due += timer->interval;
            timer->active = timer->callback(timer);
        }
        else if (event == TIMER_SLOTS) sample();
        else
        {
            reset_by_watchdog = true;
            fprintf(stdout, "RESET %llu\n", (unsigned long long)now);
            boot();
        }
    }
    if (now < target) now = target;
}

static void event_bytes(const char *kind, const void *data, size_t count)
{
    const uint8_t *bytes = data;
    fprintf(stdout, "%s %llu ", kind, (unsigned long long)now);
    for (size_t i = 0; i < count; ++i) fprintf(stdout, "%02x", bytes[i]);
    fputc('\n', stdout);
}

static void send_bytes(const char *kind, const void *data, size_t count)
{
    event_bytes(kind, data, count);
    // Assumed 700 kB/s link, not a measurement of USB or task execution time.
    advance(now + (count * 10 + 6) / 7);
}

int model_vprintf(const char *format, va_list args)
{
    char buffer[1024];
    int n = vsnprintf(buffer, sizeof buffer, format, args);
    assert(n >= 0 && (size_t)n < sizeof buffer);
    if (n) send_bytes("TEXT", buffer, (size_t)n);
    return n;
}
int model_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = model_vprintf(format, args);
    va_end(args);
    return n;
}
int model_putchar(int c)
{
    char ch = (char)c;
    send_bytes("TEXT", &ch, 1);
    return c;
}

void stdio_init_all(void) {}
int getchar_timeout_us(uint32_t timeout)
{
    assert(timeout == 0);
    int value = input;
    input = PICO_ERROR_TIMEOUT;
    return value;
}
int stdio_put_string(const char *data, int len, bool newline, bool cr_translation)
{
    assert(!newline && !cr_translation && len > 0);
    send_bytes("DATA", data, (size_t)len);
    return len;
}
uint64_t time_us_64(void) { return now - boot_time; }
uint32_t time_us_32(void) { return (uint32_t)time_us_64(); }
void pico_get_unique_board_id_string(char *serial, size_t size)
{
    assert(size >= 17);
    memcpy(serial, "0000000000000000", 17);
}

void gpio_init(uint pin) { assert(pin < 30); }
void gpio_set_dir(uint pin, bool out) { (void)pin; (void)out; }
void gpio_put(uint pin, bool on) { pins[pin] = on; }
bool gpio_get(uint pin) { return pins[pin]; }
void gpio_pull_up(uint pin) { pins[pin] = true; }
void gpio_set_function(uint pin, uint function) { assert(pin == 20 && function == GPIO_FUNC_PWM); }

bool add_repeating_timer_us(int64_t period, bool (*fn)(repeating_timer_t *), void *user,
                            repeating_timer_t *timer)
{
    (void)user;
    assert(period < 0);
    uint64_t interval = (uint64_t)-period;
    for (uint i = 0; i < TIMER_SLOTS; ++i)
    {
        if (!timers[i] || !timers[i]->active)
        {
            *timer = (repeating_timer_t){now + interval, interval, fn, true};
            timers[i] = timer;
            return true;
        }
    }
    return false;
}
bool add_repeating_timer_ms(int32_t period, bool (*fn)(repeating_timer_t *), void *user,
                            repeating_timer_t *timer)
{
    return add_repeating_timer_us((int64_t)period * 1000, fn, user, timer);
}
bool cancel_repeating_timer(repeating_timer_t *timer)
{
    bool active = timer->active;
    timer->active = false;
    return active;
}
void adc_init(void) { adc_interval = 2; }
void adc_gpio_init(uint pin) { assert(pin == 26); }
void adc_set_temp_sensor_enabled(bool enabled) { assert(enabled); }
void adc_select_input(uint channel) { assert(channel == 0 || channel == 4); selected = channel; }
uint16_t adc_read(void)
{
    assert(!adc_running && !adc_fifo);
    uint16_t value = signal_at((double)now);
    now += 2;
    return value;
}
void adc_fifo_setup(bool enable, bool dreq, uint16_t threshold, bool error, bool shift)
{
    assert(!adc_running && !error && threshold == (dreq ? 1 : 0));
    adc_fifo = enable;
    adc_dreq = dreq;
    adc_shift = shift;
}
void adc_fifo_drain(void) { assert(!adc_running); }
void adc_set_clkdiv(float divider)
{
    // RP2040 divider is fixed-point with 8 fractional bits.
    adc_interval = fmax(96, 1 + floor((double)divider * 256) / 256) / 48;
    if (divider) assert(fabs(1e6 / adc_interval - 44100) < 1);
}
void adc_run(bool on) { adc_running = on; if (on) next_sample = now + adc_interval; }

int dma_claim_unused_channel(bool required) { assert(required && claimed < CHANNELS); return (int)claimed++; }
dma_channel_config dma_channel_get_default_config(uint channel)
{
    return (dma_channel_config){.chain = channel, .size = 2};
}
dma_channel_config dma_get_channel_config(uint channel) { return dma[channel].cfg; }
void channel_config_set_transfer_data_size(dma_channel_config *c, dma_channel_transfer_size s) { c->size = s; }
void channel_config_set_read_increment(dma_channel_config *c, bool on) { c->read_increment = on; }
void channel_config_set_write_increment(dma_channel_config *c, bool on) { c->write_increment = on; }
void channel_config_set_dreq(dma_channel_config *c, uint dreq) { c->dreq = dreq; }
void channel_config_set_ring(dma_channel_config *c, bool write, uint bits) { assert(write); c->ring = bits; }
void channel_config_set_chain_to(dma_channel_config *c, uint next) { c->chain = next; }
void dma_channel_configure(uint channel, const dma_channel_config *cfg, volatile void *write,
                           const volatile void *read, uint32_t count, bool trigger)
{
    assert(read == &adc_hw->fifo && cfg->write_increment && !cfg->read_increment && count > 0);
    if (cfg->ring) assert(((uintptr_t)write & ((1u << cfg->ring) - 1)) == 0);
    dma[channel].cfg = *cfg;
    dma[channel].base = dma[channel].write = (uint8_t *)write;
    dma[channel].count = count;
    if (trigger) dma_channel_start(channel);
}
void dma_channel_start(uint channel)
{
    assert(channel < CHANNELS && !dma[channel].busy && dma[channel].count);
    dma[channel].remaining = dma[channel].count;
    dma[channel].busy = true;
}
void dma_channel_abort(uint channel)
{
    // Deliberately model the risk of an in-flight transfer chaining after abort.
    if (dma[channel].busy && dma[channel].cfg.chain != channel)
        dma_channel_start(dma[channel].cfg.chain);
    dma[channel].busy = false;
}
bool dma_channel_is_busy(uint c) { return dma[c].busy; }
void dma_channel_set_config(uint c, const dma_channel_config *cfg, bool trigger)
{
    dma[c].cfg = *cfg;
    if (trigger) dma_channel_start(c);
}
void dma_channel_set_write_addr(uint c, volatile void *write, bool trigger)
{
    assert(!dma[c].busy && write == dma[c].base);
    dma[c].write = (uint8_t *)write;
    if (trigger) dma_channel_start(c);
}
bool dma_channel_get_irq0_status(uint c) { return dma[c].pending && dma[c].irq[0]; }
bool dma_channel_get_irq1_status(uint c) { return dma[c].pending && dma[c].irq[1]; }
void dma_channel_set_irq0_enabled(uint c, bool enabled) { dma[c].irq[0] = enabled; }
void dma_channel_set_irq1_enabled(uint c, bool enabled) { dma[c].irq[1] = enabled; }
void dma_channel_acknowledge_irq0(uint c) { dma[c].pending = false; }
void dma_channel_acknowledge_irq1(uint c) { dma[c].pending = false; }
void irq_set_exclusive_handler(uint irq, void (*handler)(void)) { irq_handlers[irq] = handler; }
void irq_set_enabled(uint irq, bool enabled) { irq_enabled[irq] = enabled; }
uint32_t save_and_disable_interrupts(void)
{
    bool previous = interrupts_disabled;
    interrupts_disabled = true;
    return previous;
}
void restore_interrupts(uint32_t previous)
{
    interrupts_disabled = previous != 0;
    dispatch_irqs();
}
uint clock_get_hz(uint clock) { assert(clock == clk_sys); return 125000000; }
uint pwm_gpio_to_slice_num(uint pin) { assert(pin == 20); return 2; }
void pwm_set_clkdiv(uint slice, float divider) { assert(slice == 2 && divider == 125); }
void pwm_set_wrap(uint slice, uint16_t wrap) { assert(slice == 2); tone_period = wrap + 1u; }
void pwm_set_gpio_level(uint pin, uint16_t level)
{
    assert(pin == 20);
    tone_level = level;
    tone_start = now;
}
void pwm_set_enabled(uint slice, bool enabled) { assert(slice == 2 && enabled); }
bool watchdog_enable_caused_reboot(void) { return reset_by_watchdog; }
void watchdog_enable(uint32_t timeout_ms, bool pause_on_debug)
{
    assert(timeout_ms > 0 && timeout_ms <= 8388 && pause_on_debug);
    watchdog_period = (uint64_t)timeout_ms * 1000;
    watchdog_update();
}
void watchdog_update(void) { watchdog_deadline = now + watchdog_period; }

static void step(void)
{
    if (api_superloop_halted()) { advance(now + 10); return; }
    const command_t *command = stdio_text_protocol_handle();
    if (command) api_handle(command);
    if (api_superloop_halted()) return;
    led_task_handle();
    button_task_handle();
    bool calculating = pi_task_get_state() == PI_RUNNING;
    pi_task_handle();
    telemetry_task_handle();
    block_task_handle();
    stream_task_handle();
    platform_watchdog_feed();
    advance(now + (calculating ? 310 : 10));
}

int main(int argc, char **argv)
{
    loopback = argc == 2 && strcmp(argv[1], "tone") == 0;
    boot();
    char line[4096];
    while (fgets(line, sizeof line, stdin))
    {
        char *hex;
        uint64_t wait = strtoull(line, &hex, 10);
        while (*hex == ' ') ++hex;
        if (*hex == '!')
        {
            advance(now + wait); // model a superloop stall with IRQs still active
            continue;
        }
        size_t len = strcspn(hex, "\r\n");
        assert(len % 2 == 0);
        char command[2048];
        for (size_t i = 0; i < len / 2; ++i)
        {
            char byte[] = {hex[2*i], hex[2*i+1], 0};
            command[i] = (char)strtoul(byte, NULL, 16);
        }
        if (len) event_bytes("SEND", command, len / 2);
        uint64_t deadline = now + wait;
        for (size_t i = 0; i < len / 2; ++i)
        {
            input = (uint8_t)command[i];
            step();
        }
        while (now < deadline) step();
    }
    return 0;
}
