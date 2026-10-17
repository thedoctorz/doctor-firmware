#include "analyzer_view_tune.h"
#include "analyzer_ui.h"

#include <furi.h>
#include <gui/elements.h>

#include <stdio.h>
#include <string.h>

#define ANALYZER_TUNE_STARVED_HINT 4u

typedef struct {
    AnalyzerProgress progress;
    uint32_t frequency;
    uint8_t best_score;
    uint8_t phase;
} AnalyzerViewTuneModel;

struct AnalyzerViewTune {
    View* view;
    AnalyzerViewTuneCallback callback;
    void* context;
};

static void analyzer_view_tune_draw_gate(Canvas* canvas, const AnalyzerViewTuneModel* m) {
    char buf[24];

    canvas_clear(canvas);
    analyzer_ui_chrome(canvas, AnalyzerPipelineTune, m->frequency, 0.0f, false);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 24, "Ready to sweep");

    canvas_set_font(canvas, FontSecondary);
    analyzer_ui_freq_str(buf, sizeof(buf), m->frequency);
    canvas_draw_str(canvas, 2, 37, buf);

    if(m->phase == (uint8_t)AnalyzerTunePhaseStarting) {
        canvas_draw_str(canvas, 2, 49, "Starting... press the fob");
    } else {
        canvas_draw_str(canvas, 2, 49, "Press the fob");
        elements_button_center(canvas, "Start");
    }
}

static void analyzer_view_tune_draw(Canvas* canvas, void* context) {
    const AnalyzerViewTuneModel* m = context;

    if(m->phase != (uint8_t)AnalyzerTunePhaseRunning) {
        analyzer_view_tune_draw_gate(canvas, m);
        return;
    }

    const AnalyzerProgress* p = &m->progress;
    char buf[40];

    canvas_clear(canvas);
    analyzer_ui_chrome(
        canvas,
        AnalyzerPipelineTune,
        m->frequency,
        (float)p->level_raw / 2.0f - 74.0f,
        p->level_raw != 0);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 21, analyzer_stage_name(p->stage));

    canvas_set_font(canvas, FontSecondary);
    if(p->steps) {
        snprintf(buf, sizeof(buf), "%u/%u", p->step, p->steps);
        canvas_draw_str_aligned(canvas, 126, 21, AlignRight, AlignBottom, buf);
    }

    analyzer_ui_bar(canvas, 2, 25, 100, 9, p->progress);
    snprintf(buf, sizeof(buf), "%u%%", p->progress);
    canvas_draw_str(canvas, 106, 33, buf);

    const AnalyzerModel* model = &p->model;
    if(model->mod == AnalyzerModOok) {
        snprintf(
            buf,
            sizeof(buf),
            "OOK %lukHz %luBd",
            (unsigned long)(analyzer_model_bandwidth_hz(model) / 1000u),
            (unsigned long)analyzer_model_rate_baud(model));
    } else {
        snprintf(
            buf,
            sizeof(buf),
            "%s %lukHz %c%luk %luBd",
            (model->mod == AnalyzerModGfsk) ? "GFSK" : "2FSK",
            (unsigned long)(analyzer_model_bandwidth_hz(model) / 1000u),
            '~',
            (unsigned long)(analyzer_model_deviation_hz(model) / 1000u),
            (unsigned long)analyzer_model_rate_baud(model));
    }
    canvas_draw_str(canvas, 2, 44, buf);

    if(p->waiting) {
        canvas_draw_str(canvas, 2, 55, "Press fob");
        if(p->wait_s >= 3u) {
            snprintf(buf, sizeof(buf), "%us", p->wait_s);
            canvas_draw_str_aligned(canvas, 126, 55, AlignRight, AlignBottom, buf);
        }
    } else if(p->starved >= ANALYZER_TUNE_STARVED_HINT) {
        canvas_draw_str(canvas, 2, 55, "Press fob");
    } else if(m->best_score) {
        snprintf(buf, sizeof(buf), "best %u", m->best_score);
        canvas_draw_str(canvas, 2, 55, buf);
    } else if(p->eta_s) {
        if(p->eta_s >= 60u) {
            snprintf(buf, sizeof(buf), "~%um%02us", p->eta_s / 60u, p->eta_s % 60u);
        } else {
            snprintf(buf, sizeof(buf), "~%us left", p->eta_s);
        }
        canvas_draw_str(canvas, 2, 55, buf);
    } else {
        canvas_draw_str(canvas, 2, 55, "searching");
    }

    elements_button_center(canvas, "Stop");
}

static bool analyzer_view_tune_input(InputEvent* event, void* context) {
    AnalyzerViewTune* instance = context;
    if(event->type != InputTypeShort) return true;

    uint8_t phase = AnalyzerTunePhaseRunning;
    with_view_model(instance->view, AnalyzerViewTuneModel * m, { phase = m->phase; }, false);

    if(phase == (uint8_t)AnalyzerTunePhaseArmed) {
        if(event->key == InputKeyOk) {
            if(instance->callback) instance->callback(instance->context, AnalyzerTuneEventStart);
        } else if(event->key == InputKeyBack) {
            if(instance->callback) instance->callback(instance->context, AnalyzerTuneEventAbort);
        }
        return true;
    }

    if(phase == (uint8_t)AnalyzerTunePhaseStarting) {
        if(event->key == InputKeyBack && instance->callback) {
            instance->callback(instance->context, AnalyzerTuneEventAbort);
        }
        return true;
    }

    if(event->key == InputKeyOk || event->key == InputKeyBack) {
        if(instance->callback) instance->callback(instance->context, AnalyzerTuneEventAbort);
        return true;
    }
    return true;
}

AnalyzerViewTune* analyzer_view_tune_alloc(void) {
    AnalyzerViewTune* instance = malloc(sizeof(AnalyzerViewTune));
    memset(instance, 0, sizeof(*instance));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(AnalyzerViewTuneModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, analyzer_view_tune_draw);
    view_set_input_callback(instance->view, analyzer_view_tune_input);

    return instance;
}

void analyzer_view_tune_free(AnalyzerViewTune* instance) {
    furi_check(instance);
    view_free(instance->view);
    free(instance);
}

View* analyzer_view_tune_get_view(AnalyzerViewTune* instance) {
    furi_check(instance);
    return instance->view;
}

void analyzer_view_tune_set_callback(
    AnalyzerViewTune* instance,
    AnalyzerViewTuneCallback callback,
    void* context) {
    furi_check(instance);
    instance->callback = callback;
    instance->context = context;
}

void analyzer_view_tune_set_progress(
    AnalyzerViewTune* instance,
    const AnalyzerProgress* progress,
    uint32_t frequency,
    uint8_t best_score) {
    furi_check(instance);
    with_view_model(
        instance->view,
        AnalyzerViewTuneModel * m,
        {
            m->progress = *progress;
            m->frequency = frequency;
            m->best_score = best_score;
        },
        true);
}

void analyzer_view_tune_set_phase(AnalyzerViewTune* instance, AnalyzerTunePhase phase) {
    furi_check(instance);
    with_view_model(
        instance->view, AnalyzerViewTuneModel * m, { m->phase = (uint8_t)phase; }, true);
}

void analyzer_view_tune_set_frequency(AnalyzerViewTune* instance, uint32_t frequency) {
    furi_check(instance);
    with_view_model(
        instance->view, AnalyzerViewTuneModel * m, { m->frequency = frequency; }, true);
}
