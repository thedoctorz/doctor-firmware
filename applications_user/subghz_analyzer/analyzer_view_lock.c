#include "analyzer_view_lock.h"
#include "analyzer_model.h"
#include "analyzer_ui.h"

#include <furi.h>
#include <gui/elements.h>

#include <stdio.h>
#include <string.h>

#define ANALYZER_LOCK_CHART_BASE 46
#define ANALYZER_LOCK_CHART_H    20
#define ANALYZER_LOCK_SPAN_Y     61

typedef struct {
    AnalyzerLockState state;
    uint32_t frequency;
    uint8_t zoom;
    bool detail;
} AnalyzerViewLockModel;

struct AnalyzerViewLock {
    View* view;
    AnalyzerViewLockCallback callback;
    void* context;
};

static void analyzer_view_lock_emit(AnalyzerViewLock* instance, AnalyzerLockEvent event) {
    if(instance->callback) instance->callback(instance->context, event);
}

static const char* analyzer_view_lock_vote_text(AnalyzerEnvVote vote) {
    switch(vote) {
    case AnalyzerEnvVoteAm:
        return "ASK/OOK";
    case AnalyzerEnvVoteFm:
        return "2-FSK";
    default:
        return "listening";
    }
}

static void analyzer_view_lock_draw_detail(Canvas* canvas, const AnalyzerViewLockModel* m) {
    const AnalyzerSurvey* s = &m->state.survey;
    char buf[36];

    canvas_set_font(canvas, FontSecondary);

    if(!analyzer_band_is_fine((AnalyzerZoom)m->zoom)) {
        canvas_draw_str(canvas, 2, 24, "Shape is only measured");
        canvas_draw_str(canvas, 2, 34, "at the FINE span.");
        canvas_draw_str(canvas, 2, 48, "Wider spans retune per");
        canvas_draw_str(canvas, 2, 58, "bin: peaks, not lobes.");
        return;
    }

    if(!s->valid) {
        canvas_draw_str(canvas, 2, 30, "No signal measured yet.");
        canvas_draw_str(canvas, 2, 44, "Press the fob.");
        return;
    }

    char off[16];
    analyzer_ui_offset_str(off, sizeof(off), s->peak_offset_hz);
    snprintf(buf, sizeof(buf), "carrier err %s Hz", off);
    canvas_draw_str(canvas, 2, 21, buf);

    snprintf(buf, sizeof(buf), "occupied  %lu kHz", (unsigned long)(s->occupied_bw_hz / 1000u));
    canvas_draw_str(canvas, 2, 31, buf);

    snprintf(
        buf,
        sizeof(buf),
        "-6dB %lu  -20dB %lu kHz",
        (unsigned long)(s->width_6db_hz / 1000u),
        (unsigned long)(s->width_20db_hz / 1000u));
    canvas_draw_str(canvas, 2, 41, buf);

    if(s->twin_lobe) {
        snprintf(
            buf,
            sizeof(buf),
            "two tones, dev +-%lu kHz",
            (unsigned long)(s->lobe_split_hz / 2000u));
    } else {
        snprintf(buf, sizeof(buf), "one lobe (dev not resolved)");
    }
    canvas_draw_str(canvas, 2, 51, buf);

    snprintf(
        buf,
        sizeof(buf),
        "%u passes  floor %d dBm",
        m->state.survey_passes,
        (int)m->state.floor_dbm);
    canvas_draw_str(canvas, 2, 61, buf);
}

static void analyzer_view_lock_draw(Canvas* canvas, void* context) {
    const AnalyzerViewLockModel* m = context;
    char buf[32];

    canvas_clear(canvas);
    analyzer_ui_chrome(
        canvas, AnalyzerPipelineLock, m->frequency, m->state.peak_dbm, m->state.survey_passes > 0);

    if(m->detail) {
        analyzer_view_lock_draw_detail(canvas, m);
        return;
    }

    canvas_set_font(canvas, FontSecondary);

    if(m->state.vote == AnalyzerEnvVoteNone) {
        canvas_draw_str(canvas, 2, 20, "Press the fob");
        if(m->state.overloaded) {
            snprintf(buf, sizeof(buf), "OVL %s", analyzer_band_name((AnalyzerZoom)m->zoom));
        } else if(m->state.atten) {
            snprintf(
                buf,
                sizeof(buf),
                "att%u %s",
                m->state.atten,
                analyzer_band_name((AnalyzerZoom)m->zoom));
        } else {
            snprintf(buf, sizeof(buf), "%s", analyzer_band_name((AnalyzerZoom)m->zoom));
        }
        canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, buf);
    } else {
        const char* vote = analyzer_view_lock_vote_text(m->state.vote);
        canvas_draw_str(canvas, 2, 20, vote);
        const int vote_w = canvas_string_width(canvas, vote);
        const int bar_x = 2 + vote_w + 6;
        analyzer_ui_bar(canvas, bar_x, 13, 28, 8, m->state.confidence);
        snprintf(buf, sizeof(buf), "%u%%", m->state.confidence);
        canvas_draw_str(canvas, bar_x + 32, 20, buf);
        if(m->state.overloaded) {
            canvas_draw_str_aligned(canvas, 126, 20, AlignRight, AlignBottom, "OVL");
        }
    }

    int8_t raw[ANALYZER_BAND_BINS];
    bool seen[ANALYZER_BAND_BINS];
    int8_t floor_raw;
    uint16_t peak_bin = 0;
    bool have = false;

    if(analyzer_band_is_fine((AnalyzerZoom)m->zoom)) {
        const AnalyzerSurvey* s = &m->state.survey;
        floor_raw = (int8_t)((s->floor_dbm + 74.0f) * 2.0f);
        for(uint16_t i = 0; i < ANALYZER_SURVEY_BINS; i++) {
            raw[i] = s->bins[i].rssi_raw;
            seen[i] = s->bins[i].seen;
            if(seen[i] && (!have || raw[i] > raw[peak_bin])) {
                peak_bin = i;
                have = true;
            }
        }
    } else {
        const AnalyzerBand* b = &m->state.band;
        floor_raw = b->floor_raw;
        memcpy(raw, b->rssi_raw, sizeof(raw));
        memcpy(seen, b->seen, sizeof(seen));
        peak_bin = b->peak_bin;
        have = b->valid;
    }

    analyzer_ui_chart(
        canvas,
        ANALYZER_LOCK_CHART_BASE,
        ANALYZER_LOCK_CHART_H,
        raw,
        seen,
        ANALYZER_BAND_BINS,
        floor_raw,
        peak_bin,
        have);
    analyzer_ui_axis(canvas, ANALYZER_LOCK_CHART_BASE + 1);

    const uint32_t half = analyzer_band_span_hz((AnalyzerZoom)m->zoom) / 2u;
    canvas_set_font(canvas, FontSecondary);
    analyzer_ui_offset_str(buf, sizeof(buf), -(int32_t)half);
    canvas_draw_str(canvas, 0, ANALYZER_LOCK_SPAN_Y, buf);
    analyzer_ui_offset_str(buf, sizeof(buf), (int32_t)half);
    canvas_draw_str_aligned(canvas, 127, ANALYZER_LOCK_SPAN_Y, AlignRight, AlignBottom, buf);

    analyzer_ui_hint_center(canvas, "Tune");
}

static void analyzer_view_lock_pan(AnalyzerViewLockModel* m, bool up) {
    const uint32_t step = analyzer_band_pan_step_hz((AnalyzerZoom)m->zoom);
    int64_t next = (int64_t)m->frequency + (up ? (int64_t)step : -(int64_t)step);
    if(next < 0) next = 0;

    if(analyzer_model_frequency_valid((uint32_t)next)) m->frequency = (uint32_t)next;
}

static bool analyzer_view_lock_input(InputEvent* event, void* context) {
    AnalyzerViewLock* instance = context;
    bool handled = true;
    bool emit_tune = false, emit_back = false, emit_zoom = false, emit_retune = false;

    const bool step = (event->type == InputTypePress) || (event->type == InputTypeRepeat);

    with_view_model(
        instance->view,
        AnalyzerViewLockModel * m,
        {
            if(event->key == InputKeyOk && event->type == InputTypeLong) {
                m->detail = !m->detail;

            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                emit_tune = true;

            } else if(event->key == InputKeyBack && event->type == InputTypeShort) {
                if(m->detail) {
                    m->detail = false;
                } else {
                    emit_back = true;
                }

            } else if(step && event->key == InputKeyUp) {
                if(m->zoom > 0) {
                    m->zoom--;
                    emit_zoom = true;
                }
            } else if(step && event->key == InputKeyDown) {
                if(m->zoom + 1u < AnalyzerZoomCount) {
                    m->zoom++;
                    emit_zoom = true;
                }
            } else if(step && event->key == InputKeyLeft) {
                analyzer_view_lock_pan(m, false);
                emit_retune = true;
            } else if(step && event->key == InputKeyRight) {
                analyzer_view_lock_pan(m, true);
                emit_retune = true;
            } else {
                handled = false;
            }
        },
        true);

    if(emit_tune) analyzer_view_lock_emit(instance, AnalyzerLockEventTune);
    if(emit_back) analyzer_view_lock_emit(instance, AnalyzerLockEventBack);
    if(emit_zoom) analyzer_view_lock_emit(instance, AnalyzerLockEventZoomChanged);
    if(emit_retune) analyzer_view_lock_emit(instance, AnalyzerLockEventRetune);

    return handled;
}

AnalyzerViewLock* analyzer_view_lock_alloc(void) {
    AnalyzerViewLock* instance = malloc(sizeof(AnalyzerViewLock));
    memset(instance, 0, sizeof(*instance));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(AnalyzerViewLockModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, analyzer_view_lock_draw);
    view_set_input_callback(instance->view, analyzer_view_lock_input);

    return instance;
}

void analyzer_view_lock_free(AnalyzerViewLock* instance) {
    furi_check(instance);
    view_free(instance->view);
    free(instance);
}

View* analyzer_view_lock_get_view(AnalyzerViewLock* instance) {
    furi_check(instance);
    return instance->view;
}

void analyzer_view_lock_set_callback(
    AnalyzerViewLock* instance,
    AnalyzerViewLockCallback callback,
    void* context) {
    furi_check(instance);
    instance->callback = callback;
    instance->context = context;
}

void analyzer_view_lock_set_state(AnalyzerViewLock* instance, const AnalyzerLockState* state) {
    furi_check(instance);
    with_view_model(instance->view, AnalyzerViewLockModel * m, { m->state = *state; }, true);
}

uint32_t analyzer_view_lock_frequency(AnalyzerViewLock* instance) {
    furi_check(instance);
    uint32_t frequency = 0;
    with_view_model(
        instance->view, AnalyzerViewLockModel * m, { frequency = m->frequency; }, false);
    return frequency;
}

void analyzer_view_lock_set_frequency(AnalyzerViewLock* instance, uint32_t frequency) {
    furi_check(instance);
    with_view_model(
        instance->view,
        AnalyzerViewLockModel * m,
        {
            m->frequency = frequency;
            memset(&m->state, 0, sizeof(m->state));
        },
        true);
}

AnalyzerZoom analyzer_view_lock_zoom(AnalyzerViewLock* instance) {
    furi_check(instance);
    uint8_t zoom = AnalyzerZoomFine;
    with_view_model(instance->view, AnalyzerViewLockModel * m, { zoom = m->zoom; }, false);
    return (AnalyzerZoom)zoom;
}
