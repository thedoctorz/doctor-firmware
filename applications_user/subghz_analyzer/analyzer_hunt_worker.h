#pragma once

#include <lib/subghz/devices/devices.h>
#include <lib/subghz/subghz_setting.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_HUNT_RSSI_MIN   (-97.0f)
#define ANALYZER_HUNT_RSSI_MAX   (-60.0f)
#define ANALYZER_HUNT_RSSI_SCALE 2.3f

#define ANALYZER_HUNT_TRIGGER_DEFAULT (-78.0f)
#define ANALYZER_HUNT_TRIGGER_STEP    1.0f

typedef struct AnalyzerHuntWorker AnalyzerHuntWorker;

typedef void (*AnalyzerHuntCallback)(void* context, uint32_t frequency, float rssi, bool locked);

AnalyzerHuntWorker*
    analyzer_hunt_worker_alloc(const SubGhzDevice* device, SubGhzSetting* setting, float trigger);

void analyzer_hunt_worker_free(AnalyzerHuntWorker* instance);

void analyzer_hunt_worker_set_callback(
    AnalyzerHuntWorker* instance,
    AnalyzerHuntCallback callback,
    void* context);

void analyzer_hunt_worker_set_trigger(AnalyzerHuntWorker* instance, float trigger);
float analyzer_hunt_worker_get_trigger(const AnalyzerHuntWorker* instance);

void analyzer_hunt_worker_start(AnalyzerHuntWorker* instance);
void analyzer_hunt_worker_stop(AnalyzerHuntWorker* instance);
bool analyzer_hunt_worker_is_running(const AnalyzerHuntWorker* instance);

uint32_t analyzer_hunt_worker_nearest(const AnalyzerHuntWorker* instance, uint32_t frequency);

#ifdef __cplusplus
}
#endif
