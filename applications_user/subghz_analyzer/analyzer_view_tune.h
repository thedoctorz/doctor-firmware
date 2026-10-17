#pragma once

#include "analyzer_tune_worker.h"

#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AnalyzerTuneEventStart,
    AnalyzerTuneEventAbort,
} AnalyzerTuneEvent;

typedef enum {
    AnalyzerTunePhaseArmed,
    AnalyzerTunePhaseStarting,
    AnalyzerTunePhaseRunning,
} AnalyzerTunePhase;

typedef struct AnalyzerViewTune AnalyzerViewTune;

typedef void (*AnalyzerViewTuneCallback)(void* context, AnalyzerTuneEvent event);

AnalyzerViewTune* analyzer_view_tune_alloc(void);
void analyzer_view_tune_free(AnalyzerViewTune* instance);
View* analyzer_view_tune_get_view(AnalyzerViewTune* instance);

void analyzer_view_tune_set_callback(
    AnalyzerViewTune* instance,
    AnalyzerViewTuneCallback callback,
    void* context);

void analyzer_view_tune_set_phase(AnalyzerViewTune* instance, AnalyzerTunePhase phase);

void analyzer_view_tune_set_frequency(AnalyzerViewTune* instance, uint32_t frequency);

void analyzer_view_tune_set_progress(
    AnalyzerViewTune* instance,
    const AnalyzerProgress* progress,
    uint32_t frequency,
    uint8_t best_score);

#ifdef __cplusplus
}
#endif
