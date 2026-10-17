#include "analyzer_hunt_worker.h"
#include "analyzer_model.h"

#include <cc1101_regs.h>
#include <float_tools.h>
#include <furi.h>

#include <math.h>
#include <string.h>

#define TAG "SubGhzAnalyzerHunt"

#define ANALYZER_HUNT_FINE_SPAN_HZ 300000u
#define ANALYZER_HUNT_FINE_STEP_HZ 20000u

#define ANALYZER_HUNT_COARSE_BW_HZ 650000u
#define ANALYZER_HUNT_FINE_BW_HZ   135417u

#define ANALYZER_HUNT_HOLD             20u
#define ANALYZER_HUNT_HOLD_RELEASE     18u
#define ANALYZER_HUNT_HOLD_COARSE_GATE 10u

#define ANALYZER_HUNT_PRESET(mdmcfg4)                                                             \
    CC1101_IOCFG0, CC1101IocfgHW, CC1101_FIFOTHR, 0x07, CC1101_PKTCTRL0, 0x32, CC1101_FSCTRL1,    \
        0x06, CC1101_MDMCFG0, 0x00, CC1101_MDMCFG1, 0x00, CC1101_MDMCFG2, 0x30, CC1101_MDMCFG3,   \
        0x7F, CC1101_MDMCFG4, (mdmcfg4), CC1101_MCSM0, 0x18, CC1101_FOCCFG, 0x18,                 \
        CC1101_AGCCTRL0, 0x30, CC1101_AGCCTRL1, 0x08, CC1101_AGCCTRL2, 0x07, CC1101_WORCTRL,      \
        0xFB, CC1101_FREND0, 0x11, CC1101_FREND1, 0xB6, 0, 0, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, \
        0x00, 0x00

static uint8_t analyzer_hunt_preset_coarse[] = {ANALYZER_HUNT_PRESET(0x17)};
static uint8_t analyzer_hunt_preset_fine[] = {ANALYZER_HUNT_PRESET(0xA7)};

struct AnalyzerHuntWorker {
    FuriThread* thread;
    volatile bool running;

    const SubGhzDevice* device;
    SubGhzSetting* setting;

    volatile float trigger;
    uint8_t hold;
    float smoothed;

    AnalyzerHuntCallback callback;
    void* context;
};

static float analyzer_hunt_read_rssi(
    const SubGhzDevice* device,
    uint32_t frequency,
    uint32_t settle_ms,
    uint32_t* got) {
    subghz_devices_idle(device);
    const uint32_t set = subghz_devices_set_frequency(device, frequency);
    if(got) *got = set;
    subghz_devices_flush_rx(device);
    subghz_devices_set_rx(device);
    furi_delay_ms(settle_ms);
    return subghz_devices_get_rssi(device);
}

static uint32_t analyzer_hunt_smooth(AnalyzerHuntWorker* instance, uint32_t value) {
    const float next = (float)value;
    const float k = (fabsf(next - instance->smoothed) > (float)ANALYZER_HUNT_FINE_STEP_HZ) ? 1.0f :
                                                                                             0.25f;
    instance->smoothed += (next - instance->smoothed) * k;
    return (uint32_t)instance->smoothed;
}

static bool analyzer_hunt_frequency_usable(const SubGhzDevice* device, uint32_t frequency) {
    if(!subghz_devices_is_frequency_valid(device, frequency)) return false;
    if(frequency == 462750000u || frequency == 467750000u || frequency == 464000000u) return false;
    return frequency <= 920000000u;
}

static int32_t analyzer_hunt_worker_thread(void* context) {
    AnalyzerHuntWorker* instance = context;
    const SubGhzDevice* device = instance->device;

    subghz_devices_reset(device);
    subghz_devices_idle(device);

    uint32_t held_frequency = 0;
    float held_rssi = 0.0f;

    while(instance->running) {
        furi_delay_ms(10);

        float coarse_rssi = -127.0f;
        uint32_t coarse_frequency = 0;
        size_t sampled = 0;

        subghz_devices_idle(device);
        subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, analyzer_hunt_preset_coarse);

        const size_t count = subghz_setting_get_frequency_count(instance->setting);
        for(size_t i = 0; i < count && instance->running; i++) {
            const uint32_t candidate = subghz_setting_get_frequency(instance->setting, i);
            if(!analyzer_hunt_frequency_usable(device, candidate)) continue;

            uint32_t actual = 0;
            const float rssi = analyzer_hunt_read_rssi(
                device, candidate, analyzer_model_settle_ms(ANALYZER_HUNT_COARSE_BW_HZ), &actual);
            sampled++;
            if(rssi > coarse_rssi) {
                coarse_rssi = rssi;
                coarse_frequency = actual;
            }
        }
        if(!instance->running) break;

        float fine_rssi = -127.0f;
        uint32_t fine_frequency = 0;
        const float trigger = instance->trigger;

        if(sampled > 0 && coarse_rssi > trigger) {
            subghz_devices_idle(device);
            subghz_devices_load_preset(
                device, FuriHalSubGhzPresetCustom, analyzer_hunt_preset_fine);

            const uint32_t from = coarse_frequency - ANALYZER_HUNT_FINE_SPAN_HZ;
            const uint32_t to = coarse_frequency + ANALYZER_HUNT_FINE_SPAN_HZ;
            for(uint32_t f = from; f < to && instance->running; f += ANALYZER_HUNT_FINE_STEP_HZ) {
                if(!subghz_devices_is_frequency_valid(device, f)) continue;

                uint32_t actual = 0;
                const float rssi = analyzer_hunt_read_rssi(
                    device, f, analyzer_model_settle_ms(ANALYZER_HUNT_FINE_BW_HZ), &actual);
                if(rssi > fine_rssi) {
                    fine_rssi = rssi;
                    fine_frequency = actual;
                }
            }
        }
        if(!instance->running) break;

        if(fine_rssi > trigger) {
            instance->hold = ANALYZER_HUNT_HOLD;
            held_frequency = fine_frequency;
            held_rssi = fine_rssi;

            uint32_t report = fine_frequency;
            if(!float_is_equal(instance->smoothed, 0.0f)) {
                report = analyzer_hunt_smooth(instance, fine_frequency);
            } else {
                instance->smoothed = (float)fine_frequency;
            }
            if(instance->callback) instance->callback(instance->context, report, fine_rssi, true);

        } else if(coarse_rssi > trigger && instance->hold < ANALYZER_HUNT_HOLD_COARSE_GATE) {
            instance->hold = ANALYZER_HUNT_HOLD;
            held_frequency = coarse_frequency;
            held_rssi = coarse_rssi;

            uint32_t report = coarse_frequency;
            if(!float_is_equal(instance->smoothed, 0.0f)) {
                report = analyzer_hunt_smooth(instance, coarse_frequency);
            } else {
                instance->smoothed = (float)coarse_frequency;
            }
            if(instance->callback)
                instance->callback(instance->context, report, coarse_rssi, true);

        } else if(instance->hold > 0) {
            instance->hold--;
            if(instance->hold == ANALYZER_HUNT_HOLD_RELEASE && instance->callback) {
                instance->callback(instance->context, held_frequency, held_rssi, false);
            }
        } else {
            instance->smoothed = 0.0f;
            if(instance->callback) instance->callback(instance->context, 0, 0.0f, false);
        }
    }

    subghz_devices_idle(device);
    subghz_devices_sleep(device);
    return 0;
}

AnalyzerHuntWorker*
    analyzer_hunt_worker_alloc(const SubGhzDevice* device, SubGhzSetting* setting, float trigger) {
    AnalyzerHuntWorker* instance = malloc(sizeof(AnalyzerHuntWorker));
    memset(instance, 0, sizeof(AnalyzerHuntWorker));

    instance->device = device;
    instance->setting = setting;
    instance->trigger = trigger;
    instance->thread =
        furi_thread_alloc_ex("SubGhzAnalyzerHunt", 2048, analyzer_hunt_worker_thread, instance);

    return instance;
}

void analyzer_hunt_worker_free(AnalyzerHuntWorker* instance) {
    furi_check(instance);
    if(instance->running) analyzer_hunt_worker_stop(instance);
    furi_thread_free(instance->thread);
    free(instance);
}

void analyzer_hunt_worker_set_callback(
    AnalyzerHuntWorker* instance,
    AnalyzerHuntCallback callback,
    void* context) {
    furi_check(instance);
    instance->callback = callback;
    instance->context = context;
}

void analyzer_hunt_worker_set_trigger(AnalyzerHuntWorker* instance, float trigger) {
    furi_check(instance);
    if(trigger < ANALYZER_HUNT_RSSI_MIN) trigger = ANALYZER_HUNT_RSSI_MIN;
    if(trigger > ANALYZER_HUNT_RSSI_MAX) trigger = ANALYZER_HUNT_RSSI_MAX;
    instance->trigger = trigger;
}

float analyzer_hunt_worker_get_trigger(const AnalyzerHuntWorker* instance) {
    furi_check(instance);
    return instance->trigger;
}

void analyzer_hunt_worker_start(AnalyzerHuntWorker* instance) {
    furi_check(instance);
    if(instance->running) return;
    instance->running = true;
    instance->hold = 0;
    instance->smoothed = 0.0f;
    furi_thread_start(instance->thread);
}

void analyzer_hunt_worker_stop(AnalyzerHuntWorker* instance) {
    furi_check(instance);
    if(!instance->running) return;
    instance->running = false;
    furi_thread_join(instance->thread);
}

bool analyzer_hunt_worker_is_running(const AnalyzerHuntWorker* instance) {
    furi_check(instance);
    return instance->running;
}

uint32_t analyzer_hunt_worker_nearest(const AnalyzerHuntWorker* instance, uint32_t frequency) {
    furi_check(instance);

    uint32_t best = frequency;
    uint32_t best_err = UINT32_MAX;

    const size_t count = subghz_setting_get_frequency_count(instance->setting);
    for(size_t i = 0; i < count; i++) {
        const uint32_t candidate = subghz_setting_get_frequency(instance->setting, i);
        const uint32_t err = (candidate > frequency) ? (candidate - frequency) :
                                                       (frequency - candidate);
        if(err < best_err) {
            best_err = err;
            best = candidate;
        }
    }
    return best;
}
