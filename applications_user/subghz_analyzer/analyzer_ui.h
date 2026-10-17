#pragma once

#include "analyzer_session.h"

#include <gui/gui.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_UI_CHROME_H 11

#define ANALYZER_UI_CHART_X 13
#define ANALYZER_UI_CHART_W 102
#define ANALYZER_UI_BIN_W   2

void analyzer_ui_chrome(
    Canvas* canvas,
    AnalyzerPipelineStage stage,
    uint32_t frequency,
    float rssi_dbm,
    bool has_rssi);

void analyzer_ui_freq_str(char* out, size_t cap, uint32_t hz);

void analyzer_ui_freq_short(char* out, size_t cap, uint32_t hz);

void analyzer_ui_offset_str(char* out, size_t cap, int32_t hz);

void analyzer_ui_bar(Canvas* canvas, int x, int y, int w, int h, uint8_t percent);

void analyzer_ui_chart(
    Canvas* canvas,
    int baseline_y,
    int height,
    const int8_t* raw,
    const bool* seen,
    uint16_t count,
    int8_t floor_raw,
    uint16_t peak_bin,
    bool mark_peak);

void analyzer_ui_axis(Canvas* canvas, int y);

void analyzer_ui_hint_center(Canvas* canvas, const char* text);

#ifdef __cplusplus
}
#endif
