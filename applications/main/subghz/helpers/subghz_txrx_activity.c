#include "subghz_txrx_activity.h"
#include "subghz_txrx_i.h"
#include <string.h>

#define ACTIVITY_SAMPLE_MS     10u
#define ACTIVITY_HOP_SETTLE_MS 4u
#define ACTIVITY_RX_SETTLE_MS  10u

struct SubGhzTxRxActivity {
    FuriMutex* mutex;
    FuriThread* thread;
    bool running;
    bool active;
    bool hopping;
    uint8_t sources;
    const SubGhzDevice* devices[2];
    bool external[2];
    bool connected[2];
    uint8_t power_kicks;
    uint32_t probe_at;
    uint32_t settle_until;
    SubGhzActivityStream* streams[2];
};

static uint32_t subghz_activity_now(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000 / furi_kernel_get_tick_frequency());
}

static void subghz_activity_probe(SubGhzTxRxActivity* activity, uint8_t source) {
    const bool was_connected = activity->connected[source];
    activity->connected[source] = subghz_devices_is_connect(activity->devices[source]);
    if(activity->connected[source]) {
        return;
    }
    if(was_connected) {
        activity->power_kicks = 5;
    }
    if(activity->power_kicks) {
        activity->power_kicks--;
        if(!furi_hal_power_is_otg_enabled()) furi_hal_power_enable_otg();
    }
}

static bool subghz_activity_is_hopping(SubGhzTxRx* instance) {
    return !instance->dual_active && instance->hopper_state != SubGhzHopperStateOFF &&
           (instance->hopper_mode == SubGhzHopperModeFrequency ||
            instance->hopper_preset_a != instance->hopper_preset_b);
}

static int32_t subghz_txrx_activity_worker(void* context) {
    SubGhzTxRxActivity* activity = context;
    uint32_t wake = furi_get_tick();
    while(true) {
        furi_mutex_acquire(activity->mutex, FuriWaitForever);
        if(!activity->running) {
            furi_mutex_release(activity->mutex);
            break;
        }
        const uint32_t now = subghz_activity_now();
        if(activity->active && !activity->hopping &&
           (int32_t)(now - activity->settle_until) >= 0) {
            const bool probe = (uint32_t)(now - activity->probe_at) >= 100u;
            for(uint8_t source = 0; source < activity->sources; ++source) {
                const SubGhzDevice* device = activity->devices[source];
                if(probe && activity->external[source]) {
                    subghz_activity_probe(activity, source);
                }
                if(activity->connected[source]) {
                    const float rssi = subghz_devices_get_rssi(device);
                    subghz_activity_sample(activity->streams[source], subghz_activity_now(), rssi);
                    if(activity->external[source] && rssi < -130.0f) {
                        subghz_activity_probe(activity, source);
                    }
                }
            }
            if(probe) activity->probe_at = now;
        }
        furi_mutex_release(activity->mutex);
        wake += furi_ms_to_ticks(ACTIVITY_SAMPLE_MS);
        if((int32_t)(wake - furi_get_tick()) > 0) {
            furi_delay_until_tick(wake);
        } else {
            wake = furi_get_tick();
            furi_delay_ms(ACTIVITY_SAMPLE_MS);
        }
    }
    return 0;
}

void subghz_txrx_activity_pause(SubGhzTxRx* instance) {
    SubGhzTxRxActivity* activity = instance->activity;
    if(!activity) return;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    activity->active = false;
    furi_mutex_release(activity->mutex);
}

void subghz_txrx_activity_rx(SubGhzTxRx* instance, bool hop) {
    SubGhzTxRxActivity* activity = instance->activity;
    if(!activity) return;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    if(!activity->running) {
        furi_mutex_release(activity->mutex);
        return;
    }
    const uint32_t now = subghz_activity_now();
    activity->hopping = subghz_activity_is_hopping(instance);
    activity->sources = instance->dual_active ? 2 : 1;
    activity->devices[0] = instance->dual_active ? instance->radio_device_int :
                                                   instance->radio_device;
    activity->devices[1] = instance->radio_device_ext;
    activity->external[0] = !instance->dual_active &&
                            instance->radio_device_type != SubGhzRadioDeviceTypeInternal;
    activity->external[1] = true;
    for(uint8_t source = 0; source < activity->sources; ++source) {
        if(!hop) subghz_activity_reset(activity->streams[source], now, source);
        subghz_activity_tune(activity->streams[source], now, activity->hopping);
        if(!hop) activity->connected[source] = true;
    }
    if(!hop) activity->probe_at = now - 100u;
    activity->settle_until = now + (hop ? ACTIVITY_HOP_SETTLE_MS : ACTIVITY_RX_SETTLE_MS);
    activity->active = activity->running;
    furi_mutex_release(activity->mutex);
}

void subghz_txrx_activity_prepare(SubGhzTxRx* instance, bool dual) {
    if(!instance->activity) {
        SubGhzTxRxActivity* activity = calloc(1, sizeof(SubGhzTxRxActivity));
        activity->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
        instance->activity = activity;
    }
    SubGhzTxRxActivity* activity = instance->activity;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    furi_check(!activity->running);
    if(!activity->streams[0]) activity->streams[0] = calloc(1, sizeof(SubGhzActivityStream));
    if(dual && !activity->streams[1]) {
        activity->streams[1] = calloc(1, sizeof(SubGhzActivityStream));
    } else if(!dual) {
        free(activity->streams[1]);
        activity->streams[1] = NULL;
    }
    furi_mutex_release(activity->mutex);
}

void subghz_txrx_activity_start(SubGhzTxRx* instance) {
    furi_check(instance->activity);
    SubGhzTxRxActivity* activity = instance->activity;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    if(activity->running) {
        furi_mutex_release(activity->mutex);
        return;
    }
    activity->running = true;
    furi_mutex_release(activity->mutex);
    subghz_txrx_activity_rx(instance, false);
    if(!activity->hopping) {
        activity->thread =
            furi_thread_alloc_ex("SubGhzActivity", 1536, subghz_txrx_activity_worker, activity);
        furi_thread_start(activity->thread);
    }
}

void subghz_txrx_activity_stop(SubGhzTxRx* instance) {
    SubGhzTxRxActivity* activity = instance->activity;
    if(!activity) return;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    activity->active = false;
    activity->running = false;
    furi_mutex_release(activity->mutex);
    if(activity->thread) {
        furi_thread_join(activity->thread);
        furi_thread_free(activity->thread);
        activity->thread = NULL;
    }
}

void subghz_txrx_activity_free(SubGhzTxRx* instance) {
    if(!instance->activity) return;
    subghz_txrx_activity_stop(instance);
    free(instance->activity->streams[0]);
    free(instance->activity->streams[1]);
    furi_mutex_free(instance->activity->mutex);
    free(instance->activity);
    instance->activity = NULL;
}

void subghz_txrx_activity_hopper_sample(SubGhzTxRx* instance, float rssi) {
    SubGhzTxRxActivity* activity = instance->activity;
    if(!activity) return;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    const uint32_t now = subghz_activity_now();
    if(activity->active && activity->hopping && (int32_t)(now - activity->settle_until) >= 0) {
        if(activity->external[0] && (uint32_t)(now - activity->probe_at) >= 100u) {
            subghz_activity_probe(activity, 0);
            activity->probe_at = now;
        }
        if(activity->connected[0]) subghz_activity_sample(activity->streams[0], now, rssi);
    }
    furi_mutex_release(activity->mutex);
}

void subghz_txrx_activity_note_decode(SubGhzTxRx* instance, uint8_t source) {
    SubGhzTxRxActivity* activity = instance->activity;
    if(!activity || source > 1) return;
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    if(activity->active && source < activity->sources) {
        subghz_activity_decode(activity->streams[source], subghz_activity_now());
    }
    furi_mutex_release(activity->mutex);
}

void subghz_txrx_activity_read(
    SubGhzTxRx* instance,
    uint8_t source,
    SubGhzActivityHistory* history) {
    SubGhzTxRxActivity* activity = instance->activity;
    if(!activity || source > 1) {
        memset(history, 0, sizeof(*history));
        return;
    }
    furi_mutex_acquire(activity->mutex, FuriWaitForever);
    if(activity->streams[source]) {
        subghz_activity_advance(activity->streams[source], subghz_activity_now());
        *history = activity->streams[source]->history;
    } else {
        memset(history, 0, sizeof(*history));
    }
    furi_mutex_release(activity->mutex);
}
