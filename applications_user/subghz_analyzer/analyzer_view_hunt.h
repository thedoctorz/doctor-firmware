#pragma once

#include <gui/view.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AnalyzerHuntEventLock,
    AnalyzerHuntEventExit,
    AnalyzerHuntEventTriggerChanged,
} AnalyzerHuntEvent;

typedef struct AnalyzerViewHunt AnalyzerViewHunt;

typedef void (*AnalyzerViewHuntCallback)(void* context, AnalyzerHuntEvent event);

AnalyzerViewHunt* analyzer_view_hunt_alloc(void);
void analyzer_view_hunt_free(AnalyzerViewHunt* instance);
View* analyzer_view_hunt_get_view(AnalyzerViewHunt* instance);

void analyzer_view_hunt_set_callback(
    AnalyzerViewHunt* instance,
    AnalyzerViewHuntCallback callback,
    void* context);

void analyzer_view_hunt_update(
    AnalyzerViewHunt* instance,
    uint32_t frequency,
    float rssi,
    bool locked);

uint32_t analyzer_view_hunt_selected(AnalyzerViewHunt* instance);

float analyzer_view_hunt_trigger(AnalyzerViewHunt* instance);
void analyzer_view_hunt_set_trigger(AnalyzerViewHunt* instance, float trigger);

bool analyzer_view_hunt_is_manual(AnalyzerViewHunt* instance);

#ifdef __cplusplus
}
#endif
