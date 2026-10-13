#pragma once

#include <stdbool.h>

typedef enum
{
    PLATFORM_BUTTON_FIRST,
    PLATFORM_BUTTON_SECOND,
    PLATFORM_BUTTON_COUNT,
} platform_button_t;

void platform_init(void);
void platform_led_set(bool on);
bool platform_button_read(platform_button_t button);
