#include "platform/platform.h"

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include <string.h>

#define ADC_PIN 26
#define ADC_CHANNEL 0
#define ADC_TEMP_CHANNEL 4
#define ADC_VREF_V 3.3f
#define ADC_LEVELS 4096
#define CHARGE_PIN 22
#define TONE_PIN 20
#define TONE_CLOCK_HZ 1000000u
#define ADC_CLOCK_HZ 48000000.0f
#define WATCHDOG_MAX_MS 8388

static const uint LED_PIN = PICO_DEFAULT_LED_PIN;
static const uint BUTTON_PIN = 15;

typedef enum { ADC_FREE, ADC_CAPTURE, ADC_BLOCK, ADC_STREAM } adc_user_t;
static volatile adc_user_t adc_user;
static repeating_timer_t blink_timer;
static bool blink_running;
static volatile bool blink_on;
static volatile uint32_t blink_toggles;
static repeating_timer_t capture_timer;
static uint16_t capture_samples[PLATFORM_CAPTURE_MAX];
static uint32_t capture_times[PLATFORM_CAPTURE_MAX];
static uint32_t capture_count, capture_period_us;
static volatile uint32_t capture_index, capture_handler_us;
static int block_channel;
static uint16_t block_samples[PLATFORM_BLOCK_MAX];
static uint32_t block_start_us;
static volatile uint32_t block_end_us;
static volatile bool block_complete;
static int stream_channels[2];
// 1024-byte write rings also keep DMA inside its buffer if an IRQ arrives late.
static uint8_t stream_buffers[2][PLATFORM_STREAM_BYTES] __attribute__((aligned(1024)));
static uint8_t stream_snapshot[PLATFORM_STREAM_BYTES];
static volatile bool stream_ready[2];
static volatile uint32_t stream_numbers[2], stream_overruns;
static uint32_t stream_samples, stream_produced;
static uint32_t stream_irq_index, stream_claimed, stream_claimed_number;
static uint tone_slice;
static bool watchdog_reset, watchdog_enabled;
static uint32_t watchdog_restarts;

_Static_assert(sizeof capture_samples + sizeof capture_times + sizeof block_samples +
               sizeof stream_buffers + sizeof stream_snapshot <= 32 * 1024,
               "Recorder buffer budget exceeded");

static bool on_blink(repeating_timer_t *timer)
{
    (void)timer;
    blink_on = !blink_on;
    gpio_put(LED_PIN, blink_on);
    ++blink_toggles;
    return true;
}

static bool on_capture(repeating_timer_t *timer)
{
    (void)timer;
    uint32_t start_us = time_us_32();
    uint32_t index = capture_index;
    capture_times[index] = start_us;
    capture_samples[index] = adc_read();
    capture_handler_us += time_us_32() - start_us;
    __dmb();
    capture_index = index + 1;
    if (capture_index < capture_count) return true;
    adc_user = ADC_FREE;
    return false;
}

static void adc_free_running(bool bits8)
{
    adc_select_input(ADC_CHANNEL);
    adc_fifo_setup(true, true, 1, false, bits8);
    adc_set_clkdiv(ADC_CLOCK_HZ / PLATFORM_SAMPLE_RATE_HZ - 1);
}

static void adc_one_shot(void)
{
    adc_run(false);
    adc_fifo_drain();
    adc_fifo_setup(false, false, 0, false, false);
    adc_set_clkdiv(0);
}

static void on_block_done(void)
{
    if (!dma_channel_get_irq1_status(block_channel)) return;
    dma_channel_acknowledge_irq1(block_channel);
    block_end_us = time_us_32();
    adc_run(false);
    __dmb();
    block_complete = true;
}

static void on_stream_buffer(void)
{
    // If both interrupts are pending, process them in acquisition order.
    for (uint i = 0; i < 2; ++i)
    {
        uint index = stream_irq_index;
        int channel = stream_channels[index];
        if (!dma_channel_get_irq0_status(channel)) break;
        dma_channel_acknowledge_irq0(channel);
        if (!dma_channel_is_busy(channel))
            dma_channel_set_write_addr(channel, stream_buffers[index], false);
        uint next = index ^ 1u;
        if (stream_ready[next])
        {
            ++stream_overruns;
            stream_ready[next] = false;
        }
        stream_numbers[index] = stream_produced++;
        __dmb();
        stream_ready[index] = true;
        stream_irq_index = next;
    }
}

void platform_init(void)
{
    watchdog_reset = watchdog_enable_caused_reboot();
    if (watchdog_reset) ++watchdog_hw->scratch[0];
    else watchdog_hw->scratch[0] = 0;
    watchdog_restarts = watchdog_hw->scratch[0];
    watchdog_enabled = false;
    adc_user = ADC_FREE;
    blink_running = false;
    blink_toggles = 0;
    capture_count = capture_index = 0;
    block_complete = false;
    stream_overruns = 0;
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_set_temp_sensor_enabled(true);
    gpio_init(CHARGE_PIN);
    gpio_put(CHARGE_PIN, false);
    gpio_set_dir(CHARGE_PIN, GPIO_OUT);
    gpio_set_function(TONE_PIN, GPIO_FUNC_PWM);
    tone_slice = pwm_gpio_to_slice_num(TONE_PIN);
    pwm_set_clkdiv(tone_slice, (float)clock_get_hz(clk_sys) / TONE_CLOCK_HZ);
    pwm_set_wrap(tone_slice, 999);
    pwm_set_gpio_level(TONE_PIN, 0);
    pwm_set_enabled(tone_slice, true);

    block_channel = dma_claim_unused_channel(true);
    dma_channel_set_irq1_enabled(block_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_1, on_block_done);
    irq_set_enabled(DMA_IRQ_1, true);
    stream_channels[0] = dma_claim_unused_channel(true);
    stream_channels[1] = dma_claim_unused_channel(true);
    irq_set_exclusive_handler(DMA_IRQ_0, on_stream_buffer);
    irq_set_enabled(DMA_IRQ_0, true);
}

void platform_led_set(bool on)
{
    if (!blink_running) gpio_put(LED_PIN, on);
}

bool platform_button_read(void)
{
    return !gpio_get(BUTTON_PIN);
}

float platform_voltage_read(void)
{
    adc_select_input(ADC_CHANNEL);
    return platform_adc_volts(adc_read());
}

float platform_temp_read(void)
{
    adc_select_input(ADC_TEMP_CHANNEL);
    float voltage_v = platform_adc_volts(adc_read());
    return 27.0f - (voltage_v - 0.706f) / 0.001721f;
}

void platform_charge_set(bool on)
{
    gpio_put(CHARGE_PIN, on);
}

bool platform_blink_start(uint32_t period_ms)
{
    if (period_ms < 10 || period_ms > 60000) return false;
    platform_blink_stop();
    blink_on = false;
    blink_toggles = 0;
    blink_running = add_repeating_timer_ms(-(int32_t)period_ms, on_blink, NULL, &blink_timer);
    return blink_running;
}

uint32_t platform_blink_stop(void)
{
    if (blink_running) cancel_repeating_timer(&blink_timer);
    blink_running = false;
    gpio_put(LED_PIN, false);
    return blink_toggles;
}

bool platform_adc_busy(void) { return adc_user != ADC_FREE; }
float platform_adc_volts(float code) { return code * ADC_VREF_V / ADC_LEVELS; }

bool platform_capture_start(uint32_t count, uint32_t period_us)
{
    if (platform_adc_busy() || count == 0 || count > PLATFORM_CAPTURE_MAX || period_us < 10)
        return false;
    adc_select_input(ADC_CHANNEL);
    capture_count = count;
    capture_period_us = period_us;
    capture_index = 0;
    capture_handler_us = 0;
    adc_user = ADC_CAPTURE;
    if (add_repeating_timer_us(-(int64_t)period_us, on_capture, NULL, &capture_timer))
        return true;
    capture_count = 0;
    adc_user = ADC_FREE;
    return false;
}

bool platform_capture_done(void)
{
    return capture_count != 0 && capture_index == capture_count;
}

void platform_capture_get(platform_capture_t *capture)
{
    uint32_t min_us = capture_count > 1 ? UINT32_MAX : 0, max_us = 0;
    for (uint32_t i = 1; i < capture_count; ++i)
    {
        uint32_t interval = capture_times[i] - capture_times[i - 1];
        if (interval < min_us) min_us = interval;
        if (interval > max_us) max_us = interval;
    }
    *capture = (platform_capture_t){
        capture_samples, capture_count, capture_period_us, min_us, max_us,
        capture_count ? (100.0f * capture_handler_us) /
                        ((float)capture_count * capture_period_us) : 0.0f
    };
}

bool platform_block_start(uint32_t count)
{
    if (platform_adc_busy() || count == 0 || count > PLATFORM_BLOCK_MAX) return false;
    adc_free_running(false);
    dma_channel_config cfg = dma_channel_get_default_config(block_channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);
    block_complete = false;
    adc_user = ADC_BLOCK;
    dma_channel_configure(block_channel, &cfg, block_samples, &adc_hw->fifo, count, true);
    block_start_us = time_us_32();
    adc_run(true);
    return true;
}

bool platform_block_done(void)
{
    if (!block_complete) return false;
    __dmb();
    if (adc_user == ADC_BLOCK)
    {
        adc_one_shot();
        adc_user = ADC_FREE;
    }
    return true;
}

const uint16_t *platform_block_samples(void) { return block_samples; }
uint32_t platform_block_time_us(void) { return block_end_us - block_start_us; }

bool platform_stream_start(uint8_t bits)
{
    if (platform_adc_busy() || (bits != 8 && bits != 16)) return false;
    bool bits8 = bits == 8;
    stream_samples = bits8 ? PLATFORM_STREAM_BYTES : PLATFORM_STREAM_BYTES / 2;
    adc_free_running(bits8);
    stream_overruns = stream_produced = stream_irq_index = 0;
    for (uint i = 0; i < 2; ++i)
    {
        int channel = stream_channels[i];
        dma_channel_config cfg = dma_channel_get_default_config(channel);
        channel_config_set_transfer_data_size(&cfg, bits8 ? DMA_SIZE_8 : DMA_SIZE_16);
        channel_config_set_read_increment(&cfg, false);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_dreq(&cfg, DREQ_ADC);
        channel_config_set_ring(&cfg, true, 10);
        channel_config_set_chain_to(&cfg, stream_channels[i ^ 1u]);
        dma_channel_configure(channel, &cfg, stream_buffers[i], &adc_hw->fifo,
                              stream_samples, false);
        stream_ready[i] = false;
        dma_channel_acknowledge_irq0(channel);
        dma_channel_set_irq0_enabled(channel, true);
    }
    adc_user = ADC_STREAM;
    dma_channel_start(stream_channels[0]);
    adc_run(true);
    return true;
}

void platform_stream_stop(void)
{
    if (adc_user != ADC_STREAM) return;
    adc_run(false);
    uint32_t irq_state = save_and_disable_interrupts();
    // Break both chains before aborting either channel (RP2040-E13).
    for (uint i = 0; i < 2; ++i)
    {
        int channel = stream_channels[i];
        dma_channel_set_irq0_enabled(channel, false);
        dma_channel_config cfg = dma_get_channel_config(channel);
        channel_config_set_chain_to(&cfg, channel);
        dma_channel_set_config(channel, &cfg, false);
    }
    restore_interrupts(irq_state);
    for (uint i = 0; i < 2; ++i)
    {
        dma_channel_abort(stream_channels[i]);
        dma_channel_acknowledge_irq0(stream_channels[i]);
        stream_ready[i] = false;
    }
    adc_one_shot();
    adc_user = ADC_FREE;
}

const uint8_t *platform_stream_next(uint32_t *samples)
{
    if (adc_user != ADC_STREAM) return NULL;
    uint32_t irq_state = save_and_disable_interrupts();
    uint index = stream_ready[0] ? 0 : 1;
    uint32_t number = stream_numbers[index];
    bool ready = stream_ready[index] && !dma_channel_is_busy(stream_channels[index]);
    restore_interrupts(irq_state);
    if (!ready) return NULL;

    // A stable snapshot prevents USB from reading memory that DMA is refilling.
    memcpy(stream_snapshot, stream_buffers[index], sizeof stream_snapshot);
    __dmb();
    irq_state = save_and_disable_interrupts();
    ready = stream_ready[index] && stream_numbers[index] == number &&
            !dma_channel_is_busy(stream_channels[index]) &&
            !dma_channel_get_irq0_status(stream_channels[index]);
    restore_interrupts(irq_state);
    if (!ready) return NULL;
    stream_claimed = index;
    stream_claimed_number = number;
    *samples = stream_samples;
    return stream_snapshot;
}

uint32_t platform_stream_sequence(void) { return stream_claimed_number; }

void platform_stream_release(void)
{
    uint32_t irq_state = save_and_disable_interrupts();
    if (stream_numbers[stream_claimed] == stream_claimed_number)
        stream_ready[stream_claimed] = false;
    restore_interrupts(irq_state);
}

uint32_t platform_stream_overruns(void) { return stream_overruns; }

void platform_write(const void *data, uint32_t bytes)
{
    stdio_put_string((const char *)data, (int)bytes, false, false);
}

bool platform_tone_set(uint32_t freq_hz)
{
    if (freq_hz == 0)
    {
        pwm_set_gpio_level(TONE_PIN, 0);
        return true;
    }
    if (freq_hz < 20 || freq_hz > 20000) return false;
    uint32_t period = (TONE_CLOCK_HZ + freq_hz / 2) / freq_hz;
    pwm_set_wrap(tone_slice, period - 1);
    pwm_set_gpio_level(TONE_PIN, period / 2);
    return true;
}

bool platform_watchdog_enable(uint32_t timeout_ms)
{
    if (timeout_ms == 0 || timeout_ms > WATCHDOG_MAX_MS) return false;
    watchdog_enable(timeout_ms, true);
    watchdog_enabled = true;
    return true;
}

void platform_watchdog_feed(void)
{
    if (watchdog_enabled) watchdog_update();
}

bool platform_watchdog_caused_reset(uint32_t *restarts)
{
    *restarts = watchdog_restarts;
    return watchdog_reset;
}
