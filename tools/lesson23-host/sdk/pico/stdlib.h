#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pico/time.h"
typedef unsigned int uint;
#define PICO_ERROR_TIMEOUT (-1)
#define PICO_DEFAULT_LED_PIN 25
#define GPIO_IN 0
#define GPIO_OUT 1
void stdio_init_all(void);
int getchar_timeout_us(uint32_t timeout);
void gpio_init(uint pin);
void gpio_set_dir(uint pin, bool out);
void gpio_put(uint pin, bool on);
bool gpio_get(uint pin);
void gpio_pull_up(uint pin);
