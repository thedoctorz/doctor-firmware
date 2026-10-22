#include "subghz_worker.h"

#include <furi.h>

#define TAG "SubGhzWorker"

#define SUBGHZ_WORKER_BATCH_SAMPLES 32
#define SUBGHZ_WORKER_RUN_TICKS     10

struct SubGhzWorker {
    FuriThread* thread;
    FuriStreamBuffer* stream;

    volatile bool running;
    volatile bool overrun;
    volatile uint32_t overrun_count;
    uint32_t overrun_log_stamp;
    volatile uint32_t edge_count;

    volatile uint32_t write_seq;
    uint32_t read_seq;
    volatile uint32_t retune_req;
    uint32_t retune_done;
    uint32_t retune_discard_until;
    uint32_t retune_payload;

    LevelDuration filter_level_duration;
    uint16_t filter_duration;

    SubGhzWorkerOverrunCallback overrun_callback;
    SubGhzWorkerPairCallback pair_callback;
    SubGhzWorkerBarrierCallback barrier_callback;
    void* context;
};

/** Rx callback timer
 * 
 * @param level received signal level
 * @param duration received signal duration
 * @param context 
 */
void subghz_worker_rx_callback(bool level, uint32_t duration, void* context) {
    SubGhzWorker* instance = context;

    instance->edge_count++;

    LevelDuration level_duration = level_duration_make(level, duration);
    if(instance->overrun) {
        instance->overrun = false;
        level_duration = level_duration_reset();
    }
    size_t ret =
        furi_stream_buffer_send(instance->stream, &level_duration, sizeof(LevelDuration), 0);
    if(sizeof(LevelDuration) != ret) {
        instance->overrun = true;
        instance->overrun_count++;
    } else {
        instance->write_seq++;
    }
}

static bool subghz_worker_retune_pending(SubGhzWorker* instance) {
    if(instance->retune_req == instance->retune_done) return false;

    FURI_CRITICAL_ENTER();
    const uint32_t req = instance->retune_req;
    const uint32_t until = instance->retune_discard_until;
    const uint32_t payload = instance->retune_payload;
    FURI_CRITICAL_EXIT();

    if(req == instance->retune_done) return false;
    if((int32_t)(until - instance->read_seq) > 0) return true;

    instance->retune_done = req;
    instance->filter_level_duration.level = LEVEL_DURATION_RESET;
    instance->filter_level_duration.duration = 0;
    if(instance->overrun_callback) instance->overrun_callback(instance->context);
    if(instance->barrier_callback) instance->barrier_callback(instance->context, payload);
    return false;
}

/** Worker callback thread
 * 
 * @param context 
 * @return exit code 
 */
static int32_t subghz_worker_thread_callback(void* context) {
    SubGhzWorker* instance = context;

    LevelDuration batch[SUBGHZ_WORKER_BATCH_SAMPLES];
    uint32_t run_start = furi_get_tick();
    while(instance->running) {
        subghz_worker_retune_pending(instance);
        size_t ret = furi_stream_buffer_receive(instance->stream, batch, sizeof(batch), 10);
        for(size_t i = 0; i < ret / sizeof(LevelDuration) && instance->running; i++) {
            const LevelDuration level_duration = batch[i];
            const bool stale = subghz_worker_retune_pending(instance);
            instance->read_seq++;
            if(stale) continue;
            if(level_duration_is_wait(level_duration)) {
                instance->filter_level_duration.level = LEVEL_DURATION_RESET;
                instance->filter_level_duration.duration = 0;

                if(instance->overrun_callback) instance->overrun_callback(instance->context);

                if(instance->barrier_callback) {
                    instance->barrier_callback(
                        instance->context, level_duration_get_duration(level_duration));
                }
            } else if(level_duration_is_reset(level_duration)) {
                const uint32_t now = furi_get_tick();
                if(now - instance->overrun_log_stamp >= furi_ms_to_ticks(1000)) {
                    instance->overrun_log_stamp = now;
                    FURI_LOG_E(TAG, "Overrun buffer (%lu total)", instance->overrun_count);
                }
                if(instance->overrun_callback) instance->overrun_callback(instance->context);
            } else {
                bool level = level_duration_get_level(level_duration);
                uint32_t duration = level_duration_get_duration(level_duration);

                if((duration < instance->filter_duration) ||
                   (instance->filter_level_duration.level == level)) {
                    instance->filter_level_duration.duration += duration;

                } else if(instance->filter_level_duration.level != level) {
                    if(instance->pair_callback)
                        instance->pair_callback(
                            instance->context,
                            instance->filter_level_duration.level,
                            instance->filter_level_duration.duration);

                    instance->filter_level_duration.duration = duration;
                    instance->filter_level_duration.level = level;
                }
            }
        }
        
        const uint32_t now = furi_get_tick();
        if(now - run_start >= SUBGHZ_WORKER_RUN_TICKS) {
            furi_delay_tick(1);
            run_start = furi_get_tick();
        }
    }

    return 0;
}

SubGhzWorker* subghz_worker_alloc(void) {
    return subghz_worker_alloc_ex(4096);
}

SubGhzWorker* subghz_worker_alloc_ex(size_t stream_samples) {
    SubGhzWorker* instance = malloc(sizeof(SubGhzWorker));

    instance->thread =
        furi_thread_alloc_ex("SubGhzWorker", 2048, subghz_worker_thread_callback, instance);

    instance->stream =
        furi_stream_buffer_alloc(sizeof(LevelDuration) * stream_samples, sizeof(LevelDuration));

    instance->running = false;
    instance->overrun = false;
    instance->overrun_count = 0;
    instance->overrun_log_stamp = 0;
    instance->edge_count = 0;
    instance->write_seq = 0;
    instance->read_seq = 0;
    instance->retune_req = 0;
    instance->retune_done = 0;
    instance->retune_discard_until = 0;
    instance->retune_payload = 0;
    instance->barrier_callback = NULL;
    instance->filter_level_duration.level = LEVEL_DURATION_RESET;
    instance->filter_level_duration.duration = 0;

    instance->filter_duration = 30;

    return instance;
}

void subghz_worker_free(SubGhzWorker* instance) {
    furi_check(instance);

    furi_stream_buffer_free(instance->stream);
    furi_thread_free(instance->thread);

    free(instance);
}

void subghz_worker_set_overrun_callback(
    SubGhzWorker* instance,
    SubGhzWorkerOverrunCallback callback) {
    furi_check(instance);
    instance->overrun_callback = callback;
}

void subghz_worker_set_pair_callback(SubGhzWorker* instance, SubGhzWorkerPairCallback callback) {
    furi_check(instance);
    instance->pair_callback = callback;
}

void subghz_worker_set_context(SubGhzWorker* instance, void* context) {
    furi_check(instance);
    instance->context = context;
}

void subghz_worker_start(SubGhzWorker* instance) {
    furi_check(instance);
    furi_check(!instance->running);

    instance->running = true;

    furi_thread_start(instance->thread);
}

void subghz_worker_stop(SubGhzWorker* instance) {
    furi_check(instance);
    furi_check(instance->running);

    instance->running = false;

    furi_thread_join(instance->thread);
}

bool subghz_worker_is_running(SubGhzWorker* instance) {
    furi_check(instance);
    return instance->running;
}

void subghz_worker_set_barrier_callback(
    SubGhzWorker* instance,
    SubGhzWorkerBarrierCallback callback) {
    furi_check(instance);
    instance->barrier_callback = callback;
}

void subghz_worker_flush(SubGhzWorker* instance, uint32_t payload) {
    furi_check(instance);

    LevelDuration marker = level_duration_wait();
    marker.duration = payload & 0x3FFFFFFFU;
    furi_stream_buffer_send(instance->stream, &marker, sizeof(LevelDuration), 0);
}

void subghz_worker_retune(SubGhzWorker* instance, uint32_t payload) {
    furi_check(instance);

    FURI_CRITICAL_ENTER();
    instance->retune_payload = payload & 0x3FFFFFFFU;
    instance->retune_discard_until = instance->write_seq;
    instance->retune_req++;
    FURI_CRITICAL_EXIT();
}

uint32_t subghz_worker_get_edge_count(SubGhzWorker* instance) {
    furi_check(instance);
    return instance->edge_count;
}

uint32_t subghz_worker_get_overrun_count(SubGhzWorker* instance) {
    furi_check(instance);
    return instance->overrun_count;
}

void subghz_worker_set_filter(SubGhzWorker* instance, uint16_t timeout) {
    furi_check(instance);
    instance->filter_duration = timeout;
}
