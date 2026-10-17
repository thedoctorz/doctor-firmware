#pragma once

#include "analyzer_survey.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_BAND_BINS ANALYZER_SURVEY_BINS

typedef enum {
    AnalyzerZoomFine,
    AnalyzerZoom400k,
    AnalyzerZoom2M,
    AnalyzerZoom8M,
    AnalyzerZoom32M,
    AnalyzerZoomCount,
} AnalyzerZoom;

bool analyzer_band_is_fine(AnalyzerZoom zoom);

uint32_t analyzer_band_span_hz(AnalyzerZoom zoom);

uint32_t analyzer_band_bin_step_hz(AnalyzerZoom zoom);

uint32_t analyzer_band_pan_step_hz(AnalyzerZoom zoom);

const char* analyzer_band_name(AnalyzerZoom zoom);

uint32_t analyzer_band_bin_frequency(AnalyzerZoom zoom, uint32_t centre_hz, uint16_t bin);

uint32_t analyzer_band_filter_hz(AnalyzerZoom zoom);

typedef struct {
    uint32_t centre_hz;
    AnalyzerZoom zoom;
    int8_t rssi_raw[ANALYZER_BAND_BINS];
    bool seen[ANALYZER_BAND_BINS];
    int8_t floor_raw;
    uint16_t peak_bin;
    bool valid;
} AnalyzerBand;

void analyzer_band_reset(AnalyzerBand* band, uint32_t centre_hz, AnalyzerZoom zoom);
void analyzer_band_accumulate(AnalyzerBand* band, uint16_t bin, int8_t rssi_raw);

void analyzer_band_finish(AnalyzerBand* band);

#ifdef __cplusplus
}
#endif
