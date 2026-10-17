#pragma once

#include "analyzer_lock_worker.h"

#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AnalyzerLockEventTune,
    AnalyzerLockEventBack,
    AnalyzerLockEventZoomChanged,
    AnalyzerLockEventRetune,
} AnalyzerLockEvent;

typedef struct AnalyzerViewLock AnalyzerViewLock;

typedef void (*AnalyzerViewLockCallback)(void* context, AnalyzerLockEvent event);

AnalyzerViewLock* analyzer_view_lock_alloc(void);
void analyzer_view_lock_free(AnalyzerViewLock* instance);
View* analyzer_view_lock_get_view(AnalyzerViewLock* instance);

void analyzer_view_lock_set_callback(
    AnalyzerViewLock* instance,
    AnalyzerViewLockCallback callback,
    void* context);

void analyzer_view_lock_set_state(AnalyzerViewLock* instance, const AnalyzerLockState* state);

uint32_t analyzer_view_lock_frequency(AnalyzerViewLock* instance);
void analyzer_view_lock_set_frequency(AnalyzerViewLock* instance, uint32_t frequency);

AnalyzerZoom analyzer_view_lock_zoom(AnalyzerViewLock* instance);

#ifdef __cplusplus
}
#endif
