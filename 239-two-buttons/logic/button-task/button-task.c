#include "button-task/button-task.h"

#include <stddef.h>
#include "platform/platform.h"
#include "systime/systime.h"

typedef enum
{
    BUTTON_RELEASED,
    BUTTON_PRESS_PENDING,
    BUTTON_PRESSED,
    BUTTON_RELEASE_PENDING,
} button_state_t;

static const uint64_t DEBOUNCE_US = 20000;
static const uint64_t LONG_PRESS_US = 1000000;
typedef struct
{
    button_state_t state;
    uint64_t changed_us;
    uint32_t press_count;
    button_task_callback_t on_press;
    button_task_callback_t on_long_press;
    uint64_t pressed_us;
    bool long_reported;
} button_t;

static button_t buttons[PLATFORM_BUTTON_COUNT];

void button_task_init(platform_button_t button, button_task_callback_t on_press,
                      button_task_callback_t on_long_press)
{
    if ((unsigned)button < PLATFORM_BUTTON_COUNT)
    {
        buttons[button] = (button_t){.state = BUTTON_RELEASED, .on_press = on_press,
                                    .on_long_press = on_long_press};
    }
}

static void handle_button(platform_button_t id)
{
    button_t *button = &buttons[id];
    bool pressed = platform_button_read(id);
    uint64_t now_us = systime_us();
    switch (button->state)
    {
    case BUTTON_RELEASED:
        if (pressed)
        {
            button->changed_us = now_us;
            button->state = BUTTON_PRESS_PENDING;
        }
        break;
    case BUTTON_PRESS_PENDING:
        if (!pressed)
        {
            button->state = BUTTON_RELEASED;
        }
        else if (now_us - button->changed_us >= DEBOUNCE_US)
        {
            button->state = BUTTON_PRESSED;
            button->press_count++;
            button->pressed_us = now_us;
            button->long_reported = false;
            if (button->on_long_press == NULL && button->on_press != NULL)
            {
                button->on_press();
            }
        }
        break;
    case BUTTON_PRESSED:
        if (!pressed)
        {
            button->changed_us = now_us;
            button->state = BUTTON_RELEASE_PENDING;
        }
        else if (button->on_long_press != NULL && !button->long_reported &&
                 now_us - button->pressed_us >= LONG_PRESS_US)
        {
            button->long_reported = true;
            button->on_long_press();
        }
        break;
    case BUTTON_RELEASE_PENDING:
        if (pressed)
        {
            button->state = BUTTON_PRESSED;
        }
        else if (now_us - button->changed_us >= DEBOUNCE_US)
        {
            button->state = BUTTON_RELEASED;
            if (button->on_long_press != NULL && !button->long_reported &&
                button->on_press != NULL)
            {
                button->on_press();
            }
        }
        break;
    }
}

void button_task_handle(void)
{
    for (platform_button_t id = 0; id < PLATFORM_BUTTON_COUNT; id++)
    {
        handle_button(id);
    }
}

bool button_task_is_pressed(platform_button_t button)
{
    return (unsigned)button < PLATFORM_BUTTON_COUNT &&
        (buttons[button].state == BUTTON_PRESSED ||
         buttons[button].state == BUTTON_RELEASE_PENDING);
}

uint32_t button_task_get_press_count(platform_button_t button)
{
    return (unsigned)button < PLATFORM_BUTTON_COUNT ? buttons[button].press_count : 0;
}
