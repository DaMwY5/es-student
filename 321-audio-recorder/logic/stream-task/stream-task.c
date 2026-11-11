#include "stream-task/stream-task.h"
#include "platform/platform.h"

_Static_assert(sizeof(stream_header_t) == 6, "Incorrect audio frame header layout");
static stream_state_t state;
static stream_task_callback_t callback;
static uint8_t stream_bits;
static uint32_t frames, samples_total;

void stream_task_init(stream_task_callback_t on_frame)
{
    callback = on_frame;
    state = STREAM_STATE_IDLE;
    stream_bits = 16;
    frames = samples_total = 0;
}

bool stream_task_start(uint8_t bits)
{
    if (state != STREAM_STATE_IDLE || !callback || !platform_stream_start(bits)) return false;
    stream_bits = bits;
    frames = samples_total = 0;
    state = STREAM_STATE_RUN;
    return true;
}

void stream_task_stop(void)
{
    if (state == STREAM_STATE_RUN) platform_stream_stop();
    state = STREAM_STATE_IDLE;
}

void stream_task_handle(void)
{
    if (state != STREAM_STATE_RUN) return;
    uint32_t samples;
    const uint8_t *data = platform_stream_next(&samples);
    if (!data) return;
    stream_header_t header = {STREAM_SYNC_WORD, (uint16_t)platform_stream_sequence(),
                              (uint16_t)samples};
    callback(&header, data, samples * (stream_bits / 8));
    platform_stream_release();
    ++frames;
    samples_total += samples;
}

stream_state_t stream_task_get_state(void) { return state; }
uint8_t stream_task_get_bits(void) { return stream_bits; }
uint32_t stream_task_get_frames(void) { return frames; }
uint32_t stream_task_get_samples(void) { return samples_total; }
uint32_t stream_task_get_overruns(void) { return platform_stream_overruns(); }
