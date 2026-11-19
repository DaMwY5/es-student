#include "block-task/block-task.h"
#include "platform/platform.h"
#include <stddef.h>

static block_task_callback_t callback;
static uint32_t block_count;

void block_task_init(void) { callback = NULL; }

bool block_task_start(uint32_t count, block_task_callback_t on_block)
{
    if (callback || !on_block || !platform_block_start(count)) return false;
    block_count = count;
    callback = on_block;
    return true;
}

void block_task_handle(void)
{
    if (!callback || !platform_block_done()) return;
    block_task_callback_t on_block = callback;
    callback = NULL;
    on_block(platform_block_samples(), block_count, platform_block_time_us());
}
