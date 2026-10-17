#include "subghz_frequency_analyzer_worker.h"
#include <subghz/helpers/subghz_txrx_i.h>
#include <lib/drivers/cc1101.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <furi_hal_subghz.h>
#include <string.h>

#include <furi.h>
#include <float_tools.h>

#define TAG "SubghzFrequencyAnalyzerWorker"

#define SUBGHZ_FREQUENCY_ANALYZER_THRESHOLD -97.0f

#define SUBGHZ_FA_PRESET_BODY(mdmcfg4)                                                            \
    CC1101_IOCFG0, CC1101IocfgHW, CC1101_FIFOTHR, 0x07, CC1101_PKTCTRL0, 0x32, CC1101_FSCTRL1,    \
        0x06, CC1101_MDMCFG0, 0x00, CC1101_MDMCFG1, 0x00, CC1101_MDMCFG2, 0x30, CC1101_MDMCFG3,   \
        0x7F, CC1101_MDMCFG4, (mdmcfg4), CC1101_MCSM0, 0x18, CC1101_FOCCFG, 0x18,                 \
        CC1101_AGCCTRL0, 0x30, CC1101_AGCCTRL1, 0x08, CC1101_AGCCTRL2, 0x07, CC1101_WORCTRL,      \
        0xFB, CC1101_FREND0, 0x11, CC1101_FREND1, 0xB6, 0, 0, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, \
        0x00, 0x00

static uint8_t subghz_fa_preset_coarse[] = {SUBGHZ_FA_PRESET_BODY(0x17)};
static uint8_t subghz_fa_preset_fine[] = {SUBGHZ_FA_PRESET_BODY(0xF7)};

struct SubGhzFrequencyAnalyzerWorker {
    FuriThread* thread;

    volatile bool worker_running;
    uint8_t sample_hold_counter;
    FrequencyRSSI frequency_rssi_buf;
    SubGhzSetting* setting;
    const SubGhzDevice* device;

    float filVal;
    float trigger_level;

    SubGhzFrequencyAnalyzerWorkerPairCallback pair_callback;
    void* context;
};

static float subghz_frequency_analyzer_worker_read_rssi(
    const SubGhzDevice* device,
    uint32_t frequency,
    uint32_t* actual) {
    subghz_devices_idle(device);
    uint32_t set = subghz_devices_set_frequency(device, frequency);
    if(actual) *actual = set;
    subghz_devices_flush_rx(device);
    subghz_devices_set_rx(device);
    furi_delay_ms(3);
    return subghz_devices_get_rssi(device);
}

static uint32_t subghz_frequency_analyzer_worker_expRunningAverageAdaptive(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t newVal) {
    float k;
    float newValFloat = newVal;
    if(fabsf(newValFloat - instance->filVal) > 500000.f)
        k = 0.9;
    else
        k = 0.03;

    instance->filVal += (newValFloat - instance->filVal) * k;
    return (uint32_t)instance->filVal;
}

static int32_t subghz_frequency_analyzer_worker_thread(void* context) {
    SubGhzFrequencyAnalyzerWorker* instance = context;

    FrequencyRSSI frequency_rssi = {
        .frequency_coarse = 0, .rssi_coarse = 0, .frequency_fine = 0, .rssi_fine = 0};
    float rssi = 0;
    uint32_t frequency = 0;
    float rssi_temp = 0;
    uint32_t frequency_temp = 0;

    const SubGhzDevice* device = instance->device;

    subghz_devices_reset(device);
    subghz_devices_idle(device);

    while(instance->worker_running) {
        furi_delay_ms(10);

        float rssi_min = 26.0f;
        float rssi_avg = 0;
        size_t rssi_avg_samples = 0;

        frequency_rssi.rssi_coarse = -127.0f;
        frequency_rssi.rssi_fine = -127.0f;
        subghz_devices_idle(device);
        subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, subghz_fa_preset_coarse);

        for(size_t i = 0; i < subghz_setting_get_frequency_count(instance->setting); i++) {
            uint32_t current_frequency = subghz_setting_get_frequency(instance->setting, i);
            if(subghz_devices_is_frequency_valid(device, current_frequency) &&
               (((current_frequency != 462750000) && (current_frequency != 467750000) &&
                 (current_frequency != 464000000)) &&
                (current_frequency <= 920000000))) {
                rssi = subghz_frequency_analyzer_worker_read_rssi(
                    device, current_frequency, &frequency);

                rssi_avg += rssi;
                rssi_avg_samples++;

                if(rssi < rssi_min) rssi_min = rssi;

                if(frequency_rssi.rssi_coarse < rssi) {
                    frequency_rssi.rssi_coarse = rssi;
                    frequency_rssi.frequency_coarse = frequency;
                }
            }
        }

        if(rssi_avg_samples > 0) {
            FURI_LOG_T(
                TAG,
                "RSSI: avg %f, max %f at %lu, min %f",
                (double)(rssi_avg / rssi_avg_samples),
                (double)frequency_rssi.rssi_coarse,
                frequency_rssi.frequency_coarse,
                (double)rssi_min);
        }

        if(rssi_avg_samples > 0 && frequency_rssi.rssi_coarse > instance->trigger_level) {
            subghz_devices_idle(device);
            subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, subghz_fa_preset_fine);
            for(uint32_t i = frequency_rssi.frequency_coarse - 300000;
                i < frequency_rssi.frequency_coarse + 300000;
                i += 20000) {
                if(subghz_devices_is_frequency_valid(device, i)) {
                    rssi = subghz_frequency_analyzer_worker_read_rssi(device, i, &frequency);

                    FURI_LOG_T(TAG, "#:%lu:%f", frequency, (double)rssi);

                    if(frequency_rssi.rssi_fine < rssi) {
                        frequency_rssi.rssi_fine = rssi;
                        frequency_rssi.frequency_fine = frequency;
                    }
                }
            }
        }

        if(frequency_rssi.rssi_fine > instance->trigger_level) {
            FURI_LOG_D(
                TAG, "=:%lu:%f", frequency_rssi.frequency_fine, (double)frequency_rssi.rssi_fine);

            instance->sample_hold_counter = 20;
            rssi_temp = frequency_rssi.rssi_fine;
            frequency_temp = frequency_rssi.frequency_fine;

            if(!float_is_equal(instance->filVal, 0.f)) {
                frequency_rssi.frequency_fine =
                    subghz_frequency_analyzer_worker_expRunningAverageAdaptive(
                        instance, frequency_rssi.frequency_fine);
            }
            if(instance->pair_callback) {
                instance->pair_callback(
                    instance->context,
                    frequency_rssi.frequency_fine,
                    frequency_rssi.rssi_fine,
                    true);
            }
        } else if(
            (frequency_rssi.rssi_coarse > instance->trigger_level) &&
            (instance->sample_hold_counter < 10)) {
            FURI_LOG_D(
                TAG,
                "~:%lu:%f",
                frequency_rssi.frequency_coarse,
                (double)frequency_rssi.rssi_coarse);

            instance->sample_hold_counter = 20;
            rssi_temp = frequency_rssi.rssi_coarse;
            frequency_temp = frequency_rssi.frequency_coarse;
            if(!float_is_equal(instance->filVal, 0.f)) {
                frequency_rssi.frequency_coarse =
                    subghz_frequency_analyzer_worker_expRunningAverageAdaptive(
                        instance, frequency_rssi.frequency_coarse);
            }
            if(instance->pair_callback) {
                instance->pair_callback(
                    instance->context,
                    frequency_rssi.frequency_coarse,
                    frequency_rssi.rssi_coarse,
                    true);
            }
        } else {
            if(instance->sample_hold_counter > 0) {
                instance->sample_hold_counter--;
                if(instance->sample_hold_counter == 18) {
                    if(instance->pair_callback) {
                        instance->pair_callback(
                            instance->context, frequency_temp, rssi_temp, false);
                    }
                }
            } else {
                instance->filVal = 0;
                if(instance->pair_callback)
                    instance->pair_callback(instance->context, 0, 0, false);
            }
        }
    }

    subghz_devices_idle(device);
    subghz_devices_reset(device);
    if(strcmp(subghz_devices_get_name(device), SUBGHZ_DEVICE_CC1101_INT_NAME) == 0) {
        furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    }
    subghz_devices_sleep(device);

    return 0;
}

SubGhzFrequencyAnalyzerWorker* subghz_frequency_analyzer_worker_alloc(void* context) {
    furi_assert(context);
    SubGhzFrequencyAnalyzerWorker* instance = malloc(sizeof(SubGhzFrequencyAnalyzerWorker));

    instance->thread = furi_thread_alloc_ex(
        "SubGhzFAWorker", 2048, subghz_frequency_analyzer_worker_thread, instance);
    SubGhz* subghz = context;
    instance->setting = subghz_txrx_get_setting(subghz->txrx);
    instance->device = subghz->txrx->radio_device;
    instance->trigger_level = subghz->last_settings->frequency_analyzer_trigger;
    return instance;
}

void subghz_frequency_analyzer_worker_free(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);

    furi_thread_free(instance->thread);
    free(instance);
}

void subghz_frequency_analyzer_worker_set_pair_callback(
    SubGhzFrequencyAnalyzerWorker* instance,
    SubGhzFrequencyAnalyzerWorkerPairCallback callback,
    void* context) {
    furi_assert(instance);
    furi_assert(context);
    instance->pair_callback = callback;
    instance->context = context;
}

void subghz_frequency_analyzer_worker_start(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->worker_running);

    instance->worker_running = true;

    furi_thread_start(instance->thread);
}

void subghz_frequency_analyzer_worker_stop(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->worker_running);

    instance->worker_running = false;

    furi_thread_join(instance->thread);
}

bool subghz_frequency_analyzer_worker_is_running(SubGhzFrequencyAnalyzerWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}

void subghz_frequency_analyzer_worker_set_trigger_level(
    SubGhzFrequencyAnalyzerWorker* instance,
    float value) {
    instance->trigger_level = value;
}

float subghz_frequency_analyzer_worker_get_trigger_level(SubGhzFrequencyAnalyzerWorker* instance) {
    return instance->trigger_level;
}

uint32_t subghz_frequency_analyzer_get_nearest_frequency(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t input) {
    uint32_t prev_freq = 0;
    uint32_t result = 0;
    uint32_t current;

    for(size_t i = 0; i < subghz_setting_get_frequency_count(instance->setting); i++) {
        current = subghz_setting_get_frequency(instance->setting, i);
        if(current == 0) {
            continue;
        }
        if(current == input) {
            result = current;
            break;
        }
        if(current > input && prev_freq < input) {
            if(current - input < input - prev_freq) {
                result = current;
            } else {
                result = prev_freq;
            }
            break;
        }
        prev_freq = current;
    }

    return result;
}
