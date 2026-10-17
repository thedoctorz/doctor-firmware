#include "analyzer_view_hunt.h"
#include "analyzer_hunt_worker.h"
#include "analyzer_model.h"
#include "analyzer_ui.h"

#include <float_tools.h>
#include <furi.h>
#include <gui/elements.h>

#include <stdio.h>
#include <string.h>

#define ANALYZER_HUNT_HISTORY 4u

#define ANALYZER_HUNT_DIGITS 6u

#define ANALYZER_HUNT_FREQ_BOX_Y 11
#define ANALYZER_HUNT_FREQ_BOX_H 18
#define ANALYZER_HUNT_ROW_Y      39
#define ANALYZER_HUNT_ROW_PITCH  10
#define ANALYZER_HUNT_BAR_STEPS \
    ((ANALYZER_HUNT_RSSI_MAX - ANALYZER_HUNT_RSSI_MIN) / ANALYZER_HUNT_RSSI_SCALE)
#define ANALYZER_HUNT_BAR_W ((uint8_t)(ANALYZER_HUNT_BAR_STEPS * 2.0f))
#define ANALYZER_HUNT_BAR_X ((128 - ANALYZER_HUNT_BAR_W) / 2)
#define ANALYZER_HUNT_BAR_Y 60
#define ANALYZER_HUNT_BAR_H 8

static const uint32_t analyzer_hunt_digit_step[ANALYZER_HUNT_DIGITS] =
    {100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u};

typedef struct {
    uint32_t frequency;
    uint16_t count;
} AnalyzerHuntHistory;

typedef struct {
    uint32_t frequency;
    float rssi;
    float rssi_peak;
    float trigger;
    bool locked;

    AnalyzerHuntHistory history[ANALYZER_HUNT_HISTORY];
    uint8_t history_count;
    uint8_t selected;
    bool show_frame;

    bool editing;
    uint32_t edit_frequency;
    uint8_t cursor;
    bool edit_valid;
    bool manual;
} AnalyzerViewHuntModel;

struct AnalyzerViewHunt {
    View* view;
    AnalyzerViewHuntCallback callback;
    void* context;
};

static void analyzer_view_hunt_emit(AnalyzerViewHunt* instance, AnalyzerHuntEvent event) {
    if(instance->callback) instance->callback(instance->context, event);
}

static void analyzer_view_hunt_draw_rssi(
    Canvas* canvas,
    float rssi,
    float peak,
    float trigger,
    uint8_t x,
    uint8_t y) {
    if(!float_is_equal(rssi, 0.0f)) {
        if(rssi > ANALYZER_HUNT_RSSI_MAX) rssi = ANALYZER_HUNT_RSSI_MAX;
        const float scaled = (rssi - ANALYZER_HUNT_RSSI_MIN) / ANALYZER_HUNT_RSSI_SCALE;
        uint8_t column = 0;
        for(size_t i = 0; i <= (uint8_t)scaled; i++) {
            if((i + 1u) % 4u) {
                column++;
                const uint8_t h = (column > ANALYZER_HUNT_BAR_H) ? ANALYZER_HUNT_BAR_H : column;
                canvas_draw_box(canvas, x + 2 * i, y - h, 2, h);
            }
        }
    }

    if(!float_is_equal(peak, 0.0f)) {
        if(peak > ANALYZER_HUNT_RSSI_MAX) peak = ANALYZER_HUNT_RSSI_MAX;
        const int max_x = (int)((peak - ANALYZER_HUNT_RSSI_MIN) / ANALYZER_HUNT_RSSI_SCALE) * 2;
        canvas_draw_line(canvas, x + max_x + 1, y - ANALYZER_HUNT_BAR_H, x + max_x + 1, y);
    }

    const float t = (trigger - ANALYZER_HUNT_RSSI_MIN) / ANALYZER_HUNT_RSSI_SCALE;
    const uint8_t tx = (uint8_t)((float)x + 2.0f * t);
    canvas_draw_dot(canvas, tx, y + 1);
    canvas_draw_line(canvas, tx - 1, y + 2, tx + 1, y + 2);

    canvas_draw_line(canvas, x, y, x + ANALYZER_HUNT_BAR_W, y);
}

static void analyzer_view_hunt_chevron(Canvas* canvas, int x, int cy, int dir) {
    canvas_draw_line(canvas, x, cy, x + dir * 4, cy - 4);
    canvas_draw_line(canvas, x, cy, x + dir * 4, cy + 4);
    canvas_draw_line(canvas, x + dir, cy, x + dir + dir * 4, cy - 4);
    canvas_draw_line(canvas, x + dir, cy, x + dir + dir * 4, cy + 4);
}

static void analyzer_view_hunt_draw_edit(Canvas* canvas, const AnalyzerViewHuntModel* m) {
    char text[12];
    snprintf(
        text,
        sizeof(text),
        "%03lu.%03lu",
        (unsigned long)(m->edit_frequency / 1000000u),
        (unsigned long)((m->edit_frequency % 1000000u) / 1000u));

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 21, "Set frequency");
    canvas_draw_str_aligned(canvas, 126, 21, AlignRight, AlignBottom, "MHz");

    const int pitch = 13;
    const int total = (int)strlen(text) * pitch;
    const int block_x = (128 - total) / 2;
    uint8_t digit = 0;

    for(size_t i = 0; i < strlen(text); i++) {
        const char c[2] = {text[i], '\0'};
        const int cx = block_x + (int)i * pitch;
        if(text[i] == '.') {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, cx, 31, AlignCenter, AlignTop, c);
            continue;
        }
        const bool here = (digit == m->cursor);
        canvas_set_font(canvas, here ? FontBigNumbers : FontPrimary);
        canvas_draw_str_aligned(canvas, cx, here ? 27 : 31, AlignCenter, AlignTop, c);
        digit++;
    }

    const int arrow_cy = 36;
    analyzer_view_hunt_chevron(canvas, block_x - 6, arrow_cy, -1);
    analyzer_view_hunt_chevron(canvas, block_x + total + 4, arrow_cy, +1);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 62, m->edit_valid ? "in band" : "no band");
    canvas_draw_str_aligned(canvas, 126, 62, AlignRight, AlignBottom, "UD digit");
    if(m->edit_valid) analyzer_ui_hint_center(canvas, "Set");
}

static void analyzer_view_hunt_draw(Canvas* canvas, void* context) {
    const AnalyzerViewHuntModel* m = context;
    canvas_clear(canvas);

    analyzer_ui_chrome(
        canvas, AnalyzerPipelineHunt, m->editing ? m->edit_frequency : 0, m->rssi, false);

    if(m->editing) {
        analyzer_view_hunt_draw_edit(canvas, m);
        return;
    }

    char buf[20];

    if(m->frequency) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 8, AlignRight, AlignBottom, "OK");
    }

    if(m->locked)
        canvas_draw_box(canvas, 2, ANALYZER_HUNT_FREQ_BOX_Y, 124, ANALYZER_HUNT_FREQ_BOX_H);
    canvas_set_color(canvas, m->locked ? ColorWhite : ColorBlack);

    if(m->frequency) {
        analyzer_ui_freq_short(buf, sizeof(buf), m->frequency);
    } else {
        snprintf(buf, sizeof(buf), "---.---");
    }
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, 6, ANALYZER_HUNT_FREQ_BOX_Y + ANALYZER_HUNT_FREQ_BOX_H - 1, buf);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 100, ANALYZER_HUNT_FREQ_BOX_Y + ANALYZER_HUNT_FREQ_BOX_H - 3, "MHz");
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < ANALYZER_HUNT_HISTORY; i++) {
        const int col = (i % 2) ? 66 : 4;
        const int y = ANALYZER_HUNT_ROW_Y + ((i < 2) ? 0 : ANALYZER_HUNT_ROW_PITCH);

        if(i < m->history_count) {
            analyzer_ui_freq_short(buf, sizeof(buf), m->history[i].frequency);
            canvas_draw_str(canvas, col, y, buf);
            if(m->history[i].count > 1) {
                char n[8];
                snprintf(n, sizeof(n), "x%u", m->history[i].count);
                canvas_draw_str(canvas, col + 42, y, n);
            }
        } else {
            canvas_draw_str(canvas, col, y, "---.---");
        }

        if(m->show_frame && i == m->selected && i < m->history_count) {
            canvas_draw_frame(canvas, col - 3, y - 9, 61, 11);
        }
    }

    elements_button_left(canvas, "T-");
    elements_button_right(canvas, "+T");
    analyzer_view_hunt_draw_rssi(
        canvas, m->rssi, m->rssi_peak, m->trigger, ANALYZER_HUNT_BAR_X, ANALYZER_HUNT_BAR_Y);
}

static void analyzer_view_hunt_edit_adjust(AnalyzerViewHuntModel* m, bool up) {
    const uint32_t step = analyzer_hunt_digit_step[m->cursor];
    int64_t next = (int64_t)m->edit_frequency + (up ? (int64_t)step : -(int64_t)step);

    if(next < 0) next = 0;
    if(next > 1000000000LL) next = 1000000000LL;

    m->edit_frequency = (uint32_t)next;
    m->edit_valid = analyzer_model_frequency_valid(m->edit_frequency);
}

static bool analyzer_view_hunt_input(InputEvent* event, void* context) {
    AnalyzerViewHunt* instance = context;
    bool handled = true;
    bool emit_lock = false;
    bool emit_trigger = false;
    bool emit_exit = false;

    const bool step = (event->type == InputTypePress) || (event->type == InputTypeRepeat);

    with_view_model(
        instance->view,
        AnalyzerViewHuntModel * m,
        {
            if(m->editing) {
                if(event->key == InputKeyBack) {
                    if(event->type == InputTypeShort) m->editing = false;
                } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                    if(m->edit_valid) {
                        m->frequency = m->edit_frequency;
                        m->editing = false;
                        m->manual = true;
                        emit_lock = true;
                    }
                } else if(step && event->key == InputKeyLeft) {
                    if(m->cursor > 0) m->cursor--;
                } else if(step && event->key == InputKeyRight) {
                    if(m->cursor + 1u < ANALYZER_HUNT_DIGITS) m->cursor++;
                } else if(step && event->key == InputKeyUp) {
                    analyzer_view_hunt_edit_adjust(m, true);
                } else if(step && event->key == InputKeyDown) {
                    analyzer_view_hunt_edit_adjust(m, false);
                } else {
                    handled = true;
                }
            } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
                m->editing = true;
                m->edit_frequency = m->frequency ? m->frequency : 433920000u;
                m->cursor = 2;
                m->edit_valid = analyzer_model_frequency_valid(m->edit_frequency);

            } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                if(m->frequency) emit_lock = true;

            } else if(step && event->key == InputKeyLeft) {
                m->trigger -= ANALYZER_HUNT_TRIGGER_STEP;
                if(m->trigger < ANALYZER_HUNT_RSSI_MIN) m->trigger = ANALYZER_HUNT_RSSI_MIN;
                emit_trigger = true;

            } else if(step && event->key == InputKeyRight) {
                m->trigger += ANALYZER_HUNT_TRIGGER_STEP;
                if(m->trigger > ANALYZER_HUNT_RSSI_MAX) m->trigger = ANALYZER_HUNT_RSSI_MAX;
                emit_trigger = true;

            } else if(step && event->key == InputKeyDown) {
                if(m->history_count) {
                    m->show_frame = true;
                    m->selected = (uint8_t)((m->selected + 1u) % m->history_count);
                }

            } else if(event->key == InputKeyBack && event->type == InputTypeShort) {
                if(m->show_frame) {
                    m->show_frame = false;
                } else {
                    emit_exit = true;
                }
            } else {
                handled = false;
            }
        },
        true);

    if(emit_lock) analyzer_view_hunt_emit(instance, AnalyzerHuntEventLock);
    if(emit_trigger) analyzer_view_hunt_emit(instance, AnalyzerHuntEventTriggerChanged);
    if(emit_exit) analyzer_view_hunt_emit(instance, AnalyzerHuntEventExit);

    return handled;
}

AnalyzerViewHunt* analyzer_view_hunt_alloc(void) {
    AnalyzerViewHunt* instance = malloc(sizeof(AnalyzerViewHunt));
    memset(instance, 0, sizeof(*instance));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(AnalyzerViewHuntModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, analyzer_view_hunt_draw);
    view_set_input_callback(instance->view, analyzer_view_hunt_input);

    with_view_model(
        instance->view,
        AnalyzerViewHuntModel * m,
        { m->trigger = ANALYZER_HUNT_TRIGGER_DEFAULT; },
        false);

    return instance;
}

void analyzer_view_hunt_free(AnalyzerViewHunt* instance) {
    furi_check(instance);
    view_free(instance->view);
    free(instance);
}

View* analyzer_view_hunt_get_view(AnalyzerViewHunt* instance) {
    furi_check(instance);
    return instance->view;
}

void analyzer_view_hunt_set_callback(
    AnalyzerViewHunt* instance,
    AnalyzerViewHuntCallback callback,
    void* context) {
    furi_check(instance);
    instance->callback = callback;
    instance->context = context;
}

void analyzer_view_hunt_update(
    AnalyzerViewHunt* instance,
    uint32_t frequency,
    float rssi,
    bool locked) {
    furi_check(instance);

    with_view_model(
        instance->view,
        AnalyzerViewHuntModel * m,
        {
            if(m->editing) {
            } else if(locked) {
                m->manual = false;
                m->frequency = frequency;
                m->rssi = rssi;
                if(rssi > m->rssi_peak || float_is_equal(m->rssi_peak, 0.0f)) m->rssi_peak = rssi;
                m->locked = true;
            } else {
                m->locked = false;
                m->rssi = 0.0f;

                if(frequency) {
                    bool found = false;
                    for(uint8_t i = 0; i < m->history_count; i++) {
                        if(m->history[i].frequency != frequency) continue;

                        const uint16_t count = (uint16_t)(m->history[i].count + 1u);
                        for(uint8_t j = i; j > 0; j--)
                            m->history[j] = m->history[j - 1u];
                        m->history[0].frequency = frequency;
                        m->history[0].count = count;
                        found = true;
                        break;
                    }

                    if(!found) {
                        for(uint8_t j = ANALYZER_HUNT_HISTORY - 1u; j > 0; j--) {
                            m->history[j] = m->history[j - 1u];
                        }
                        m->history[0].frequency = frequency;
                        m->history[0].count = 1;
                        if(m->history_count < ANALYZER_HUNT_HISTORY) m->history_count++;
                    }
                    m->frequency = frequency;
                }
            }
        },
        true);
}

uint32_t analyzer_view_hunt_selected(AnalyzerViewHunt* instance) {
    furi_check(instance);
    uint32_t frequency = 0;

    with_view_model(
        instance->view,
        AnalyzerViewHuntModel * m,
        {
            if(m->show_frame && m->selected < m->history_count && !m->locked) {
                frequency = m->history[m->selected].frequency;
            } else {
                frequency = m->frequency;
            }
        },
        false);

    return frequency;
}

float analyzer_view_hunt_trigger(AnalyzerViewHunt* instance) {
    furi_check(instance);
    float trigger = ANALYZER_HUNT_TRIGGER_DEFAULT;
    with_view_model(instance->view, AnalyzerViewHuntModel * m, { trigger = m->trigger; }, false);
    return trigger;
}

void analyzer_view_hunt_set_trigger(AnalyzerViewHunt* instance, float trigger) {
    furi_check(instance);
    with_view_model(instance->view, AnalyzerViewHuntModel * m, { m->trigger = trigger; }, true);
}

bool analyzer_view_hunt_is_manual(AnalyzerViewHunt* instance) {
    furi_check(instance);
    bool manual = false;
    with_view_model(instance->view, AnalyzerViewHuntModel * m, { manual = m->manual; }, false);
    return manual;
}
