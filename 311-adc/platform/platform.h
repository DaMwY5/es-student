#pragma once

#include <stdbool.h>

void platform_init(void);
void platform_led_set(bool on);
bool platform_button_read(void);

// Напряжение на GP26 в вольтах, температура кристалла в градусах Цельсия.
float platform_voltage_read(void);
float platform_temp_read(void);
void platform_charge_set(bool on);
