#include "analyzer_view_result.h"
#include "analyzer_ui.h"

#include <furi.h>
#include <gui/elements.h>

#include <stdio.h>
#include <string.h>

#define ANALYZER_RESULT_ROW1         21
#define ANALYZER_RESULT_ROW2         31
#define ANALYZER_RESULT_ROW3         41
#define ANALYZER_RESULT_ROW4         50
#define ANALYZER_RESULT_NOTIFY_TICKS 20u

typedef struct {
    AnalyzerResult result;
    AnalyzerPresetMatch best;
    bool have_match;
    char save_name[ANALYZER_PRESET_NAME_LEN];

    bool detail;

    char notify[28];
    uint8_t notify_ticks;
} AnalyzerViewResultModel;

struct AnalyzerViewResult {
    View* view;
    AnalyzerViewResultCallback callback;
    void* context;
};

static void analyzer_result_hz(char* out, size_t cap, uint32_t hz) {
    if(hz >= 1000000u) {
        snprintf(
            out,
            cap,
            "%lu.%02luM",
            (unsigned long)(hz / 1000000u),
            (unsigned long)((hz % 1000000u) / 10000u));
    } else if(hz >= 1000u) {
        snprintf(
            out,
            cap,
            "%lu.%01luk",
            (unsigned long)(hz / 1000u),
            (unsigned long)((hz % 1000u) / 100u));
    } else {
        snprintf(out, cap, "%lu", (unsigned long)hz);
    }
}

static const char* analyzer_result_axis_name(AnalyzerParam axis) {
    switch(axis) {
    case AnalyzerParamBandwidth:
        return "BW";
    case AnalyzerParamDeviation:
        return "dev";
    case AnalyzerParamRate:
    default:
        return "rate";
    }
}

static void analyzer_result_fit(Canvas* canvas, char* s, uint16_t width) {
    size_t len = strlen(s);
    while(len > 2u && canvas_string_width(canvas, s) > width) {
        len--;
        s[len - 2u] = '.';
        s[len - 1u] = '.';
        s[len] = '\0';
    }
}

static void analyzer_result_headline(const AnalyzerViewResultModel* m, char* out, size_t cap) {
    if(m->result.outcome != AnalyzerOutcomeTuned) {
        snprintf(out, cap, "%s", analyzer_outcome_name(m->result.outcome));
        return;
    }
    if(!m->have_match) {
        snprintf(out, cap, "Custom only");
        return;
    }
    if(m->best.exact_blob) {
        snprintf(out, cap, "%s saved", m->best.name);
        return;
    }
    snprintf(out, cap, "%s %s", m->best.name, analyzer_preset_tier_name(m->best.tier));
}

static void analyzer_result_draw_detail(Canvas* canvas, const AnalyzerViewResultModel* m) {
    const AnalyzerResult* r = &m->result;
    char buf[48];
    char a[12], b[12];

    canvas_set_font(canvas, FontSecondary);

    snprintf(buf, sizeof(buf), "score %u  confidence %u%%", r->score, r->confidence);
    canvas_draw_str(canvas, 2, 21, buf);

    if(r->symbol_us) {
        snprintf(
            buf,
            sizeof(buf),
            "symbol %luus -> %lu Bd",
            (unsigned long)r->symbol_us,
            (unsigned long)r->symbol_baud);
    } else {
        snprintf(buf, sizeof(buf), "symbol time not measured");
    }
    canvas_draw_str(canvas, 2, 31, buf);

    if(r->survey.valid && r->survey.occupied_bw_hz) {
        analyzer_result_hz(a, sizeof(a), r->survey.occupied_bw_hz);
        snprintf(buf, sizeof(buf), "occupied bw %sHz", a);
        canvas_draw_str(canvas, 2, 41, buf);
    }

    if(m->have_match && !m->best.exact_blob) {
        analyzer_result_hz(a, sizeof(a), m->best.worst_measured);
        analyzer_result_hz(b, sizeof(b), m->best.worst_preset);
        snprintf(
            buf,
            sizeof(buf),
            "%s: %s vs %s  gap %u",
            analyzer_result_axis_name(m->best.worst_axis),
            a,
            b,
            m->best.grid_delta);
        canvas_draw_str(canvas, 2, 51, buf);

        if(m->best.narrower_than_signal) {
            canvas_draw_str(canvas, 2, 61, "preset filter clips signal");
        }
    } else if(r->logged) {
        canvas_draw_str(canvas, 2, 61, "log written to SD");
    }
}

static bool analyzer_result_reuse_existing(const AnalyzerViewResultModel* m) {
    if(!m->have_match) return false;
    return m->best.exact_blob || m->best.tier == AnalyzerMatchEquivalent ||
           m->best.tier == AnalyzerMatchClose;
}

static void analyzer_view_result_draw(Canvas* canvas, void* context) {
    const AnalyzerViewResultModel* m = context;
    const AnalyzerModel* model = &m->result.model;
    char buf[48];

    canvas_clear(canvas);
    analyzer_ui_chrome(canvas, AnalyzerPipelineResult, model->frequency, m->result.peak_dbm, true);

    if(m->detail) {
        analyzer_result_draw_detail(canvas, m);
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    analyzer_result_headline(m, buf, sizeof(buf));
    analyzer_result_fit(canvas, buf, 125);
    canvas_draw_str(canvas, 2, ANALYZER_RESULT_ROW1, buf);

    canvas_set_font(canvas, FontSecondary);

    if(m->notify_ticks) {
        snprintf(buf, sizeof(buf), "%s", m->notify);
    } else if(model->mod == AnalyzerModOok) {
        snprintf(
            buf,
            sizeof(buf),
            "OOK %luk %luBd",
            (unsigned long)(analyzer_model_bandwidth_hz(model) / 1000u),
            (unsigned long)analyzer_model_rate_baud(model));
    } else {
        char dev[12];
        analyzer_result_hz(dev, sizeof(dev), analyzer_model_deviation_hz(model));
        snprintf(
            buf,
            sizeof(buf),
            "%s %luk d%s %luBd",
            (model->mod == AnalyzerModGfsk) ? "GFSK" : "2FSK",
            (unsigned long)(analyzer_model_bandwidth_hz(model) / 1000u),
            dev,
            (unsigned long)analyzer_model_rate_baud(model));
    }
    analyzer_result_fit(canvas, buf, 125);
    canvas_draw_str(canvas, 2, ANALYZER_RESULT_ROW2, buf);

    if(m->result.symbol_us) {
        snprintf(
            buf,
            sizeof(buf),
            "sym %luus  %lu Bd",
            (unsigned long)m->result.symbol_us,
            (unsigned long)m->result.symbol_baud);
    } else {
        snprintf(buf, sizeof(buf), "symbol time not measured");
    }
    analyzer_result_fit(canvas, buf, 125);
    canvas_draw_str(canvas, 2, ANALYZER_RESULT_ROW3, buf);

    if(analyzer_result_reuse_existing(m)) {
        snprintf(buf, sizeof(buf), "score %u  use existing", m->result.score);
    } else {
        snprintf(buf, sizeof(buf), "score %u  conf %u%%", m->result.score, m->result.confidence);
    }
    analyzer_result_fit(canvas, buf, 125);
    canvas_draw_str(canvas, 2, ANALYZER_RESULT_ROW4, buf);

    analyzer_ui_hint_center(canvas, "Actions");
}

static bool analyzer_view_result_input(InputEvent* event, void* context) {
    AnalyzerViewResult* instance = context;
    bool handled = true;
    bool emit = false;
    AnalyzerResultEvent out = AnalyzerResultEventBack;

    with_view_model(
        instance->view,
        AnalyzerViewResultModel * m,
        {
            if(event->key == InputKeyOk && event->type == InputTypeLong) {
                m->detail = !m->detail;

            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                if(m->detail) {
                    m->detail = false;
                } else {
                    emit = true;
                    out = AnalyzerResultEventActions;
                }
            } else if(event->key == InputKeyBack && event->type == InputTypeShort) {
                if(m->detail) {
                    m->detail = false;
                } else {
                    emit = true;
                    out = AnalyzerResultEventBack;
                }
            } else {
                handled = (event->key != InputKeyBack);
            }
        },
        true);

    if(emit && instance->callback) instance->callback(instance->context, out);
    return handled;
}

AnalyzerViewResult* analyzer_view_result_alloc(void) {
    AnalyzerViewResult* instance = malloc(sizeof(AnalyzerViewResult));
    memset(instance, 0, sizeof(*instance));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(AnalyzerViewResultModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, analyzer_view_result_draw);
    view_set_input_callback(instance->view, analyzer_view_result_input);

    return instance;
}

void analyzer_view_result_free(AnalyzerViewResult* instance) {
    furi_check(instance);
    view_free(instance->view);
    free(instance);
}

View* analyzer_view_result_get_view(AnalyzerViewResult* instance) {
    furi_check(instance);
    return instance->view;
}

void analyzer_view_result_set_callback(
    AnalyzerViewResult* instance,
    AnalyzerViewResultCallback callback,
    void* context) {
    furi_check(instance);
    instance->callback = callback;
    instance->context = context;
}

void analyzer_view_result_set(
    AnalyzerViewResult* instance,
    const AnalyzerResult* result,
    const AnalyzerPresetMatch* matches,
    uint8_t match_count,
    const char* save_name) {
    furi_check(instance);

    with_view_model(
        instance->view,
        AnalyzerViewResultModel * m,
        {
            m->result = *result;
            m->have_match = (match_count > 0);
            if(m->have_match) m->best = matches[0];
            snprintf(m->save_name, sizeof(m->save_name), "%s", save_name ? save_name : "?");
            m->detail = false;
            m->notify_ticks = 0;
        },
        true);
}

void analyzer_view_result_notify(AnalyzerViewResult* instance, const char* text) {
    furi_check(instance);
    with_view_model(
        instance->view,
        AnalyzerViewResultModel * m,
        {
            snprintf(m->notify, sizeof(m->notify), "%s", text);
            m->notify_ticks = ANALYZER_RESULT_NOTIFY_TICKS;
        },
        true);
}

void analyzer_view_result_tick(AnalyzerViewResult* instance) {
    furi_check(instance);
    bool changed = false;
    with_view_model(
        instance->view,
        AnalyzerViewResultModel * m,
        {
            if(m->notify_ticks) {
                m->notify_ticks--;
                changed = true;
            }
        },
        changed);
}

bool analyzer_view_result_reuse_existing(AnalyzerViewResult* instance) {
    furi_check(instance);
    bool reuse = false;
    with_view_model(
        instance->view,
        AnalyzerViewResultModel * m,
        { reuse = analyzer_result_reuse_existing(m); },
        false);
    return reuse;
}
