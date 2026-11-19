#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    PI_NOT_STARTED,
    PI_RUNNING,
    PI_READY,
} pi_state_t;

void pi_task_init(void);
bool pi_task_start(uint32_t terms);
void pi_task_handle(void);
pi_state_t pi_task_get_state(void);
bool pi_task_get_result(double *result, uint64_t *elapsed_ms);
