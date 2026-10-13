#include "systime/systime.h"

#include <stddef.h>
#include "pico/time.h"

uint64_t systime_us(void)
{
    return time_us_64();
}

uint64_t systime_ms(void)
{
    return systime_us() / 1000;
}

bool systime_period_elapsed(uint64_t *last_us, uint64_t period_us)
{
    if (last_us == NULL || period_us == 0)
    {
        return false;
    }
    uint64_t now_us = systime_us();
    if (now_us - *last_us < period_us)
    {
        return false;
    }
    *last_us += period_us;
    if (now_us - *last_us >= period_us)
    {
        // Не исполняем очередь пропущенных срабатываний.
        *last_us = now_us;
    }
    return true;
}
