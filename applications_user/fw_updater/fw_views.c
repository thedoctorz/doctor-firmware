#include "fw_views.h"

#include <fw_updater_icons.h>
#include <gui/elements.h>

#define HEADER_HEIGHT 13

#define LIST_ROW_COUNT   3
#define LIST_ICON_X      4
#define LIST_LABEL_X     22
#define LIST_LABEL_WIDTH 100

#define DETAIL_BODY_TOP   HEADER_HEIGHT
#define DETAIL_LINE_PITCH 9
#define DETAIL_LINE_COUNT 4
#define DETAIL_FOOTER_TOP 51
#define DETAIL_TEXT_WIDTH 110

#define CONFIRM_HOLD_MS 900
#define CONFIRM_TICK_MS 40
#define LIST_BLINK_MS   400

static void fw_ui_fit_text(
    Canvas* canvas,
    char* output,
    size_t output_size,
    const char* text,
    size_t max_width) {
    snprintf(output, output_size, "%s", text);

    if(canvas_string_width(canvas, output) <= max_width) {
        return;
    }

    /* Rebuild from the source on every attempt, so the dots never eat into
       text that is still being measured */
    for(size_t keep = strlen(output); keep > 2; keep--) {
        snprintf(output, MIN(output_size, keep), "%s", text);
        const size_t length = strlen(output);
        if(length < 2) {
            break;
        }
        output[length - 2] = '.';
        output[length - 1] = '.';

        if(canvas_string_width(canvas, output) <= max_width) {
            return;
        }
    }

    output[0] = '\0';
}

typedef struct {
    FwPkgEntry* entries;
    size_t count;
    size_t selected;
    bool blink_on;
} FwListModel;

struct FwListView {
    View* view;
    FuriTimer* blink_timer;
    FwViewCallback callback;
    void* context;
};

static const uint8_t fw_list_icon_y[LIST_ROW_COUNT] = {3, 25, 47};
static const uint8_t fw_list_label_y[LIST_ROW_COUNT] = {14, 36, 58};
static const uint8_t fw_list_row_y[LIST_ROW_COUNT] = {0, 21, 42};
static const uint8_t fw_list_row_h[LIST_ROW_COUNT] = {21, 21, 22};

static const Icon* fw_list_entry_icon(const FwPkgEntry* entry) {
    return (entry->kind == FwPkgKindArchive) ? &I_Archive_14x14 : &I_Firmware_14x14;
}

static void fw_list_view_draw(Canvas* canvas, void* context) {
    FwListModel* model = context;
    char source[FW_PKG_LABEL_LEN + 4];
    char text[FW_PKG_LABEL_LEN + 4];

    canvas_clear(canvas);

    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 32, "No firmware");
        canvas_draw_str(canvas, 2, 44, "Put packages in /update");
        elements_scrollbar(canvas, 0, 0);
        return;
    }

    for(size_t row = 0; row < LIST_ROW_COUNT; row++) {
        int32_t index = (int32_t)model->selected + (int32_t)row - 1;
        if((int32_t)model->count >= (int32_t)LIST_ROW_COUNT) {
            index = (index + (int32_t)model->count) % (int32_t)model->count;
        } else if((index < 0) || (index >= (int32_t)model->count)) {
            continue;
        }

        const FwPkgEntry* entry = &model->entries[index];
        const bool invert = entry->current && model->blink_on;

        if(invert) {
            canvas_draw_box(canvas, 0, fw_list_row_y[row], 125, fw_list_row_h[row]);
            canvas_set_color(canvas, ColorWhite);
        }

        canvas_set_font(canvas, (row == 1) ? FontPrimary : FontSecondary);
        canvas_draw_icon(canvas, LIST_ICON_X, fw_list_icon_y[row], fw_list_entry_icon(entry));

        const char* title = entry->name[0] ? entry->name : entry->label;
        if(entry->armed) {
            snprintf(source, sizeof(source), "%s *", title);
        } else {
            snprintf(source, sizeof(source), "%s", title);
        }
        fw_ui_fit_text(canvas, text, sizeof(text), source, LIST_LABEL_WIDTH);
        canvas_draw_str(canvas, LIST_LABEL_X, fw_list_label_y[row], text);
        canvas_set_color(canvas, ColorBlack);
    }

    elements_frame(canvas, 0, 21, 128 - 5, 21);
    elements_scrollbar(canvas, model->selected, model->count);
}

static bool fw_list_view_input(InputEvent* event, void* context) {
    FwListView* list = context;
    bool consumed = false;
    bool open = false;

    if((event->type != InputTypeShort) && (event->type != InputTypeRepeat)) {
        return false;
    }

    with_view_model(
        list->view,
        FwListModel * model,
        {
            if(model->count) {
                if(event->key == InputKeyUp) {
                    model->selected = (model->selected == 0) ? (model->count - 1) :
                                                               (model->selected - 1);
                    consumed = true;
                } else if(event->key == InputKeyDown) {
                    model->selected = (model->selected + 1) % model->count;
                    consumed = true;
                } else if((event->key == InputKeyOk) && (event->type == InputTypeShort)) {
                    open = true;
                    consumed = true;
                }
            }
        },
        consumed);

    if(open && list->callback) {
        list->callback(FwEventOpenSelected, list->context);
    }

    return consumed;
}

static void fw_list_view_blink(void* context) {
    FwListView* list = context;
    with_view_model(
        list->view, FwListModel * model, { model->blink_on = !model->blink_on; }, true);
}

static bool fw_list_view_has_current(FwListView* list) {
    bool found = false;
    with_view_model(
        list->view,
        FwListModel * model,
        {
            for(size_t i = 0; i < model->count; i++) {
                if(model->entries[i].current) {
                    found = true;
                    break;
                }
            }
        },
        false);
    return found;
}

static void fw_list_view_enter(void* context) {
    FwListView* list = context;
    if(fw_list_view_has_current(list)) {
        furi_timer_start(list->blink_timer, furi_ms_to_ticks(LIST_BLINK_MS));
    }
}

static void fw_list_view_exit(void* context) {
    FwListView* list = context;
    furi_timer_stop(list->blink_timer);
}

FwListView* fw_list_view_alloc(void) {
    FwListView* list = malloc(sizeof(FwListView));
    list->callback = NULL;
    list->context = NULL;
    list->view = view_alloc();
    list->blink_timer = furi_timer_alloc(fw_list_view_blink, FuriTimerTypePeriodic, list);

    view_allocate_model(list->view, ViewModelTypeLocking, sizeof(FwListModel));
    view_set_draw_callback(list->view, fw_list_view_draw);
    view_set_input_callback(list->view, fw_list_view_input);
    view_set_enter_callback(list->view, fw_list_view_enter);
    view_set_exit_callback(list->view, fw_list_view_exit);
    view_set_context(list->view, list);

    with_view_model(list->view, FwListModel * model, { model->blink_on = true; }, false);
    return list;
}

void fw_list_view_free(FwListView* list) {
    furi_timer_stop(list->blink_timer);
    furi_timer_free(list->blink_timer);
    view_free(list->view);
    free(list);
}

View* fw_list_view_get_view(FwListView* list) {
    return list->view;
}

void fw_list_view_set_callback(FwListView* list, FwViewCallback callback, void* context) {
    list->callback = callback;
    list->context = context;
}

void fw_list_view_set_entries(FwListView* list, FwPkgEntry* entries, size_t count) {
    with_view_model(
        list->view,
        FwListModel * model,
        {
            model->entries = entries;
            model->count = count;
            if(model->selected >= count) {
                model->selected = count ? (count - 1) : 0;
            }
        },
        true);
}

size_t fw_list_view_get_selected(FwListView* list) {
    size_t selected = 0;
    with_view_model(list->view, FwListModel * model, { selected = model->selected; }, false);
    return selected;
}

typedef struct {
    char title[FW_PKG_LABEL_LEN];
    char lines[FW_DETAIL_MAX_LINES][FW_DETAIL_LINE_LEN];
    uint8_t kinds[FW_DETAIL_MAX_LINES];
    uint8_t count;
    uint8_t scroll;
    bool installable;
    bool armed;
    bool confirming;
    uint32_t hold_ms;
} FwDetailModel;

struct FwDetailView {
    View* view;
    FuriTimer* hold_timer;
    FwViewCallback callback;
    void* context;
};

static void fw_detail_view_draw_confirm(Canvas* canvas, const FwDetailModel* model) {
    char text[FW_PKG_LABEL_LEN + 8];

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, 4, 6, 120, 52, 4);
    canvas_set_color(canvas, ColorBlack);
    elements_bold_rounded_frame(canvas, 4, 6, 120, 52);

    canvas_set_font(canvas, FontPrimary);
    fw_ui_fit_text(canvas, text, sizeof(text), model->title, 104);
    canvas_draw_str_aligned(canvas, 64, 21, AlignCenter, AlignBottom, text);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignBottom, "Hold OK to install");

    const float progress = (float)model->hold_ms / (float)CONFIRM_HOLD_MS;
    elements_progress_bar(canvas, 14, 37, 100, progress > 1.0f ? 1.0f : progress);

    canvas_draw_str_aligned(canvas, 64, 55, AlignCenter, AlignBottom, "Back cancels");
}

static void fw_detail_view_draw(Canvas* canvas, void* context) {
    FwDetailModel* model = context;
    char text[FW_DETAIL_LINE_LEN + 8];

    canvas_clear(canvas);

    canvas_draw_box(canvas, 0, 0, 128, HEADER_HEIGHT);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    fw_ui_fit_text(canvas, text, sizeof(text), model->title, 120);
    canvas_draw_str(canvas, 4, 10, text);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t row = 0; row < DETAIL_LINE_COUNT; row++) {
        const uint8_t index = model->scroll + row;
        if(index >= model->count) {
            break;
        }

        const int32_t baseline = DETAIL_BODY_TOP + 8 + (int32_t)(row * DETAIL_LINE_PITCH);
        int32_t left = 4;

        if(model->kinds[index] == FwLineBlocker) {
            canvas_draw_box(canvas, 3, baseline - 6, 3, 7);
            left = 9;
        } else if(model->kinds[index] == FwLineWarning) {
            canvas_draw_frame(canvas, 3, baseline - 6, 3, 7);
            left = 9;
        }

        fw_ui_fit_text(canvas, text, sizeof(text), model->lines[index], DETAIL_TEXT_WIDTH);
        canvas_draw_str(canvas, left, baseline, text);
    }

    if(model->count > DETAIL_LINE_COUNT) {
        elements_scrollbar_pos(
            canvas,
            127,
            DETAIL_BODY_TOP,
            DETAIL_FOOTER_TOP - DETAIL_BODY_TOP,
            model->scroll,
            model->count - DETAIL_LINE_COUNT + 1);
    }

    if(model->confirming) {
        fw_detail_view_draw_confirm(canvas, model);
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    if(model->armed) {
        elements_button_center(canvas, "Cancel");
    } else if(model->installable) {
        elements_button_center(canvas, "Install");
    }
}

static void fw_detail_view_hold_tick(void* context) {
    FwDetailView* detail = context;
    bool finished = false;

    with_view_model(
        detail->view,
        FwDetailModel * model,
        {
            if(model->confirming) {
                model->hold_ms += CONFIRM_TICK_MS;
                finished = model->hold_ms >= CONFIRM_HOLD_MS;
            }
        },
        true);

    if(finished) {
        furi_timer_stop(detail->hold_timer);
        if(detail->callback) {
            detail->callback(FwEventInstall, detail->context);
        }
    }
}

static bool fw_detail_view_input(InputEvent* event, void* context) {
    FwDetailView* detail = context;
    bool consumed = false;
    bool start_hold = false;
    bool stop_hold = false;
    bool cancel_pending = false;
    bool confirming = false;

    with_view_model(
        detail->view, FwDetailModel * model, { confirming = model->confirming; }, false);

    if(confirming) {
        if((event->key == InputKeyOk) && (event->type == InputTypePress)) {
            with_view_model(detail->view, FwDetailModel * model, { model->hold_ms = 0; }, true);
            start_hold = true;
        } else if((event->key == InputKeyOk) && (event->type == InputTypeRelease)) {
            with_view_model(detail->view, FwDetailModel * model, { model->hold_ms = 0; }, true);
            stop_hold = true;
        } else if((event->key == InputKeyBack) && (event->type == InputTypeShort)) {
            stop_hold = true;
            with_view_model(
                detail->view,
                FwDetailModel * model,
                {
                    model->confirming = false;
                    model->hold_ms = 0;
                },
                true);
        }

        if(start_hold) {
            furi_timer_start(detail->hold_timer, furi_ms_to_ticks(CONFIRM_TICK_MS));
        }
        if(stop_hold) {
            furi_timer_stop(detail->hold_timer);
        }
        return true;
    }

    with_view_model(
        detail->view,
        FwDetailModel * model,
        {
            if((event->type == InputTypeShort) || (event->type == InputTypeRepeat)) {
                if((event->key == InputKeyUp) && model->scroll) {
                    model->scroll--;
                    consumed = true;
                } else if(
                    (event->key == InputKeyDown) &&
                    ((model->scroll + DETAIL_LINE_COUNT) < model->count)) {
                    model->scroll++;
                    consumed = true;
                }
            }

            if(event->key == InputKeyOk) {
                if(event->type == InputTypeShort) {
                    if(model->armed) {
                        cancel_pending = true;
                    } else if(model->installable) {
                        model->confirming = true;
                        model->hold_ms = 0;
                    }
                }
                consumed = true;
            }
        },
        consumed);

    if(cancel_pending && detail->callback) {
        detail->callback(FwEventCancelPending, detail->context);
    }

    return consumed;
}

FwDetailView* fw_detail_view_alloc(void) {
    FwDetailView* detail = malloc(sizeof(FwDetailView));
    detail->callback = NULL;
    detail->context = NULL;
    detail->view = view_alloc();
    detail->hold_timer = furi_timer_alloc(fw_detail_view_hold_tick, FuriTimerTypePeriodic, detail);

    view_allocate_model(detail->view, ViewModelTypeLocking, sizeof(FwDetailModel));
    view_set_draw_callback(detail->view, fw_detail_view_draw);
    view_set_input_callback(detail->view, fw_detail_view_input);
    view_set_context(detail->view, detail);
    return detail;
}

void fw_detail_view_free(FwDetailView* detail) {
    furi_timer_stop(detail->hold_timer);
    furi_timer_free(detail->hold_timer);
    view_free(detail->view);
    free(detail);
}

View* fw_detail_view_get_view(FwDetailView* detail) {
    return detail->view;
}

void fw_detail_view_set_callback(FwDetailView* detail, FwViewCallback callback, void* context) {
    detail->callback = callback;
    detail->context = context;
}

void fw_detail_view_set_package(
    FwDetailView* detail,
    const char* title,
    bool installable,
    bool armed) {
    furi_timer_stop(detail->hold_timer);

    with_view_model(
        detail->view,
        FwDetailModel * model,
        {
            snprintf(model->title, sizeof(model->title), "%s", title);
            model->count = 0;
            model->scroll = 0;
            model->installable = installable;
            model->armed = armed;
            model->confirming = false;
            model->hold_ms = 0;
        },
        true);
}

void fw_detail_view_add_line(FwDetailView* detail, FwLineKind kind, const char* text) {
    with_view_model(
        detail->view,
        FwDetailModel * model,
        {
            if(model->count < FW_DETAIL_MAX_LINES) {
                snprintf(model->lines[model->count], FW_DETAIL_LINE_LEN, "%s", text);
                model->kinds[model->count] = kind;
                model->count++;
            }
        },
        true);
}

typedef struct {
    char title[32];
    char detail[40];
    uint8_t percent;
} FwBusyModel;

struct FwBusyView {
    View* view;
};

static void fw_busy_view_draw(Canvas* canvas, void* context) {
    FwBusyModel* model = context;

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 21, AlignCenter, AlignBottom, model->title);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignBottom, model->detail);

    elements_progress_bar(canvas, 14, 38, 100, (float)model->percent / 100.0f);

    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", model->percent);
    canvas_draw_str_aligned(canvas, 64, 61, AlignCenter, AlignBottom, percent);
}

static bool fw_busy_view_input(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return true;
}

FwBusyView* fw_busy_view_alloc(void) {
    FwBusyView* busy = malloc(sizeof(FwBusyView));
    busy->view = view_alloc();

    view_allocate_model(busy->view, ViewModelTypeLocking, sizeof(FwBusyModel));
    view_set_draw_callback(busy->view, fw_busy_view_draw);
    view_set_input_callback(busy->view, fw_busy_view_input);
    view_set_context(busy->view, busy);
    return busy;
}

void fw_busy_view_free(FwBusyView* busy) {
    view_free(busy->view);
    free(busy);
}

View* fw_busy_view_get_view(FwBusyView* busy) {
    return busy->view;
}

void fw_busy_view_set(FwBusyView* busy, const char* title, const char* detail) {
    with_view_model(
        busy->view,
        FwBusyModel * model,
        {
            snprintf(model->title, sizeof(model->title), "%s", title);
            snprintf(model->detail, sizeof(model->detail), "%s", detail);
            model->percent = 0;
        },
        true);
}

void fw_busy_view_set_progress(FwBusyView* busy, uint8_t percent) {
    with_view_model(busy->view, FwBusyModel * model, { model->percent = percent; }, true);
}
