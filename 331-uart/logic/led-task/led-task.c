#include "led-task/led-task.h"

#include "platform/platform.h"
#include "systime/systime.h"

static led_state_t led_state;
static uint32_t blink_period_ms;
static uint64_t last_toggle_us;
static bool led_on;

static uint64_t half_period_us(void)
{
    // Умножение до деления: период 1 мс даёт полупериод 500 мкс.
    return (uint64_t)blink_period_ms * 1000 / 2;
}

void led_task_init(void)
{
    blink_period_ms = 1000;
    led_on = false;
    led_task_set_state(LED_STATE_BLINK);
}

void led_task_set_state(led_state_t state)
{
    switch (state)
    {
    case LED_STATE_OFF:
        led_on = false;
        break;
    case LED_STATE_ON:
        led_on = true;
        break;
    case LED_STATE_BLINK:
        led_on = !led_on;
        last_toggle_us = systime_us();
        break;
    default:
        return;
    }
    led_state = state;
    // Первое переключение при входе в мигание выполняется сразу.
    platform_led_set(led_on);
}

void led_task_handle(void)
{
    switch (led_state)
    {
    case LED_STATE_BLINK:
        if (systime_period_elapsed(&last_toggle_us, half_period_us()))
        {
            led_on = !led_on;
            platform_led_set(led_on);
        }
        break;
    case LED_STATE_OFF:
    case LED_STATE_ON:
        break;
    }
}

led_state_t led_task_get_state(void)
{
    return led_state;
}

bool led_task_set_period_ms(uint32_t period_ms)
{
    if (period_ms == 0)
    {
        return false;
    }
    blink_period_ms = period_ms;
    last_toggle_us = systime_us();
    return true;
}

uint32_t led_task_get_period_ms(void)
{
    return blink_period_ms;
}

void led_task_next_state(void)
{
    switch (led_state)
    {
    case LED_STATE_OFF: led_task_set_state(LED_STATE_ON); break;
    case LED_STATE_ON: led_task_set_state(LED_STATE_BLINK); break;
    case LED_STATE_BLINK: led_task_set_state(LED_STATE_OFF); break;
    }
}
