#pragma once

#include <gui/view.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AnalyzerSetEventLook,
    AnalyzerSetEventBack,
} AnalyzerSetEvent;

typedef struct AnalyzerViewSet AnalyzerViewSet;

typedef void (*AnalyzerViewSetCallback)(void* context, AnalyzerSetEvent event);

AnalyzerViewSet* analyzer_view_set_alloc(void);
void analyzer_view_set_free(AnalyzerViewSet* instance);
View* analyzer_view_set_get_view(AnalyzerViewSet* instance);

void analyzer_view_set_set_callback(
    AnalyzerViewSet* instance,
    AnalyzerViewSetCallback callback,
    void* context);

void analyzer_view_set_set_frequency(AnalyzerViewSet* instance, uint32_t frequency);
uint32_t analyzer_view_set_frequency(AnalyzerViewSet* instance);
bool analyzer_view_set_is_valid(AnalyzerViewSet* instance);

#ifdef __cplusplus
}
#endif
