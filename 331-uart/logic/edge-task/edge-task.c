#include "edge-task/edge-task.h"
#include "systime/systime.h"

#define EMPTY_V 0.05f
#define RISE_V 2.0f
#define FULL_V 3.2f
#define FALL_V 1.3f
#define TIMEOUT_US 2000000u

typedef enum { IDLE, DISCHARGE, RISE, CHARGE, FALL } edge_state_t;
static edge_state_t state;
static platform_line_mode_t line_mode;
static edge_task_callback_t callback;
static uint64_t phase_start;
static uint32_t rise_us;

static void enter(edge_state_t next, bool high)
{
    platform_line_set(line_mode, high);
    phase_start = systime_us();
    state = next;
}

static void finish(bool ok, uint32_t fall_us)
{
    platform_line_set(line_mode, false);
    state = IDLE;
    callback(line_mode, ok, rise_us, fall_us);
}

bool edge_task_start(platform_line_mode_t mode, edge_task_callback_t on_done)
{
    if (state != IDLE || !on_done) return false;
    line_mode = mode;
    callback = on_done;
    rise_us = 0;
    enter(DISCHARGE, false);
    return true;
}

bool edge_task_busy(void) { return state != IDLE; }

void edge_task_handle(void)
{
    if (state == IDLE) return;
    float voltage = platform_voltage_read();
    uint64_t elapsed = systime_us() - phase_start;
    if (elapsed >= TIMEOUT_US)
    {
        finish(false, 0);
        return;
    }
    switch (state)
    {
    case DISCHARGE:
        if (voltage < EMPTY_V) enter(RISE, true);
        break;
    case RISE:
        if (voltage >= RISE_V)
        {
            rise_us = (uint32_t)elapsed;
            phase_start = systime_us();
            state = CHARGE;
        }
        break;
    case CHARGE:
        if (voltage >= FULL_V) enter(FALL, false);
        break;
    case FALL:
        if (voltage <= FALL_V) finish(true, (uint32_t)elapsed);
        break;
    case IDLE:
        break;
    }
}
