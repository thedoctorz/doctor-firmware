#pragma once

#include <furi_hal.h>
#include <subghz/subghz_i.h>

typedef struct SubGhzFrequencyAnalyzerWorker SubGhzFrequencyAnalyzerWorker;

typedef void (*SubGhzFrequencyAnalyzerWorkerPairCallback)(
    void* context,
    uint32_t frequency,
    float rssi,
    bool signal);

typedef struct {
    uint32_t frequency_coarse;
    float rssi_coarse;
    uint32_t frequency_fine;
    float rssi_fine;
} FrequencyRSSI;

SubGhzFrequencyAnalyzerWorker* subghz_frequency_analyzer_worker_alloc(void* context);

void subghz_frequency_analyzer_worker_free(SubGhzFrequencyAnalyzerWorker* instance);

void subghz_frequency_analyzer_worker_set_pair_callback(
    SubGhzFrequencyAnalyzerWorker* instance,
    SubGhzFrequencyAnalyzerWorkerPairCallback callback,
    void* context);

void subghz_frequency_analyzer_worker_start(SubGhzFrequencyAnalyzerWorker* instance);

void subghz_frequency_analyzer_worker_stop(SubGhzFrequencyAnalyzerWorker* instance);

bool subghz_frequency_analyzer_worker_is_running(SubGhzFrequencyAnalyzerWorker* instance);

void subghz_frequency_analyzer_worker_set_trigger_level(
    SubGhzFrequencyAnalyzerWorker* instance,
    float value);

float subghz_frequency_analyzer_worker_get_trigger_level(SubGhzFrequencyAnalyzerWorker* instance);

uint32_t subghz_frequency_analyzer_get_nearest_frequency(
    SubGhzFrequencyAnalyzerWorker* instance,
    uint32_t input);
