#include "profiling/profiling.h"

#include <stddef.h>
#if PROFILING_ENABLED
#include "pico/time.h"

typedef struct
{
    const char *name;
    uint32_t count;
    uint32_t sum_us;
    uint32_t max_us;
    uint32_t started_us;
    bool running;
} stopwatch_t;

static stopwatch_t stopwatches[PROFILING_STOPWATCH_COUNT];

void profiling_stopwatch_init(uint32_t id, const char *name)
{
    if (id < PROFILING_STOPWATCH_COUNT)
    {
        stopwatches[id] = (stopwatch_t){.name = name};
    }
}

void profiling_start(uint32_t id)
{
    if (id < PROFILING_STOPWATCH_COUNT && stopwatches[id].name != NULL)
    {
        stopwatches[id].started_us = time_us_32();
        stopwatches[id].running = true;
    }
}

void profiling_stop(uint32_t id)
{
    if (id >= PROFILING_STOPWATCH_COUNT || !stopwatches[id].running)
    {
        return;
    }
    stopwatch_t *watch = &stopwatches[id];
    uint32_t elapsed_us = time_us_32() - watch->started_us;
    watch->running = false;
    watch->count++;
    watch->sum_us += elapsed_us;
    if (elapsed_us > watch->max_us)
    {
        watch->max_us = elapsed_us;
    }
}

bool profiling_get(uint32_t id, profiling_result_t *result)
{
    if (id >= PROFILING_STOPWATCH_COUNT || result == NULL ||
        stopwatches[id].name == NULL)
    {
        return false;
    }
    const stopwatch_t *watch = &stopwatches[id];
    *result = (profiling_result_t){
        .name = watch->name,
        .count = watch->count,
        .mean_us = watch->count ? (double)watch->sum_us / watch->count : 0.0,
        .max_us = watch->max_us,
    };
    return true;
}

void profiling_reset(void)
{
    uint32_t now_us = time_us_32();
    for (uint32_t id = 0; id < PROFILING_STOPWATCH_COUNT; id++)
    {
        stopwatches[id].count = 0;
        stopwatches[id].sum_us = 0;
        stopwatches[id].max_us = 0;
        // Сброс может прийти внутри измеряемого обработчика команды.
        stopwatches[id].started_us = now_us;
    }
}
#else
void profiling_stopwatch_init(uint32_t id, const char *name)
{
    (void)id;
    (void)name;
}

bool profiling_get(uint32_t id, profiling_result_t *result)
{
    (void)id;
    (void)result;
    return false;
}

void profiling_reset(void)
{
}
#endif
