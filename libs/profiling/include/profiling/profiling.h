#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PROFILING_STOPWATCH_COUNT 8

#ifndef PROFILING_ENABLED
#define PROFILING_ENABLED 1
#endif

typedef struct
{
    const char *name;
    uint32_t count;
    double mean_us;
    uint32_t max_us;
} profiling_result_t;

// Сумма хранится в uint32_t: переполнение после 4294967295 мкс
// накопленного времени (около 71 минуты). До этого нужно сбросить результаты.
void profiling_stopwatch_init(uint32_t id, const char *name);
#if PROFILING_ENABLED
void profiling_start(uint32_t id);
void profiling_stop(uint32_t id);
#else
#define profiling_start(id) ((void)0)
#define profiling_stop(id) ((void)0)
#endif
bool profiling_get(uint32_t id, profiling_result_t *result);
void profiling_reset(void);
