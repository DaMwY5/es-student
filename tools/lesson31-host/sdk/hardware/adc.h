#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdlib.h"
void adc_init(void);
void adc_gpio_init(uint pin);
void adc_set_temp_sensor_enabled(bool enabled);
void adc_select_input(uint input);
uint16_t adc_read(void);
