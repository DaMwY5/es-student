/* Virtual peripherals for exercising the real 331-uart command and task code. */
#include "api/api.h"
#include "block-task/block-task.h"
#include "edge-task/edge-task.h"
#include "uart-task/uart-task.h"
#include "platform/platform.h"
#include "stdio-text-protocol/stdio-text-protocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIFO = 32, MAX_FRAMES = UART_TASK_MAX };
typedef enum { RC, PINS_LOOP, CROSSED, UNWIRED } wiring_t;
typedef struct {
    double start, bit_us;
    unsigned bits, length;
} frame_t;
typedef struct {
    frame_t queue[FIFO];
    unsigned head, tx_count;
    double bit_us, tx_end;
    uint8_t received[128];
    size_t rx_count, rx_read;
    uint32_t errors;
    platform_parity_t parity;
} port_t;

static port_t ports[2];
static wiring_t wiring;
static bool internal_loop, rx_irq;
static int input_symbol = -1;
static uint64_t now_us, line_changed_us;
static bool line_high;
static double line_tau_us = 4700.0, line_start_v;
static uint64_t capture_start;
static uint32_t capture_count;
static uint16_t scope_samples[PLATFORM_BLOCK_MAX];
static frame_t scope_frames[MAX_FRAMES];
static size_t scope_frames_count;
uint32_t host_chip_id = 0x20002927;

uint64_t time_us_64(void) { return now_us; }
uint32_t time_us_32(void) { return (uint32_t)now_us; }

void pico_get_unique_board_id_string(char *serial, size_t size)
{
    assert(size >= 17);
    memcpy(serial, "0000000000000000", 17);
}

int getchar_timeout_us(uint32_t timeout)
{
    assert(timeout == 0);
    int result = input_symbol;
    input_symbol = -1;
    return result;
}

static double line_voltage(void)
{
    if (wiring != RC) return 0;
    double target = line_high ? 3.3 : 0;
    return target + (line_start_v - target) * exp(-(double)(now_us - line_changed_us) / line_tau_us);
}

void platform_line_set(platform_line_mode_t mode, bool high)
{
    line_start_v = line_voltage();
    line_changed_us = now_us;
    line_high = high;
    // 1 kΩ series, assumed 50 kΩ pull-up, 4.7 µF capacitor.
    line_tau_us = high && mode == PLATFORM_LINE_OPEN_DRAIN ? 51 * 4700.0 : 4700.0;
}

float platform_voltage_read(void)
{
    return (float)(floor(line_voltage() * 4096 / 3.3) * (3.3 / 4096));
}

void platform_init(void)
{
    platform_line_set(PLATFORM_LINE_PUSH_PULL, false);
    platform_uart_baud_set(PLATFORM_UART0, PLATFORM_UART_BAUD);
    platform_uart_baud_set(PLATFORM_UART1, PLATFORM_UART_BAUD);
}
void platform_led_set(bool on) { (void)on; }
bool platform_button_read(void) { return false; }

void platform_uart_loop_set(bool internal) { internal_loop = internal; }
void platform_uart_rx_irq_set(bool on) { rx_irq = on; }

void platform_uart_clear(platform_uart_t uart)
{
    ports[uart].rx_count = ports[uart].rx_read = 0;
    ports[uart].errors = 0;
}

static unsigned parity_bit(unsigned byte, platform_parity_t parity)
{
    unsigned value = parity == PLATFORM_PARITY_ODD;
    for (unsigned i = 0; i < 8; ++i) value ^= (byte >> i) & 1;
    return value;
}

bool platform_uart_put(platform_uart_t uart, uint8_t byte)
{
    port_t *port = &ports[uart];
    if (port->tx_count == FIFO) return false;
    double start = fmax((double)now_us, port->tx_end);
    unsigned length = 10;
    unsigned bits = (unsigned)byte << 1;
    if (port->parity != PLATFORM_PARITY_NONE)
    {
        bits |= parity_bit(byte, port->parity) << 9;
        ++length;
    }
    bits |= 1u << (length - 1);
    frame_t frame = {start, port->bit_us, bits, length};
    port->tx_end = start + length * port->bit_us;
    port->queue[(port->head + port->tx_count++) % FIFO] = frame;
    if (uart == PLATFORM_UART0 && capture_count && !internal_loop)
    {
        assert(scope_frames_count < MAX_FRAMES);
        scope_frames[scope_frames_count++] = frame;
    }
    return true;
}

bool platform_uart_busy(platform_uart_t uart) { return ports[uart].tx_count != 0; }

size_t platform_uart_read(platform_uart_t uart, uint8_t *data, size_t size)
{
    port_t *port = &ports[uart];
    size_t remaining = port->rx_count - port->rx_read;
    if (size > remaining) size = remaining;
    memcpy(data, port->received + port->rx_read, size);
    port->rx_read += size;
    return size;
}

uint32_t platform_uart_errors(platform_uart_t uart) { return ports[uart].errors; }

uint32_t platform_uart_baud_set(platform_uart_t uart, uint32_t baud)
{
    // The SDK programs a 16.6 bit divisor, with a nominal 125 MHz UART clock.
    unsigned divisor = (1000000000u / baud + 1) >> 1;
    ports[uart].bit_us = divisor / 500.0;
    return 500000000u / divisor;
}

void platform_uart_parity_set(platform_uart_t uart, platform_parity_t parity)
{
    ports[uart].parity = parity;
}

static unsigned sample_frame(frame_t frame, double time)
{
    double offset = (time - frame.start) / frame.bit_us;
    return offset < 0 || offset >= frame.length ? 1 : (frame.bits >> (unsigned)offset) & 1;
}

static void deliver(platform_uart_t rx, frame_t frame)
{
    port_t *port = &ports[rx];
    unsigned decoded = 0;
    for (unsigned bit = 0; bit < 8; ++bit)
        decoded |= sample_frame(frame, frame.start + (1.5 + bit) * port->bit_us) << bit;
    double stop = 9.5;
    if (port->parity != PLATFORM_PARITY_NONE)
    {
        if (parity_bit(decoded, port->parity) !=
            sample_frame(frame, frame.start + 9.5 * port->bit_us))
            port->errors |= PLATFORM_UART_PE;
        stop += 1;
    }
    if (!sample_frame(frame, frame.start + stop * port->bit_us))
        port->errors |= PLATFORM_UART_FE;
    size_t capacity = rx == PLATFORM_UART0 && rx_irq ? sizeof port->received : FIFO;
    if (port->rx_count < capacity)
        port->received[port->rx_count++] = (uint8_t)decoded;
    else
        port->errors |= PLATFORM_UART_OE;
}

static void tick_uart(void)
{
    for (unsigned tx = 0; tx < 2; ++tx)
    {
        port_t *port = &ports[tx];
        while (port->tx_count)
        {
            frame_t frame = port->queue[port->head];
            if (frame.start + frame.bit_us * frame.length > now_us) break;
            port->head = (port->head + 1) % FIFO;
            --port->tx_count;
            if (tx == PLATFORM_UART0 && (internal_loop || wiring == PINS_LOOP))
                deliver(PLATFORM_UART0, frame);
            else if (!internal_loop && wiring == CROSSED)
                deliver((platform_uart_t)(tx ^ 1), frame);
        }
    }
}

bool platform_block_start(uint32_t count)
{
    if (capture_count || !count || count > PLATFORM_BLOCK_MAX) return false;
    capture_count = count;
    capture_start = now_us;
    scope_frames_count = 0;
    return true;
}

bool platform_block_done(void)
{
    if (!capture_count || now_us - capture_start < capture_count * 2u) return false;
    size_t frame = 0;
    for (uint32_t i = 0; i < capture_count; ++i)
    {
        double time = (double)capture_start + i * 2u;
        while (frame < scope_frames_count &&
               time >= scope_frames[frame].start + scope_frames[frame].bit_us * scope_frames[frame].length)
            ++frame;
        unsigned high = frame == scope_frames_count ? 1 : sample_frame(scope_frames[frame], time);
        scope_samples[i] = high ? 4095 : 0;
    }
    capture_count = 0;
    return true;
}

const uint16_t *platform_block_samples(void) { return scope_samples; }
uint32_t platform_block_time_us(void) { return (uint32_t)(now_us - capture_start); }

static void iteration(void)
{
    tick_uart();
    const command_t *command = stdio_text_protocol_handle();
    if (command) api_handle(command);
    edge_task_handle();
    api_scope_handle();
    uart_task_handle();
    block_task_handle();
    now_us += 4;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    wiring = strcmp(argv[1], "rc") == 0 ? RC : strcmp(argv[1], "loop") == 0 ? PINS_LOOP :
             strcmp(argv[1], "crossed") == 0 ? CROSSED : UNWIRED;
    platform_init();
    stdio_text_protocol_init();
    block_task_init();
    char line[256];
    while (fgets(line, sizeof line, stdin))
    {
        char *cursor;
        uint64_t wait_us = strtoull(line, &cursor, 10);
        assert(*cursor++ == ' ');
        size_t n = strcspn(cursor, "\r\n");
        assert(n % 2 == 0);
        fprintf(stdout, "@SEND %llu %.*s\n", (unsigned long long)now_us, (int)n, cursor);
        uint64_t deadline = now_us + wait_us;
        for (size_t i = 0; i < n; i += 2)
        {
            char byte[] = {cursor[i], cursor[i + 1], '\0'};
            input_symbol = (int)strtoul(byte, NULL, 16);
            iteration();
        }
        while (now_us < deadline) iteration();
    }
    return 0;
}
