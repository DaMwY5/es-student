#include "memory.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "command.h"
#include "device.h"
#include "led.h"

extern char __flash_binary_start;
extern char __flash_binary_end;
extern char __boot2_start__;
extern char __boot2_end__;
extern char __etext;
extern char __data_start__;
extern char __data_end__;
extern char __bss_start__;
extern char __bss_end__;
extern char __HeapLimit;
extern char __StackBottom;
extern char __StackTop;

int main(void);
uint32_t data_variable = 100;
uint32_t bss_variable;

static void row(const char *name, uintptr_t start, uintptr_t end)
{
    printf("%-10s 0x%08x 0x%08x %8u\n", name,
           (unsigned)start, (unsigned)end, (unsigned)(end - start));
}

void mem_info(void)
{
    uintptr_t flash_end = XIP_BASE + PICO_FLASH_SIZE_BYTES;
    unsigned boot2 = (uintptr_t)&__boot2_end__ - (uintptr_t)&__boot2_start__;
    unsigned text = (uintptr_t)&__etext - (uintptr_t)&__boot2_end__;
    unsigned data = (uintptr_t)&__data_end__ - (uintptr_t)&__data_start__;
    unsigned bss = (uintptr_t)&__bss_end__ - (uintptr_t)&__bss_start__;
    unsigned image = (uintptr_t)&__flash_binary_end - (uintptr_t)&__flash_binary_start;
    unsigned heap = (uintptr_t)&__HeapLimit - (uintptr_t)&__bss_end__;
    unsigned stack = (uintptr_t)&__StackTop - (uintptr_t)&__StackBottom;
    unsigned free_flash = flash_end - (uintptr_t)&__flash_binary_end;

    printf("area       start      end        size\n");
    row("flash", XIP_BASE, flash_end);
    row("sram", SRAM_BASE, SRAM_BASE + 264 * 1024);
    row("rom", ROM_BASE, ROM_BASE + 16 * 1024);
    row("image", (uintptr_t)&__flash_binary_start, (uintptr_t)&__flash_binary_end);
    row("free", (uintptr_t)&__flash_binary_end, flash_end);
    row("boot2", (uintptr_t)&__boot2_start__, (uintptr_t)&__boot2_end__);
    row("text", (uintptr_t)&__boot2_end__, (uintptr_t)&__etext);
    row("data flash", (uintptr_t)&__etext, (uintptr_t)&__etext + data);
    row("data ram", (uintptr_t)&__data_start__, (uintptr_t)&__data_end__);
    row("bss", (uintptr_t)&__bss_start__, (uintptr_t)&__bss_end__);
    row("heap", (uintptr_t)&__bss_end__, (uintptr_t)&__HeapLimit);
    row("stack", (uintptr_t)&__StackBottom, (uintptr_t)&__StackTop);
    printf("\ntotal\n");
    printf("  flash image %8u = boot2 %u + text %u + data %u\n", image, boot2, text, data);
    printf("  flash free  %8u of %u\n", free_flash, (unsigned)PICO_FLASH_SIZE_BYTES);
    printf("  ram used    %8u = data %u + bss %u\n", data + bss, data, bss);
    printf("  ram free    %8u for heap and %u for stack\n", heap, stack);
}

void fw_info(void)
{
    data_variable++;
    bss_variable++;
    const uint16_t *main_code = (const uint16_t *)((uintptr_t)main & ~1u);
    const uint16_t *fw_code = (const uint16_t *)((uintptr_t)fw_info & ~1u);
    uint32_t stack_variable = 1946;
    uint32_t *heap_variable = malloc(sizeof(uint32_t));

    printf("object          address     value\n");
    printf("main            0x%08x  0x%04x\n", (unsigned)(uintptr_t)main, *main_code);
    printf("fw_info         0x%08x  0x%04x\n", (unsigned)(uintptr_t)fw_info, *fw_code);
    printf("commands        0x%08x\n", (unsigned)(uintptr_t)commands);
    for (uint i = 0; i < command_count; i++)
    {
        printf("- %-13s 0x%08x\n", commands[i].name,
               (unsigned)(uintptr_t)commands[i].handler);
    }
    printf("DEVICE_PROJECT  0x%08x  %s\n", (unsigned)(uintptr_t)DEVICE_PROJECT, DEVICE_PROJECT);
    printf("DEVICE_BOARD    0x%08x  %s\n", (unsigned)(uintptr_t)DEVICE_BOARD, DEVICE_BOARD);
    printf("DEVICE_NAME     0x%08x  %s\n", (unsigned)(uintptr_t)DEVICE_NAME, DEVICE_NAME);
    printf("data_variable   0x%08x  %u\n", (unsigned)(uintptr_t)&data_variable, (unsigned)data_variable);
    printf("bss_variable    0x%08x  %u\n", (unsigned)(uintptr_t)&bss_variable, (unsigned)bss_variable);
    printf("stack_variable  0x%08x  %u\n", (unsigned)(uintptr_t)&stack_variable, (unsigned)stack_variable);
    if (heap_variable != NULL)
    {
        *heap_variable = 1951;
        printf("heap_variable   0x%08x  %u\n", (unsigned)(uintptr_t)heap_variable, (unsigned)*heap_variable);
    }
    else
    {
        printf("heap_variable   NULL\n");
    }
    free(heap_variable);
}

void boot_info(void)
{
    const uint32_t *vectors = (const uint32_t *)0x10000100;
    uint32_t stack_top = vectors[0];
    uint32_t reset_handler = vectors[1];
    volatile uint32_t *gpio_in = (volatile uint32_t *)0xd0000004;
    uint32_t level = (*gpio_in >> led_pin()) & 1u;

    printf("vector table   0x%08x\n", (unsigned)(uintptr_t)vectors);
    printf("  stack top    0x%08x\n", (unsigned)stack_top);
    printf("  reset        0x%08x\n", (unsigned)reset_handler);
    printf("  reset (even) 0x%08x\n", (unsigned)(reset_handler & ~1u));
    printf("gpio in        0x%08x\n", (unsigned)(uintptr_t)gpio_in);
    printf("  led bit      %u\n", (unsigned)level);
    printf("  gpio_get     %u\n", (unsigned)gpio_get(led_pin()));
}
