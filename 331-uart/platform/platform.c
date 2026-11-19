#include "platform/platform.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define LINE_PIN 22
#define ADC_PIN 26
#define ADC_CHANNEL 0
#define UART0_TX_PIN 0
#define UART0_RX_PIN 1
#define UART1_TX_PIN 8
#define UART1_RX_PIN 9
#define UART_BAUD 115200u
#define UART_FIFO_SIZE 32u
#define RX_BUFFER_SIZE 128u

static const uint LED_PIN = PICO_DEFAULT_LED_PIN;
static const uint BUTTON_PIN = 15;

static volatile uint8_t rx_buffer[RX_BUFFER_SIZE];
static volatile size_t rx_count;
static size_t rx_read;
static volatile uint32_t rx_errors;

static int block_channel;
static uint16_t block_samples[PLATFORM_BLOCK_MAX];
static uint32_t block_start_us;
static volatile uint32_t block_end_us;
static volatile bool block_active, block_complete;

static uart_inst_t *uart_of(platform_uart_t uart)
{
    return uart == PLATFORM_UART0 ? uart0 : uart1;
}

static void on_uart0_rx(void)
{
    uart_hw_t *hw = uart_get_hw(uart0);
    for (size_t i = 0; i < UART_FIFO_SIZE && uart_is_readable(uart0); ++i)
    {
        uint32_t word = hw->dr;
        rx_errors |= (word >> 8) & 0xf;
        if (rx_count < RX_BUFFER_SIZE)
            rx_buffer[rx_count++] = (uint8_t)word;
        else
            rx_errors |= PLATFORM_UART_OE;
    }
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

void platform_init(void)
{
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    gpio_init(LINE_PIN);
    platform_line_set(PLATFORM_LINE_PUSH_PULL, false);
    adc_init();
    adc_gpio_init(ADC_PIN);

    uart_init(uart0, UART_BAUD);
    uart_init(uart1, UART_BAUD);
    platform_uart_loop_set(false);
    gpio_set_function(UART1_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART1_RX_PIN, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, on_uart0_rx);
    irq_set_enabled(UART0_IRQ, true);

    block_channel = dma_claim_unused_channel(true);
    dma_channel_set_irq1_enabled(block_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_1, on_block_done);
    irq_set_enabled(DMA_IRQ_1, true);
}

void platform_led_set(bool on)
{
    gpio_put(LED_PIN, on);
}

bool platform_button_read(void)
{
    return !gpio_get(BUTTON_PIN);
}

void platform_line_set(platform_line_mode_t mode, bool high)
{
    if (mode == PLATFORM_LINE_PUSH_PULL)
    {
        gpio_disable_pulls(LINE_PIN);
        gpio_put(LINE_PIN, high);
        gpio_set_dir(LINE_PIN, GPIO_OUT);
    }
    else
    {
        gpio_put(LINE_PIN, false);
        gpio_set_dir(LINE_PIN, high ? GPIO_IN : GPIO_OUT);
        gpio_pull_up(LINE_PIN);
    }
}

float platform_voltage_read(void)
{
    adc_select_input(ADC_CHANNEL);
    return adc_read() * (3.3f / 4096.0f);
}

void platform_uart_loop_set(bool internal)
{
    uint function = internal ? GPIO_FUNC_NULL : GPIO_FUNC_UART;
    gpio_set_function(UART0_TX_PIN, function);
    gpio_set_function(UART0_RX_PIN, function);
    if (internal)
        hw_set_bits(&uart_get_hw(uart0)->cr, UART_UARTCR_LBE_BITS);
    else
        hw_clear_bits(&uart_get_hw(uart0)->cr, UART_UARTCR_LBE_BITS);
}

void platform_uart_rx_irq_set(bool on)
{
    uart_set_irqs_enabled(uart0, on, false);
}

void platform_uart_clear(platform_uart_t uart)
{
    uart_inst_t *u = uart_of(uart);
    uart_hw_t *hw = uart_get_hw(u);
    uint32_t irq_state = save_and_disable_interrupts();
    for (size_t i = 0; i < UART_FIFO_SIZE && uart_is_readable(u); ++i)
        (void)hw->dr;
    hw->rsr = 0;
    if (uart == PLATFORM_UART0)
    {
        rx_count = rx_read = 0;
        rx_errors = 0;
    }
    restore_interrupts(irq_state);
}

bool platform_uart_put(platform_uart_t uart, uint8_t byte)
{
    uart_inst_t *u = uart_of(uart);
    if (!uart_is_writable(u)) return false;
    uart_get_hw(u)->dr = byte;
    return true;
}

bool platform_uart_busy(platform_uart_t uart)
{
    return (uart_get_hw(uart_of(uart))->fr & UART_UARTFR_BUSY_BITS) != 0;
}

size_t platform_uart_read(platform_uart_t uart, uint8_t *data, size_t size)
{
    uart_inst_t *u = uart_of(uart);
    uart_hw_t *hw = uart_get_hw(u);
    uint32_t irq_state = save_and_disable_interrupts();
    size_t n = 0;
    if (uart == PLATFORM_UART0)
        while (n < size && rx_read < rx_count)
            data[n++] = rx_buffer[rx_read++];
    while (n < size && uart_is_readable(u))
    {
        uint32_t word = hw->dr;
        data[n++] = (uint8_t)word;
        if (uart == PLATFORM_UART0) rx_errors |= (word >> 8) & 0xf;
    }
    restore_interrupts(irq_state);
    return n;
}

uint32_t platform_uart_errors(platform_uart_t uart)
{
    uint32_t errors = uart_get_hw(uart_of(uart))->rsr & 0xf;
    return uart == PLATFORM_UART0 ? errors | rx_errors : errors;
}

uint32_t platform_uart_baud_set(platform_uart_t uart, uint32_t baud)
{
    return uart_set_baudrate(uart_of(uart), baud);
}

void platform_uart_parity_set(platform_uart_t uart, platform_parity_t parity)
{
    static const uart_parity_t parities[] = {
        UART_PARITY_NONE, UART_PARITY_EVEN, UART_PARITY_ODD,
    };
    uart_set_format(uart_of(uart), 8, 1, parities[parity]);
}

bool platform_block_start(uint32_t count)
{
    if (block_active || count == 0 || count > PLATFORM_BLOCK_MAX) return false;
    adc_select_input(ADC_CHANNEL);
    adc_fifo_drain();
    adc_fifo_setup(true, true, 1, false, false);
    // Continuous conversions at the hardware limit of 96 ADC clocks per sample.
    adc_set_clkdiv(0);
    dma_channel_config cfg = dma_channel_get_default_config(block_channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);
    block_complete = false;
    block_active = true;
    dma_channel_configure(block_channel, &cfg, block_samples, &adc_hw->fifo, count, true);
    block_start_us = time_us_32();
    adc_run(true);
    return true;
}

bool platform_block_done(void)
{
    if (!block_complete) return false;
    __dmb();
    if (block_active)
    {
        adc_fifo_drain();
        adc_fifo_setup(false, false, 0, false, false);
        block_active = false;
    }
    return true;
}

const uint16_t *platform_block_samples(void) { return block_samples; }
uint32_t platform_block_time_us(void) { return block_end_us - block_start_us; }
