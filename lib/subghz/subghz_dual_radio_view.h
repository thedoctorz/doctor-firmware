#pragma once

#include <gui/canvas.h>
#include <stdio.h>
#include <string.h>

#define SUBGHZ_DUAL_RADIO_HISTORY_SIZE 128
#define SUBGHZ_DUAL_RADIO_WAVE_HEIGHT  33

static inline void subghz_dual_radio_draw_wave(
    Canvas* canvas,
    const uint8_t* history,
    uint8_t history_index,
    uint8_t baseline,
    uint8_t region_height) {
    for(uint8_t col = 0; col < SUBGHZ_DUAL_RADIO_HISTORY_SIZE; col++) {
        uint8_t idx = (history_index + col) % SUBGHZ_DUAL_RADIO_HISTORY_SIZE;
        uint16_t bar = (uint16_t)history[idx] * region_height / SUBGHZ_DUAL_RADIO_WAVE_HEIGHT;
        if(bar < 1) bar = 1;
        if(bar > region_height) bar = region_height;
        canvas_draw_line(canvas, col, baseline, col, (uint8_t)(baseline - bar + 1));
    }
}

static inline void subghz_dual_radio_draw_pane(
    Canvas* canvas,
    const char* marker,
    const char* info,
    const uint8_t* history,
    uint8_t history_index,
    uint8_t top,
    uint8_t label_width) {
    const uint8_t height = 31;
    const uint8_t bottom = top + height - 1;
    char label[48];
    snprintf(label, sizeof(label), "[%s] %s", marker, info);
    size_t label_length = strlen(label);
    while(label_length && canvas_string_width(canvas, label) > label_width) {
        label[--label_length] = '\0';
    }

    canvas_set_color(canvas, ColorBlack);
    if(history) {
        subghz_dual_radio_draw_wave(canvas, history, history_index, bottom - 1, height - 14);
    }
    canvas_draw_frame(canvas, 0, top, 128, height);
    canvas_draw_str(canvas, 4, top + 9, label);
}
