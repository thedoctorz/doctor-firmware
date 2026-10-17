#include "receiver.h"
#include "../subghz_i.h"

#include "types.h"
#include <input/input.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <m-array.h>
#include <string.h>
#include <math.h>

#define FRAME_HEIGHT                   12
#define MAX_LEN_PX                     111
#define MENU_ITEMS                     4u
#define UNLOCK_CNT                     3
#define SUBGHZ_RX_RSSI_HISTORY_SIZE    128
#define SUBGHZ_RX_RSSI_DISPLAY_MIN     (-110.0f)
#define SUBGHZ_RX_RSSI_DISPLAY_MAX     (-50.0f)
#define SUBGHZ_RX_RSSI_WAVEFORM_HEIGHT 33
#define SUBGHZ_RX_RSSI_EXCURSION_DB    35

#define FLIP_TIMEOUT                   (500)
#define SUBGHZ_RX_HOP_UI_MS            10
#define SUBGHZ_RX_DELETE_FREQ_GUARD_MS 300

typedef struct {
    FuriString* item_str;
    FuriString* time;
    uint8_t type;
    SubGhzHistoryButtonIcon button_icon;
} SubGhzReceiverMenuItem;

ARRAY_DEF(SubGhzReceiverMenuItemArray, SubGhzReceiverMenuItem, M_POD_OPLIST) //-V658

#define M_OPL_SubGhzReceiverMenuItemArray_t() \
    ARRAY_OPLIST(SubGhzReceiverMenuItemArray, M_POD_OPLIST)

struct SubGhzReceiverHistory {
    SubGhzReceiverMenuItemArray_t data;
};

typedef struct SubGhzReceiverHistory SubGhzReceiverHistory;

static const Icon* ReceiverItemIcons[] = {
    [SubGhzProtocolTypeUnknown] = &I_Quest_7x8,
    [SubGhzProtocolTypeStatic] = &I_Static_9x7,
    [SubGhzProtocolTypeDynamic] = &I_Dynamic_9x7,
    [SubGhzProtocolTypeBinRAW] = &I_Raw_9x7,
};

typedef enum {
    SubGhzViewReceiverBarShowDefault,
    SubGhzViewReceiverBarShowLock,
    SubGhzViewReceiverBarShowToUnlockPress,
    SubGhzViewReceiverBarShowUnlock,
} SubGhzViewReceiverBarShow;

struct SubGhzViewReceiver {
    bool lock;
    uint8_t lock_count;
    FuriTimer* timer;
    View* view;
    SubGhzViewReceiverCallback callback;
    void* context;
    FuriTimer* flip_time_timer;
    FuriTimer* hop_ui_timer;
    SubGhzViewReceiverFrequencyCallback frequency_callback;
    uint32_t hop_ui_frequency;

    bool delete_freq_guard;
    uint32_t delete_freq_guard_until;
};

typedef struct {
    FuriString* frequency_str;
    FuriString* preset_str;
    FuriString* history_stat_str;
    FuriString* progress_str;
    bool hopping_enabled;
    bool bin_raw_enabled;
    bool gps_active;
    bool gps_companion;
    uint8_t gps_sats;
    SubGhzReceiverHistory* history;
    uint16_t idx;
    uint16_t list_offset;
    uint16_t history_item;
    SubGhzViewReceiverBarShow bar_show;
    SubGhzViewReceiverMode mode;
    uint8_t u_rssi;

    SubGhzRadioDeviceType device_type;

    bool show_time;

    bool nodraw;

    bool dual_mode;
    bool dual_dashboard;
    FuriString* dual_int_str;
    FuriString* dual_ext_str;
    uint8_t rssi_history_ext[SUBGHZ_RX_RSSI_HISTORY_SIZE];
    uint8_t rssi_history_ext_index;

    uint8_t rssi_history[SUBGHZ_RX_RSSI_HISTORY_SIZE];
    uint8_t rssi_history_index;
} SubGhzViewReceiverModel;

static uint8_t subghz_receiver_bar_from_span(float value, float min, float max) {
    if(value <= min) {
        return 1;
    }
    if(value >= max) {
        return SUBGHZ_RX_RSSI_WAVEFORM_HEIGHT;
    }
    const uint8_t bar_h =
        (uint8_t)(((value - min) / (max - min)) * SUBGHZ_RX_RSSI_WAVEFORM_HEIGHT);
    return (bar_h < 1) ? 1 : bar_h;
}

static uint8_t subghz_receiver_rssi_to_bar(float rssi) {
    return subghz_receiver_bar_from_span(
        rssi, SUBGHZ_RX_RSSI_DISPLAY_MIN, SUBGHZ_RX_RSSI_DISPLAY_MAX);
}

static uint8_t subghz_receiver_rssi_to_bar_ref(float rssi, float floor) {
    return subghz_receiver_bar_from_span(rssi - floor, 0.0f, (float)SUBGHZ_RX_RSSI_EXCURSION_DB);
}

void subghz_view_receiver_set_mode(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverMode mode) {
    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->mode = mode; }, true);
}

void subghz_receiver_rssi(SubGhzViewReceiver* instance, float rssi, float floor) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        SubGhzViewReceiverModel * model,
        {
            model->u_rssi = isnan(floor) ? subghz_receiver_rssi_to_bar(rssi) :
                                           subghz_receiver_rssi_to_bar_ref(rssi, floor);
            model->rssi_history[model->rssi_history_index] = model->u_rssi;
            model->rssi_history_index =
                (model->rssi_history_index + 1) % SUBGHZ_RX_RSSI_HISTORY_SIZE;
        },
        true);
}

void subghz_view_receiver_rssi_ext(SubGhzViewReceiver* instance, float rssi) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        SubGhzViewReceiverModel * model,
        {
            model->rssi_history_ext[model->rssi_history_ext_index] =
                subghz_receiver_rssi_to_bar(rssi);
            model->rssi_history_ext_index =
                (model->rssi_history_ext_index + 1) % SUBGHZ_RX_RSSI_HISTORY_SIZE;
        },
        true);
}

void subghz_view_receiver_set_lock(SubGhzViewReceiver* subghz_receiver, bool lock) {
    furi_assert(subghz_receiver);
    subghz_receiver->lock_count = 0;
    if(lock == true) {
        subghz_receiver->lock = lock;
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            { model->bar_show = SubGhzViewReceiverBarShowLock; },
            true);
        furi_timer_start(subghz_receiver->timer, 1000);
    } else {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            { model->bar_show = SubGhzViewReceiverBarShowDefault; },
            true);
    }
}

void subghz_view_receiver_set_callback(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverCallback callback,
    void* context) {
    furi_assert(subghz_receiver);
    furi_assert(callback);
    subghz_receiver->callback = callback;
    subghz_receiver->context = context;
}

void subghz_view_receiver_set_frequency_callback(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverFrequencyCallback callback) {
    furi_assert(subghz_receiver);
    subghz_receiver->frequency_callback = callback;
}

static void subghz_view_receiver_format_frequency(FuriString* str, uint32_t frequency) {
    furi_string_printf(
        str, "%03ld.%02ld", (long)(frequency / 1000000 % 1000), (long)(frequency / 10000 % 100));
}

static void subghz_view_receiver_hop_ui_timer_callback(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    if(!subghz_receiver->frequency_callback || !subghz_receiver->context) {
        return;
    }

    const uint32_t frequency = subghz_receiver->frequency_callback(subghz_receiver->context);
    if(frequency == subghz_receiver->hop_ui_frequency) {
        return;
    }
    subghz_receiver->hop_ui_frequency = frequency;

    bool update = false;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            if(model->hopping_enabled) {
                subghz_view_receiver_format_frequency(model->frequency_str, frequency);
                update = true;
            }
        },
        update);
}

static void subghz_view_receiver_update_offset(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            size_t history_item = model->history_item;
            uint16_t bounds = history_item > 3 ? 2 : history_item;

            if(history_item > 3 && model->idx >= (int16_t)(history_item - 1)) {
                model->list_offset = model->idx - 3;
            } else if(model->list_offset < model->idx - bounds) {
                model->list_offset =
                    CLAMP(model->list_offset + 1, (int16_t)(history_item - bounds), 0);
            } else if(model->list_offset > model->idx - bounds) {
                model->list_offset = CLAMP(model->idx - 1, (int16_t)(history_item - bounds), 0);
            }
        },
        true);
}

static void subghz_view_receiver_show_time_moment(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->show_time = true; }, true);
    furi_timer_start(subghz_receiver->flip_time_timer, FLIP_TIMEOUT);
}

static void subghz_view_receiver_flip_string_callback(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->show_time = false; },
        true);
}

void subghz_view_receiver_add_item_to_menu(
    SubGhzViewReceiver* subghz_receiver,
    const char* name,
    const char* time,
    uint8_t type,
    SubGhzHistoryButtonIcon button_icon) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            SubGhzReceiverMenuItem* item_menu =
                SubGhzReceiverMenuItemArray_push_raw(model->history->data);
            item_menu->time = furi_string_alloc_set(time);
            item_menu->item_str = furi_string_alloc_set(name);
            item_menu->type = type;
            item_menu->button_icon = button_icon;
            if(model->idx == model->history_item - 1) {
                model->history_item++;
                model->idx++;
                subghz_view_receiver_show_time_moment(subghz_receiver);
            } else {
                model->history_item++;
            }
        },
        true);
    subghz_view_receiver_update_offset(subghz_receiver);
}

void subghz_view_receiver_add_data_statusbar(
    SubGhzViewReceiver* subghz_receiver,
    const char* frequency_str,
    const char* preset_str,
    const char* history_stat_str,
    bool hopping_enabled,
    bool bin_raw_enabled,
    bool gps_active,
    bool gps_companion,
    uint8_t gps_sats) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            furi_string_set(model->frequency_str, frequency_str);
            furi_string_set(model->preset_str, preset_str);
            furi_string_set(model->history_stat_str, history_stat_str);
            model->hopping_enabled = hopping_enabled;
            model->bin_raw_enabled = bin_raw_enabled;
            model->gps_active = gps_active;
            model->gps_companion = gps_companion;
            model->gps_sats = gps_sats;
        },
        true);

    if(hopping_enabled && subghz_receiver->frequency_callback) {
        if(!furi_timer_is_running(subghz_receiver->hop_ui_timer)) {
            subghz_receiver->hop_ui_frequency = 0;
            furi_timer_start(subghz_receiver->hop_ui_timer, furi_ms_to_ticks(SUBGHZ_RX_HOP_UI_MS));
        }
    } else {
        furi_timer_stop(subghz_receiver->hop_ui_timer);
        subghz_receiver->hop_ui_frequency = 0;
    }
}

static uint8_t subghz_view_config_button_center_width(Canvas* canvas) {
    canvas_set_font(canvas, FontSecondary);
    const size_t string_width = canvas_string_width(canvas, "Config");
    return (uint8_t)(string_width + 1 * 2 + 7 + 3);
}

static uint8_t subghz_view_config_button_center_x(Canvas* canvas) {
    return (uint8_t)((128 - subghz_view_config_button_center_width(canvas)) / 2);
}

static void
    subghz_view_gps_badge_draw(Canvas* canvas, const char* label, uint8_t x, uint8_t baseline_y) {
    canvas_set_font(canvas, FontSecondary);
    const uint8_t icon_y = baseline_y >= 9 ? (uint8_t)(baseline_y - 9) : 0;
    canvas_draw_icon(canvas, x, icon_y, &I_Sats_6x9);
    canvas_draw_str(canvas, (uint8_t)(x + 8), baseline_y, label);
}

static void subghz_view_gps_label(char* buf, size_t size, SubGhzViewReceiverModel* model) {
    if(model->gps_companion) {
        snprintf(buf, size, "P");
    } else {
        snprintf(buf, size, "%u", model->gps_sats);
    }
}

static void subghz_view_receiver_draw_antenna_icon_xy(
    Canvas* canvas,
    SubGhzViewReceiverModel* model,
    uint8_t x,
    uint8_t y) {
    canvas_draw_icon(
        canvas,
        x,
        y,
        (model->device_type == SubGhzRadioDeviceTypeInternal) ? &I_Internal_ant_1_9x11 :
                                                                &I_External_ant_1_9x11);
}

static void
    subghz_view_receiver_draw_antenna_icon(Canvas* canvas, SubGhzViewReceiverModel* model) {
    subghz_view_receiver_draw_antenna_icon_xy(canvas, model, 109, 0);
}

static void subghz_view_receiver_draw_preset_bottom_bar(Canvas* canvas, const char* preset_str) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 62, preset_str);
}

static void subghz_view_receiver_draw_live_preset(Canvas* canvas, SubGhzViewReceiverModel* model) {
    subghz_view_receiver_draw_antenna_icon(canvas, model);
    subghz_view_receiver_draw_preset_bottom_bar(canvas, furi_string_get_cstr(model->preset_str));
}

static void subghz_view_draw_solid_arrow(
    Canvas* canvas,
    uint8_t apex_x,
    uint8_t cy,
    uint8_t width,
    uint8_t half_height,
    bool point_left) {
    for(uint8_t i = 0; i <= width; i++) {
        const uint8_t half = (uint8_t)((half_height * i + width / 2) / width);
        const uint8_t col = point_left ? (uint8_t)(apex_x + i) : (uint8_t)(apex_x - i);
        canvas_draw_line(canvas, col, (uint8_t)(cy - half), col, (uint8_t)(cy + half));
    }
}

#define SUBGHZ_READ_FREQ_CY       9
#define SUBGHZ_READ_FREQ_BASELINE 16
static void
    subghz_view_receiver_draw_read_freq(Canvas* canvas, const char* freq_str, bool hopping) {
    uint8_t x = 2;

    if(!hopping) {
        subghz_view_draw_solid_arrow(canvas, x, SUBGHZ_READ_FREQ_CY, 5, 3, true);
        x += 9;
    }

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, x, SUBGHZ_READ_FREQ_BASELINE, freq_str);
    x = (uint8_t)(x + canvas_string_width(canvas, freq_str) + 3);

    if(!hopping) {
        subghz_view_draw_solid_arrow(canvas, (uint8_t)(x + 5), SUBGHZ_READ_FREQ_CY, 5, 3, false);
    }
}

static void subghz_view_receiver_draw_read_gps(Canvas* canvas, SubGhzViewReceiverModel* model) {
    if(!model->gps_active) return;
    canvas_set_font(canvas, FontSecondary);
    char buf[8];
    subghz_view_gps_label(buf, sizeof(buf), model);
    const uint8_t badge_w = (uint8_t)(8 + canvas_string_width(canvas, buf));
    subghz_view_gps_badge_draw(canvas, buf, (uint8_t)(107 - badge_w), 9);
}

void subghz_view_receiver_add_data_progress(
    SubGhzViewReceiver* subghz_receiver,
    const char* progress_str) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { furi_string_set(model->progress_str, progress_str); },
        true);
}

void subghz_view_receiver_set_radio_device_type(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzRadioDeviceType device_type) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->device_type = device_type; },
        true);
}

void subghz_view_receiver_set_dual(
    SubGhzViewReceiver* subghz_receiver,
    bool dual_mode,
    const char* int_str,
    const char* ext_str) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            model->dual_mode = dual_mode;
            if(dual_mode) model->dual_dashboard = true;
            furi_string_set(model->dual_int_str, int_str ? int_str : "");
            furi_string_set(model->dual_ext_str, ext_str ? ext_str : "");
        },
        true);
}

void subghz_view_receiver_set_dual_dashboard(SubGhzViewReceiver* subghz_receiver, bool dashboard) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->dual_dashboard = dashboard; },
        true);
}

bool subghz_view_receiver_get_dual_dashboard(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);
    bool dashboard;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { dashboard = model->dual_dashboard; },
        false);
    return dashboard;
}

static void subghz_view_receiver_draw_wave_section(
    Canvas* canvas,
    const uint8_t* history,
    uint8_t history_index,
    uint8_t baseline,
    uint8_t region_height) {
    for(uint8_t col = 0; col < SUBGHZ_RX_RSSI_HISTORY_SIZE; col++) {
        uint8_t idx = (history_index + col) % SUBGHZ_RX_RSSI_HISTORY_SIZE;
        uint16_t bar = (uint16_t)history[idx] * region_height / SUBGHZ_RX_RSSI_WAVEFORM_HEIGHT;
        if(bar < 1) bar = 1;
        if(bar > region_height) bar = region_height;
        canvas_draw_line(canvas, col, baseline, col, (uint8_t)(baseline - bar + 1));
    }
}

static void subghz_view_receiver_draw_dual_pane(
    Canvas* canvas,
    const char* marker,
    const char* info,
    const uint8_t* history,
    uint8_t history_index,
    uint8_t top) {
    const uint8_t height = 31;
    const uint8_t bottom = top + height - 1;
    char label[48];
    snprintf(label, sizeof(label), "[%s] %s", marker, info);

    canvas_set_color(canvas, ColorBlack);
    subghz_view_receiver_draw_wave_section(
        canvas, history, history_index, bottom - 1, height - 14);
    canvas_draw_frame(canvas, 0, top, 128, height);
    canvas_draw_str(canvas, 4, top + 9, label);
}

static void
    subghz_view_receiver_draw_dual_dashboard(Canvas* canvas, SubGhzViewReceiverModel* model) {
    canvas_set_font(canvas, FontSecondary);
    subghz_view_receiver_draw_dual_pane(
        canvas,
        "I",
        furi_string_get_cstr(model->dual_int_str),
        model->rssi_history,
        model->rssi_history_index,
        0);
    subghz_view_receiver_draw_dual_pane(
        canvas,
        "E",
        furi_string_get_cstr(model->dual_ext_str),
        model->rssi_history_ext,
        model->rssi_history_ext_index,
        33);

    const char* count = furi_string_get_cstr(model->history_stat_str);
    if(count[0] != '\0') {
        uint8_t w = canvas_string_width(canvas, count);
        canvas_draw_str(canvas, 124 - w, 9, count);
    }
}

static void subghz_view_receiver_draw_frame(Canvas* canvas, uint16_t idx, bool scrollbar) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0 + idx * FRAME_HEIGHT, scrollbar ? 122 : 127, FRAME_HEIGHT);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_dot(canvas, 0, 0 + idx * FRAME_HEIGHT);
    canvas_draw_dot(canvas, 1, 0 + idx * FRAME_HEIGHT);
    canvas_draw_dot(canvas, 0, (0 + idx * FRAME_HEIGHT) + 1);

    canvas_draw_dot(canvas, 0, (0 + idx * FRAME_HEIGHT) + 11);
    canvas_draw_dot(canvas, scrollbar ? 121 : 126, 0 + idx * FRAME_HEIGHT);
    canvas_draw_dot(canvas, scrollbar ? 121 : 126, (0 + idx * FRAME_HEIGHT) + 11);
}

static void subghz_view_rssi_draw(Canvas* canvas, SubGhzViewReceiverModel* model) {
    for(uint8_t i = 1; i < model->u_rssi; i++) {
        if(i % 5) {
            canvas_draw_dot(canvas, 46 + i, 50);
            canvas_draw_dot(canvas, 47 + i, 51);
            canvas_draw_dot(canvas, 46 + i, 52);
        }
    }
}

static void subghz_view_rssi_waveform_draw(Canvas* canvas, SubGhzViewReceiverModel* model) {
    const uint8_t max_height = SUBGHZ_RX_RSSI_WAVEFORM_HEIGHT;
    for(uint8_t col = 0; col < SUBGHZ_RX_RSSI_HISTORY_SIZE; col++) {
        uint8_t idx = (model->rssi_history_index + col) % SUBGHZ_RX_RSSI_HISTORY_SIZE;
        uint8_t bar_h = model->rssi_history[idx];
        if(bar_h < 1) bar_h = 1;
        if(bar_h > max_height) bar_h = max_height;
        canvas_draw_line(canvas, col, max_height, col, (uint8_t)(max_height - bar_h + 1));
    }
}

void subghz_view_receiver_draw(Canvas* canvas, SubGhzViewReceiverModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    if(model->dual_mode && model->dual_dashboard) {
        subghz_view_receiver_draw_dual_dashboard(canvas, model);
        return;
    }

    if(model->mode == SubGhzViewReceiverModeLive) {
        if(model->history_item == 0) {
            elements_button_center(canvas, "Config");
        } else {
            elements_button_left(canvas, "Config");
        }
    } else {
        canvas_draw_line(canvas, 2, 52, 125, 52);
        canvas_draw_str(canvas, 3, 62, furi_string_get_cstr(model->progress_str));
    }

    bool scrollbar = model->history_item > 4;
    FuriString* str_buff = furi_string_alloc();

    if(!model->nodraw) {
        SubGhzReceiverMenuItem* item_menu;

        for(size_t i = 0; i < MIN(model->history_item, MENU_ITEMS); ++i) {
            size_t idx = CLAMP((uint16_t)(i + model->list_offset), model->history_item, 0);
            item_menu = SubGhzReceiverMenuItemArray_get(model->history->data, idx);
            if(item_menu == NULL) {
                break;
            }
            if(item_menu->type == 0) {
                break;
            }
            furi_string_set(str_buff, item_menu->item_str);
            if(model->idx == idx) {
                subghz_view_receiver_draw_frame(canvas, i, scrollbar);
                if(model->show_time) {
                    // Show time of signal one moment
                    furi_string_set(str_buff, item_menu->time);
                }
            } else {
                canvas_set_color(canvas, ColorBlack);
            }
            elements_string_fit_width(canvas, str_buff, scrollbar ? MAX_LEN_PX - 7 : MAX_LEN_PX);
            const Icon* item_icon = ReceiverItemIcons[item_menu->type];
            if(item_menu->button_icon == SubGhzHistoryButtonIconLock) {
                item_icon = &I_Lock_7x8;
            } else if(item_menu->button_icon == SubGhzHistoryButtonIconUnlock) {
                item_icon = &I_Unlock_7x8;
            }
            canvas_draw_icon(canvas, 4, 2 + i * FRAME_HEIGHT, item_icon);
            canvas_draw_str(canvas, 15, 9 + i * FRAME_HEIGHT, furi_string_get_cstr(str_buff));
            furi_string_reset(str_buff);
        }
        if(scrollbar) {
            elements_scrollbar_pos(canvas, 128, 0, 49, model->idx, model->history_item);
        }
    }
    furi_string_free(str_buff);

    canvas_set_color(canvas, ColorBlack);

    if(model->history_item == 0) {
        if(model->mode == SubGhzViewReceiverModeLive) {
            subghz_view_receiver_draw_wave_section(
                canvas, model->rssi_history, model->rssi_history_index, 49, 27);
            canvas_draw_line(canvas, 0, 19, 127, 19);

            subghz_view_receiver_draw_read_freq(
                canvas, furi_string_get_cstr(model->frequency_str), model->hopping_enabled);
            subghz_view_receiver_draw_read_gps(canvas, model);

            if(model->bin_raw_enabled) {
                canvas_draw_icon(canvas, 116, 10, &I_Cos_9x7);
            }
        } else {
            subghz_view_rssi_waveform_draw(canvas, model);
            canvas_draw_line(canvas, 0, 34, 127, 34);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignBottom, "Decoding...");
            canvas_set_font(canvas, FontSecondary);
        }
    }

    if(model->mode == SubGhzViewReceiverModeLive && model->history_item > 0 && !model->dual_mode) {
        subghz_view_rssi_draw(canvas, model);
    }
    switch(model->bar_show) {
    case SubGhzViewReceiverBarShowLock:
        canvas_draw_icon(canvas, 64, 55, &I_Lock_7x8);
        canvas_draw_str(canvas, 74, 62, "Locked");
        break;
    case SubGhzViewReceiverBarShowToUnlockPress:
        canvas_draw_str(canvas, 44, 62, furi_string_get_cstr(model->frequency_str));
#ifdef SUBGHZ_EXT_PRESET_NAME
        if(model->history_item == 0 && model->mode == SubGhzViewReceiverModeLive) {
            canvas_draw_str(
                canvas,
                44 + canvas_string_width(canvas, furi_string_get_cstr(model->frequency_str)) + 1,
                62,
                "MHz");
            subghz_view_receiver_draw_live_preset(canvas, model);
        } else {
            canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
        }
#else
        canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
#endif
        canvas_draw_str(canvas, 96, 62, furi_string_get_cstr(model->history_stat_str));
        canvas_set_font(canvas, FontSecondary);
        elements_bold_rounded_frame(canvas, 14, 8, 99, 48);
        elements_multiline_text(canvas, 65, 26, "To unlock\npress:");
        canvas_draw_icon(canvas, 65, 42, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 80, 42, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 95, 42, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 16, 13, &I_WarningDolphin_45x42);
        canvas_draw_dot(canvas, 17, 61);
        break;
    case SubGhzViewReceiverBarShowUnlock:
        canvas_draw_icon(canvas, 64, 55, &I_Unlock_7x8);
        canvas_draw_str(canvas, 74, 62, "Unlocked");
        break;
    default: {
        if(model->dual_mode) {
            if(model->history_item > 0) {
                canvas_draw_str(canvas, 44, 62, furi_string_get_cstr(model->frequency_str));
                canvas_draw_str(canvas, 96, 62, furi_string_get_cstr(model->history_stat_str));
            }
            break;
        }
        const char* frequency_str = furi_string_get_cstr(model->frequency_str);
        bool big_freq_shown =
            (model->history_item == 0 && model->mode == SubGhzViewReceiverModeLive);
        if(!big_freq_shown) {
            canvas_draw_str(canvas, 44, 62, frequency_str);
        }
#ifdef SUBGHZ_EXT_PRESET_NAME
        if(model->history_item == 0 && model->mode == SubGhzViewReceiverModeLive) {
            if(!big_freq_shown) {
                canvas_draw_str(
                    canvas, 44 + canvas_string_width(canvas, frequency_str) + 1, 62, "MHz");
            }
            subghz_view_receiver_draw_live_preset(canvas, model);
        } else {
            canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
        }
#else
        canvas_draw_str(canvas, 79, 62, furi_string_get_cstr(model->preset_str));
#endif
        if(!big_freq_shown && model->mode == SubGhzViewReceiverModeLive && model->gps_active) {
            const uint8_t config_x = subghz_view_config_button_center_x(canvas);
            const uint8_t config_w = subghz_view_config_button_center_width(canvas);
            const uint8_t config_right = (uint8_t)(config_x + config_w);
            const uint8_t count_x = 96;
            char sats_buf[8];
            subghz_view_gps_label(sats_buf, sizeof(sats_buf), model);
            canvas_set_font(canvas, FontSecondary);
            const uint8_t badge_w = (uint8_t)(8 + canvas_string_width(canvas, sats_buf));
            const uint8_t x0 =
                (uint8_t)(config_right + ((count_x > config_right + badge_w) ?
                                              (count_x - config_right - badge_w) / 2 :
                                              0));
            subghz_view_gps_badge_draw(canvas, sats_buf, x0, 62);
        }
        canvas_draw_str(canvas, 96, 62, furi_string_get_cstr(model->history_stat_str));
    } break;
    }
}

static void subghz_view_receiver_timer_callback(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { model->bar_show = SubGhzViewReceiverBarShowDefault; },
        true);
    if(subghz_receiver->lock_count < UNLOCK_CNT) {
        subghz_receiver->callback(
            SubGhzCustomEventViewReceiverOffDisplay, subghz_receiver->context);
    }
    subghz_receiver->lock_count = 0;
}

bool subghz_view_receiver_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;

    if(subghz_receiver->lock == true) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            { model->bar_show = SubGhzViewReceiverBarShowToUnlockPress; },
            true);
        if(subghz_receiver->lock_count == 0) {
            furi_timer_start(subghz_receiver->timer, 1000);
        }
        if(event->key == InputKeyBack && event->type == InputTypeShort) {
            subghz_receiver->lock_count++;
        }
        if(subghz_receiver->lock_count >= UNLOCK_CNT) {
            subghz_receiver->callback(
                SubGhzCustomEventViewReceiverUnlock, subghz_receiver->context);
            with_view_model(
                subghz_receiver->view,
                SubGhzViewReceiverModel * model,
                { model->bar_show = SubGhzViewReceiverBarShowUnlock; },
                true);
            subghz_receiver->lock = false;
            furi_timer_start(subghz_receiver->timer, 650);
        }

        return true;
    }

    const uint32_t now = furi_get_tick();

    bool dual_dashboard = false;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        { dual_dashboard = model->dual_mode && model->dual_dashboard; },
        false);
    if(dual_dashboard) {
        bool consumed = false;
        if(event->type == InputTypeShort) {
            switch(event->key) {
            case InputKeyBack:
                subghz_receiver->callback(
                    SubGhzCustomEventViewReceiverBack, subghz_receiver->context);
                consumed = true;
                break;
            case InputKeyOk:
                subghz_receiver->callback(
                    SubGhzCustomEventViewReceiverConfig, subghz_receiver->context);
                consumed = true;
                break;
            default:
                break;
            }
        }
        return consumed;
    }

    bool consumed = false;
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        subghz_receiver->callback(SubGhzCustomEventViewReceiverBack, subghz_receiver->context);
        consumed = true;
    } else if(
        event->key == InputKeyUp &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->history_item == 0) {
                    if(model->mode == SubGhzViewReceiverModeLive &&
                       event->type == InputTypeShort) {
                        subghz_receiver->callback(
                            SubGhzCustomEventViewReceiverNextPreset, subghz_receiver->context);
                    }
                } else {
                    if(model->idx != 0) model->idx--;
                    subghz_view_receiver_show_time_moment(context);
                }
            },
            true);
        consumed = true;
    } else if(
        event->key == InputKeyDown &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->history_item == 0) {
                    if(model->mode == SubGhzViewReceiverModeLive &&
                       event->type == InputTypeShort) {
                        subghz_receiver->callback(
                            SubGhzCustomEventViewReceiverPrevPreset, subghz_receiver->context);
                    }
                } else if(model->idx != model->history_item - 1) {
                    model->idx++;
                    subghz_view_receiver_show_time_moment(context);
                }
            },
            true);
        consumed = true;
    } else if(
        event->key == InputKeyLeft &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat ||
         event->type == InputTypeLong)) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->mode == SubGhzViewReceiverModeLive && model->history_item == 0) {
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverPrevFrequency, subghz_receiver->context);
                } else if(model->history_item != 0 && event->type == InputTypeShort) {
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverConfig, subghz_receiver->context);
                }
            },
            false);
        consumed = true;
    } else if(event->key == InputKeyRight && event->type == InputTypeRelease) {
        subghz_receiver->delete_freq_guard = false;
        consumed = true;
    } else if(
        event->key == InputKeyRight &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat ||
         event->type == InputTypeLong)) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->mode == SubGhzViewReceiverModeLive && model->history_item == 0) {
                    bool blocked = (event->type != InputTypeShort) &&
                                   (subghz_receiver->delete_freq_guard ||
                                    now < subghz_receiver->delete_freq_guard_until);
                    if(!blocked) {
                        subghz_receiver->callback(
                            SubGhzCustomEventViewReceiverNextFrequency, subghz_receiver->context);
                    }
                } else if(model->history_item != 0 && event->type == InputTypeLong) {
                    subghz_receiver->delete_freq_guard = true;
                    subghz_receiver->delete_freq_guard_until =
                        now + furi_ms_to_ticks(SUBGHZ_RX_DELETE_FREQ_GUARD_MS);
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverDeleteItem, subghz_receiver->context);
                }
            },
            false);
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        with_view_model(
            subghz_receiver->view,
            SubGhzViewReceiverModel * model,
            {
                if(model->history_item == 0 && model->mode == SubGhzViewReceiverModeLive) {
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverConfig, subghz_receiver->context);
                } else if(model->history_item != 0) {
                    subghz_receiver->callback(
                        SubGhzCustomEventViewReceiverOK, subghz_receiver->context);
                }
            },
            false);
        consumed = true;
    }

    if(consumed) {
        subghz_view_receiver_update_offset(subghz_receiver);
    }
    return consumed;
}

void subghz_view_receiver_enter(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    subghz_receiver->delete_freq_guard = false;
    subghz_receiver->delete_freq_guard_until = 0;
    subghz_view_receiver_show_time_moment(context);
}

void subghz_view_receiver_exit(void* context) {
    furi_assert(context);
    SubGhzViewReceiver* subghz_receiver = context;
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            furi_string_reset(model->frequency_str);
            furi_string_reset(model->preset_str);
            furi_string_reset(model->history_stat_str);

                for
                    M_EACH(item_menu, model->history->data, SubGhzReceiverMenuItemArray_t) {
                        furi_string_free(item_menu->item_str);
                        furi_string_free(item_menu->time);
                        item_menu->type = 0;
                    }
                SubGhzReceiverMenuItemArray_reset(model->history->data);
                model->idx = 0;
                model->list_offset = 0;
                model->history_item = 0;
                model->nodraw = false;
                model->hopping_enabled = false;
                model->bin_raw_enabled = false;
                model->dual_mode = false;
                model->dual_dashboard = true;
                furi_string_reset(model->dual_int_str);
                furi_string_reset(model->dual_ext_str);
                model->rssi_history_index = 0;
                model->rssi_history_ext_index = 0;
                memset(model->rssi_history, 0, sizeof(model->rssi_history));
                memset(model->rssi_history_ext, 0, sizeof(model->rssi_history_ext));
        },
        false);
    furi_timer_stop(subghz_receiver->timer);
    furi_timer_stop(subghz_receiver->flip_time_timer);
    furi_timer_stop(subghz_receiver->hop_ui_timer);
    subghz_receiver->hop_ui_frequency = 0;
    subghz_receiver->delete_freq_guard = false;
    subghz_receiver->delete_freq_guard_until = 0;
}

SubGhzViewReceiver* subghz_view_receiver_alloc(void) {
    SubGhzViewReceiver* subghz_receiver = malloc(sizeof(SubGhzViewReceiver));

    // View allocation and configuration
    subghz_receiver->view = view_alloc();

    subghz_receiver->lock = false;
    subghz_receiver->lock_count = 0;
    subghz_receiver->delete_freq_guard = false;
    subghz_receiver->delete_freq_guard_until = 0;
    subghz_receiver->frequency_callback = NULL;
    subghz_receiver->hop_ui_frequency = 0;
    view_allocate_model(
        subghz_receiver->view, ViewModelTypeLocking, sizeof(SubGhzViewReceiverModel));
    view_set_context(subghz_receiver->view, subghz_receiver);
    view_set_draw_callback(subghz_receiver->view, (ViewDrawCallback)subghz_view_receiver_draw);
    view_set_input_callback(subghz_receiver->view, subghz_view_receiver_input);
    view_set_enter_callback(subghz_receiver->view, subghz_view_receiver_enter);
    view_set_exit_callback(subghz_receiver->view, subghz_view_receiver_exit);

    subghz_receiver->flip_time_timer = furi_timer_alloc(
        subghz_view_receiver_flip_string_callback, FuriTimerTypeOnce, subghz_receiver);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            model->frequency_str = furi_string_alloc();
            model->preset_str = furi_string_alloc();
            model->history_stat_str = furi_string_alloc();
            model->progress_str = furi_string_alloc();
            model->dual_int_str = furi_string_alloc();
            model->dual_ext_str = furi_string_alloc();
            model->dual_mode = false;
            model->dual_dashboard = true;
            model->bar_show = SubGhzViewReceiverBarShowDefault;
            model->nodraw = false;
            model->history = malloc(sizeof(SubGhzReceiverHistory));
            model->hopping_enabled = false;
            model->bin_raw_enabled = false;
            model->rssi_history_index = 0;
            model->rssi_history_ext_index = 0;
            SubGhzReceiverMenuItemArray_init(model->history->data);
            memset(model->rssi_history, 0, sizeof(model->rssi_history));
            memset(model->rssi_history_ext, 0, sizeof(model->rssi_history_ext));
        },
        true);
    subghz_receiver->timer =
        furi_timer_alloc(subghz_view_receiver_timer_callback, FuriTimerTypeOnce, subghz_receiver);
    subghz_receiver->hop_ui_timer = furi_timer_alloc(
        subghz_view_receiver_hop_ui_timer_callback, FuriTimerTypePeriodic, subghz_receiver);
    return subghz_receiver;
}

void subghz_view_receiver_free(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    furi_timer_stop(subghz_receiver->hop_ui_timer);
    furi_timer_free(subghz_receiver->flip_time_timer);
    furi_timer_free(subghz_receiver->hop_ui_timer);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            furi_string_free(model->frequency_str);
            furi_string_free(model->preset_str);
            furi_string_free(model->history_stat_str);
            furi_string_free(model->progress_str);
            furi_string_free(model->dual_int_str);
            furi_string_free(model->dual_ext_str);
                for
                    M_EACH(item_menu, model->history->data, SubGhzReceiverMenuItemArray_t) {
                        furi_string_free(item_menu->item_str);
                        furi_string_free(item_menu->time);
                        item_menu->type = 0;
                    }
                SubGhzReceiverMenuItemArray_clear(model->history->data);
                free(model->history);
        },
        false);
    furi_timer_free(subghz_receiver->timer);
    view_free(subghz_receiver->view);
    free(subghz_receiver);
}

View* subghz_view_receiver_get_view(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);
    return subghz_receiver->view;
}

uint16_t subghz_view_receiver_get_idx_menu(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);
    uint16_t idx;
    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { idx = model->idx; }, false);
    return idx;
}

void subghz_view_receiver_delete_item(SubGhzViewReceiver* subghz_receiver, uint16_t idx) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            if(idx < SubGhzReceiverMenuItemArray_size(model->history->data)) {
                SubGhzReceiverMenuItem* item =
                    SubGhzReceiverMenuItemArray_get(model->history->data, idx);
                furi_string_free(item->item_str);
                furi_string_free(item->time);
                item->type = 0;
                SubGhzReceiverMenuItemArray_remove_v(model->history->data, idx, idx + 1);

                if(model->history_item == 5) {
                    if(model->idx >= 2) {
                        model->idx = model->history_item - 1;
                    }
                }
                model->history_item--;

                if(model->idx && (model->idx > idx || model->idx == model->history_item)) {
                    model->idx--;
                }
            }
        },
        true);
}

void subghz_view_receiver_clear_menu(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
                for
                    M_EACH(item_menu, model->history->data, SubGhzReceiverMenuItemArray_t) {
                        furi_string_free(item_menu->item_str);
                        furi_string_free(item_menu->time);
                        item_menu->type = 0;
                    }
                SubGhzReceiverMenuItemArray_reset(model->history->data);
                model->idx = 0;
                model->list_offset = 0;
                model->history_item = 0;
        },
        true);
}

void subghz_view_receiver_enable_draw_callback(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->nodraw = false; }, true);
}

void subghz_view_receiver_disable_draw_callback(SubGhzViewReceiver* subghz_receiver) {
    furi_assert(subghz_receiver);

    with_view_model(
        subghz_receiver->view, SubGhzViewReceiverModel * model, { model->nodraw = true; }, true);
}

void subghz_view_receiver_set_idx_menu(SubGhzViewReceiver* subghz_receiver, uint16_t idx) {
    furi_assert(subghz_receiver);
    with_view_model(
        subghz_receiver->view,
        SubGhzViewReceiverModel * model,
        {
            model->idx = idx;
            if(model->idx > 2) model->list_offset = idx - 2;
        },
        true);
    subghz_view_receiver_update_offset(subghz_receiver);
}
