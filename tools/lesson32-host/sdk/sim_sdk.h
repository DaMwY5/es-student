#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;
#define PICO_ERROR_TIMEOUT (-1)
#define PICO_DEFAULT_LED_PIN 25
#define GPIO_IN 0
#define GPIO_OUT 1
#define GPIO_FUNC_PWM 4
#define DREQ_ADC 36
#define DMA_IRQ_0 0
#define DMA_IRQ_1 1
#define clk_sys 0
#define __dmb() ((void)0)

typedef struct repeating_timer {
    uint64_t due, interval;
    bool (*callback)(struct repeating_timer *);
    bool active;
} repeating_timer_t;

typedef enum { DMA_SIZE_8 = 1, DMA_SIZE_16 = 2 } dma_channel_transfer_size;
typedef struct {
    uint size, chain, dreq, ring;
    bool read_increment, write_increment;
} dma_channel_config;
typedef struct { volatile uint32_t fifo; } adc_hw_t;
typedef struct { uint32_t scratch[8]; } watchdog_hw_t;
extern adc_hw_t *adc_hw;
extern watchdog_hw_t *watchdog_hw;

void stdio_init_all(void);
int getchar_timeout_us(uint32_t timeout);
int stdio_put_string(const char *data, int len, bool newline, bool cr_translation);
uint32_t time_us_32(void);
uint64_t time_us_64(void);
void gpio_init(uint pin);
void gpio_set_dir(uint pin, bool out);
void gpio_put(uint pin, bool on);
bool gpio_get(uint pin);
void gpio_pull_up(uint pin);
void gpio_set_function(uint pin, uint function);
bool add_repeating_timer_us(int64_t period, bool (*fn)(repeating_timer_t *),
                            void *user, repeating_timer_t *timer);
bool add_repeating_timer_ms(int32_t period, bool (*fn)(repeating_timer_t *),
                            void *user, repeating_timer_t *timer);
bool cancel_repeating_timer(repeating_timer_t *timer);
void adc_init(void);
void adc_gpio_init(uint pin);
void adc_set_temp_sensor_enabled(bool enabled);
void adc_select_input(uint channel);
uint16_t adc_read(void);
void adc_fifo_setup(bool enable, bool dreq, uint16_t threshold, bool error, bool shift);
void adc_fifo_drain(void);
void adc_set_clkdiv(float divider);
void adc_run(bool on);
int dma_claim_unused_channel(bool required);
dma_channel_config dma_channel_get_default_config(uint channel);
dma_channel_config dma_get_channel_config(uint channel);
void channel_config_set_transfer_data_size(dma_channel_config *cfg, dma_channel_transfer_size size);
void channel_config_set_read_increment(dma_channel_config *cfg, bool on);
void channel_config_set_write_increment(dma_channel_config *cfg, bool on);
void channel_config_set_dreq(dma_channel_config *cfg, uint dreq);
void channel_config_set_ring(dma_channel_config *cfg, bool write, uint bits);
void channel_config_set_chain_to(dma_channel_config *cfg, uint chain);
void dma_channel_configure(uint channel, const dma_channel_config *cfg, volatile void *write,
                           const volatile void *read, uint32_t count, bool trigger);
void dma_channel_start(uint channel);
void dma_channel_abort(uint channel);
bool dma_channel_is_busy(uint channel);
void dma_channel_set_config(uint channel, const dma_channel_config *cfg, bool trigger);
void dma_channel_set_write_addr(uint channel, volatile void *write, bool trigger);
bool dma_channel_get_irq0_status(uint channel);
bool dma_channel_get_irq1_status(uint channel);
void dma_channel_set_irq0_enabled(uint channel, bool enabled);
void dma_channel_set_irq1_enabled(uint channel, bool enabled);
void dma_channel_acknowledge_irq0(uint channel);
void dma_channel_acknowledge_irq1(uint channel);
void irq_set_exclusive_handler(uint irq, void (*handler)(void));
void irq_set_enabled(uint irq, bool enabled);
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t previous);
uint clock_get_hz(uint clock);
uint pwm_gpio_to_slice_num(uint pin);
void pwm_set_clkdiv(uint slice, float divider);
void pwm_set_wrap(uint slice, uint16_t wrap);
void pwm_set_gpio_level(uint pin, uint16_t level);
void pwm_set_enabled(uint slice, bool enabled);
bool watchdog_enable_caused_reboot(void);
void watchdog_enable(uint32_t timeout_ms, bool pause_on_debug);
void watchdog_update(void);
