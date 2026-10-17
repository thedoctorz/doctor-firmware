#include "analyzer_ui.h"

#include <gui/elements.h>

#include <stdio.h>

void analyzer_ui_freq_str(char* out, size_t cap, uint32_t hz) {
    snprintf(
        out,
        cap,
        "%lu.%03lu MHz",
        (unsigned long)(hz / 1000000u),
        (unsigned long)((hz % 1000000u) / 1000u));
}

void analyzer_ui_freq_short(char* out, size_t cap, uint32_t hz) {
    snprintf(
        out,
        cap,
        "%lu.%03lu",
        (unsigned long)(hz / 1000000u),
        (unsigned long)((hz % 1000000u) / 1000u));
}

void analyzer_ui_offset_str(char* out, size_t cap, int32_t hz) {
    const char sign = (hz < 0) ? '-' : '+';
    uint32_t mag = (uint32_t)((hz < 0) ? -hz : hz);

    if(mag >= 1000000u) {
        snprintf(
            out,
            cap,
            "%c%lu.%02luM",
            sign,
            (unsigned long)(mag / 1000000u),
            (unsigned long)((mag % 1000000u) / 10000u));
    } else if(mag >= 1000u) {
        snprintf(
            out,
            cap,
            "%c%lu.%01luk",
            sign,
            (unsigned long)(mag / 1000u),
            (unsigned long)((mag % 1000u) / 100u));
    } else {
        snprintf(out, cap, "%c%lu", sign, (unsigned long)mag);
    }
}

void analyzer_ui_chrome(
    Canvas* canvas,
    AnalyzerPipelineStage stage,
    uint32_t frequency,
    float rssi_dbm,
    bool has_rssi) {
    char buf[24];

    canvas_set_font(canvas, FontSecondary);

    for(uint8_t i = 0; i < AnalyzerPipelineCount; i++) {
        const int x = 2 + i * 4;
        if(i <= (uint8_t)stage) {
            canvas_draw_box(canvas, x, 3, 3, 3);
        } else {
            canvas_draw_frame(canvas, x, 3, 3, 3);
        }
    }

    const int name_x = 24;
    const char* name = analyzer_pipeline_name(stage);
    canvas_draw_str(canvas, name_x, 8, name);

    int after_name = name_x + (int)canvas_string_width(canvas, name) + 4;
    int rssi_left = 128;

    if(has_rssi) {
        snprintf(buf, sizeof(buf), "%d", (int)rssi_dbm);
        canvas_draw_str_aligned(canvas, 126, 8, AlignRight, AlignBottom, buf);
        rssi_left = 126 - (int)canvas_string_width(canvas, buf) - 4;
    }

    if(frequency) {
        analyzer_ui_freq_short(buf, sizeof(buf), frequency);
        const int fw = (int)canvas_string_width(canvas, buf);
        if(after_name + fw <= rssi_left) {
            canvas_draw_str(canvas, after_name, 8, buf);
        }
    }

    canvas_draw_line(canvas, 0, ANALYZER_UI_CHROME_H - 1, 127, ANALYZER_UI_CHROME_H - 1);
}

void analyzer_ui_bar(Canvas* canvas, int x, int y, int w, int h, uint8_t percent) {
    if(percent > 100u) percent = 100u;

    int radius = 3;
    if(radius > (w - 1) / 2) radius = (w - 1) / 2;
    if(radius > (h - 1) / 2) radius = (h - 1) / 2;
    if(radius < 0) radius = 0;

    if(radius > 0) {
        canvas_draw_rframe(canvas, x, y, w, h, radius);
    } else {
        canvas_draw_frame(canvas, x, y, w, h);
    }

    const int inner = w - 2;
    int fill = (inner * (int)percent) / 100;
    if(fill > inner) fill = inner;
    if(fill > 0) canvas_draw_box(canvas, x + 1, y + 1, fill, h - 2);
}

void analyzer_ui_chart(
    Canvas* canvas,
    int baseline_y,
    int height,
    const int8_t* raw,
    const bool* seen,
    uint16_t count,
    int8_t floor_raw,
    uint16_t peak_bin,
    bool mark_peak) {
    if(count == 0) return;

    for(uint16_t i = 0; i < count; i++) {
        if(!seen[i]) continue;

        int bars = ((int)raw[i] - (int)floor_raw) >> 1;
        if(bars < 0) bars = 0;
        if(bars > height) bars = height;

        const int x = ANALYZER_UI_CHART_X + i * ANALYZER_UI_BIN_W;
        if(bars > 0) {
            canvas_draw_box(canvas, x, baseline_y - bars, ANALYZER_UI_BIN_W - 1, bars);
        } else {
            canvas_draw_dot(canvas, x, baseline_y);
        }
    }

    if(mark_peak && peak_bin < count && seen[peak_bin]) {
        const int x = ANALYZER_UI_CHART_X + peak_bin * ANALYZER_UI_BIN_W;
        canvas_draw_line(canvas, x, baseline_y - height - 2, x, baseline_y - height - 1);
    }
}

void analyzer_ui_axis(Canvas* canvas, int y) {
    canvas_draw_line(canvas, ANALYZER_UI_CHART_X, y, ANALYZER_UI_CHART_X + ANALYZER_UI_CHART_W, y);

    const int centre = ANALYZER_UI_CHART_X + ANALYZER_UI_CHART_W / 2;
    canvas_draw_line(canvas, ANALYZER_UI_CHART_X, y, ANALYZER_UI_CHART_X, y + 2);
    canvas_draw_line(canvas, centre, y, centre, y + 2);
    canvas_draw_line(
        canvas,
        ANALYZER_UI_CHART_X + ANALYZER_UI_CHART_W,
        y,
        ANALYZER_UI_CHART_X + ANALYZER_UI_CHART_W,
        y + 2);
}

void analyzer_ui_hint_center(Canvas* canvas, const char* text) {
    canvas_set_font(canvas, FontSecondary);
    elements_button_center(canvas, text);
}
