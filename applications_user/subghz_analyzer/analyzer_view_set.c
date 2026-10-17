#include "analyzer_view_set.h"
#include "analyzer_model.h"
#include "analyzer_ui.h"

#include <furi.h>
#include <gui/elements.h>

#include <stdio.h>
#include <string.h>

#define ANALYZER_SET_STEP_HZ 1000u

#define ANALYZER_SET_FREQ_TOP  26
#define ANALYZER_SET_ARROW_W   5
#define ANALYZER_SET_ARROW_HH  3
#define ANALYZER_SET_ARROW_GAP 3

typedef struct {
    uint32_t frequency;
    bool valid;
} AnalyzerViewSetModel;

struct AnalyzerViewSet {
    View* view;
    AnalyzerViewSetCallback callback;
    void* context;
};

static void analyzer_view_set_emit(AnalyzerViewSet* instance, AnalyzerSetEvent event) {
    if(instance->callback) instance->callback(instance->context, event);
}

static void analyzer_view_set_arrow(Canvas* canvas, int apex_x, int cy, bool point_left) {
    for(int i = 0; i <= ANALYZER_SET_ARROW_W; i++) {
        const int half =
            (ANALYZER_SET_ARROW_HH * i + ANALYZER_SET_ARROW_W / 2) / ANALYZER_SET_ARROW_W;
        const int col = point_left ? (apex_x + i) : (apex_x - i);
        canvas_draw_line(canvas, col, cy - half, col, cy + half);
    }
}

static void analyzer_view_set_nudge(AnalyzerViewSetModel* m, bool up) {
    int64_t next = (int64_t)m->frequency +
                   (up ? (int64_t)ANALYZER_SET_STEP_HZ : -(int64_t)ANALYZER_SET_STEP_HZ);
    if(next < 0) next = 0;
    if(next > 1000000000LL) next = 1000000000LL;
    m->frequency = (uint32_t)next;
    m->valid = analyzer_model_frequency_valid(m->frequency);
}

static void analyzer_view_set_draw(Canvas* canvas, void* context) {
    const AnalyzerViewSetModel* m = context;
    char buf[20];

    canvas_clear(canvas);
    analyzer_ui_chrome(canvas, AnalyzerPipelineSet, m->frequency, 0.0f, false);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 21, "Lock Frequency");
    canvas_draw_str_aligned(canvas, 126, 21, AlignRight, AlignBottom, "MHz");

    if(m->frequency) {
        analyzer_ui_freq_short(buf, sizeof(buf), m->frequency);
    } else {
        snprintf(buf, sizeof(buf), "---.---");
    }

    canvas_set_font(canvas, FontBigNumbers);
    const int text_w = (int)canvas_string_width(canvas, buf);
    const int text_x = 64 - text_w / 2;
    canvas_draw_str_aligned(canvas, 64, ANALYZER_SET_FREQ_TOP, AlignCenter, AlignTop, buf);

    const int digit_h = (int)canvas_get_font_params(canvas, FontBigNumbers)->height;
    const int cy = ANALYZER_SET_FREQ_TOP + digit_h / 2;
    analyzer_view_set_arrow(
        canvas, text_x - ANALYZER_SET_ARROW_GAP - ANALYZER_SET_ARROW_W, cy, true);
    analyzer_view_set_arrow(
        canvas, text_x + text_w + ANALYZER_SET_ARROW_GAP + ANALYZER_SET_ARROW_W, cy, false);

    if(m->valid) {
        analyzer_ui_hint_center(canvas, "Next");
    } else {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 61, AlignCenter, AlignBottom, "outside band");
    }
}

static bool analyzer_view_set_input(InputEvent* event, void* context) {
    AnalyzerViewSet* instance = context;
    bool handled = true;
    bool emit_look = false;
    bool emit_back = false;

    const bool step = (event->type == InputTypePress) || (event->type == InputTypeRepeat);

    with_view_model(
        instance->view,
        AnalyzerViewSetModel * m,
        {
            if(event->key == InputKeyOk && event->type == InputTypeShort) {
                if(m->valid && m->frequency) emit_look = true;
            } else if(event->key == InputKeyBack && event->type == InputTypeShort) {
                emit_back = true;
            } else if(step && event->key == InputKeyLeft) {
                analyzer_view_set_nudge(m, false);
            } else if(step && event->key == InputKeyRight) {
                analyzer_view_set_nudge(m, true);
            } else {
                handled = false;
            }
        },
        true);

    if(emit_look) analyzer_view_set_emit(instance, AnalyzerSetEventLook);
    if(emit_back) analyzer_view_set_emit(instance, AnalyzerSetEventBack);
    return handled;
}

AnalyzerViewSet* analyzer_view_set_alloc(void) {
    AnalyzerViewSet* instance = malloc(sizeof(AnalyzerViewSet));
    memset(instance, 0, sizeof(*instance));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(AnalyzerViewSetModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, analyzer_view_set_draw);
    view_set_input_callback(instance->view, analyzer_view_set_input);

    return instance;
}

void analyzer_view_set_free(AnalyzerViewSet* instance) {
    furi_check(instance);
    view_free(instance->view);
    free(instance);
}

View* analyzer_view_set_get_view(AnalyzerViewSet* instance) {
    furi_check(instance);
    return instance->view;
}

void analyzer_view_set_set_callback(
    AnalyzerViewSet* instance,
    AnalyzerViewSetCallback callback,
    void* context) {
    furi_check(instance);
    instance->callback = callback;
    instance->context = context;
}

void analyzer_view_set_set_frequency(AnalyzerViewSet* instance, uint32_t frequency) {
    furi_check(instance);
    with_view_model(
        instance->view,
        AnalyzerViewSetModel * m,
        {
            m->frequency = frequency;
            m->valid = frequency && analyzer_model_frequency_valid(frequency);
        },
        true);
}

uint32_t analyzer_view_set_frequency(AnalyzerViewSet* instance) {
    furi_check(instance);
    uint32_t frequency = 0;
    with_view_model(
        instance->view, AnalyzerViewSetModel * m, { frequency = m->frequency; }, false);
    return frequency;
}

bool analyzer_view_set_is_valid(AnalyzerViewSet* instance) {
    furi_check(instance);
    bool valid = false;
    with_view_model(instance->view, AnalyzerViewSetModel * m, { valid = m->valid; }, false);
    return valid;
}
