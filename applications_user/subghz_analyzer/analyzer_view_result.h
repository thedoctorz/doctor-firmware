#pragma once

#include "analyzer_preset.h"
#include "analyzer_tune_worker.h"

#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AnalyzerResultEventActions,
    AnalyzerResultEventBack,
} AnalyzerResultEvent;

typedef struct AnalyzerViewResult AnalyzerViewResult;

typedef void (*AnalyzerViewResultCallback)(void* context, AnalyzerResultEvent event);

AnalyzerViewResult* analyzer_view_result_alloc(void);
void analyzer_view_result_free(AnalyzerViewResult* instance);
View* analyzer_view_result_get_view(AnalyzerViewResult* instance);

void analyzer_view_result_set_callback(
    AnalyzerViewResult* instance,
    AnalyzerViewResultCallback callback,
    void* context);

void analyzer_view_result_set(
    AnalyzerViewResult* instance,
    const AnalyzerResult* result,
    const AnalyzerPresetMatch* matches,
    uint8_t match_count,
    const char* save_name);

void analyzer_view_result_notify(AnalyzerViewResult* instance, const char* text);

void analyzer_view_result_tick(AnalyzerViewResult* instance);

bool analyzer_view_result_reuse_existing(AnalyzerViewResult* instance);

#ifdef __cplusplus
}
#endif
