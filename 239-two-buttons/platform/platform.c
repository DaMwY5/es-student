#include "platform/platform.h"

#include "pico/stdlib.h"

static const uint LED_PIN = PICO_DEFAULT_LED_PIN;
static const uint BUTTON_PINS[PLATFORM_BUTTON_COUNT] = {15, 14};

void platform_init(void)
{
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);
    for (uint i = 0; i < PLATFORM_BUTTON_COUNT; i++)
    {
        gpio_init(BUTTON_PINS[i]);
        gpio_set_dir(BUTTON_PINS[i], GPIO_IN);
        gpio_pull_up(BUTTON_PINS[i]);
    }
}

void platform_led_set(bool on)
{
    gpio_put(LED_PIN, on);
}

bool platform_button_read(platform_button_t button)
{
    return (unsigned)button < PLATFORM_BUTTON_COUNT && !gpio_get(BUTTON_PINS[button]);
}
