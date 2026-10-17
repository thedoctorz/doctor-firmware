#include "transmitter.h"

#include <assets_icons.h>
#include <input/input.h>
#include <gui/elements.h>

#include <lib/subghz/blocks/custom_btn.h>

#include <stdio.h>
#include <string.h>

struct SubGhzViewTransmitter {
    View* view;
    SubGhzViewTransmitterCallback callback;
    void* context;
};

typedef struct {
    FuriString* frequency_str;
    FuriString* preset_str;
    FuriString* key_str;
    bool show_button;
    SubGhzRadioDeviceType device_type;
    FuriString* temp_button_id;
    bool draw_temp_button;
    uint8_t active_button_id;
} SubGhzViewTransmitterModel;

void subghz_view_transmitter_set_callback(
    SubGhzViewTransmitter* subghz_transmitter,
    SubGhzViewTransmitterCallback callback,
    void* context) {
    furi_assert(subghz_transmitter);

    subghz_transmitter->callback = callback;
    subghz_transmitter->context = context;
}

void subghz_view_transmitter_add_data_to_show(
    SubGhzViewTransmitter* subghz_transmitter,
    const char* key_str,
    const char* frequency_str,
    const char* preset_str,
    bool show_button) {
    furi_assert(subghz_transmitter);
    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            furi_string_set(model->key_str, key_str);
            furi_string_set(model->frequency_str, frequency_str);
            furi_string_set(model->preset_str, preset_str);
            model->show_button = show_button;
        },
        true);
}

void subghz_view_transmitter_set_radio_device_type(
    SubGhzViewTransmitter* subghz_transmitter,
    SubGhzRadioDeviceType device_type) {
    furi_assert(subghz_transmitter);
    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        { model->device_type = device_type; },
        true);
}

static void subghz_view_transmitter_button_right(Canvas* canvas, const char* str) {
    const uint8_t button_height = 12;
    const uint8_t vertical_offset = 3;
    const uint8_t horizontal_offset = 1;
    const uint8_t string_width = canvas_string_width(canvas, str);
    const Icon* icon = &I_ButtonCenter_7x7;
    const uint8_t icon_offset = 3;
    const uint8_t icon_width_with_offset = icon_get_width(icon) + icon_offset;
    const uint8_t button_width = string_width + horizontal_offset * 2 + icon_width_with_offset;

    const uint8_t x = (canvas_width(canvas) - button_width) / 2 + 40;
    const uint8_t y = canvas_height(canvas);

    canvas_draw_box(canvas, x, y - button_height, button_width, button_height);

    canvas_draw_line(canvas, x - 1, y, x - 1, y - button_height + 0);
    canvas_draw_line(canvas, x - 2, y, x - 2, y - button_height + 1);
    canvas_draw_line(canvas, x - 3, y, x - 3, y - button_height + 2);

    canvas_draw_line(canvas, x + button_width + 0, y, x + button_width + 0, y - button_height + 0);
    canvas_draw_line(canvas, x + button_width + 1, y, x + button_width + 1, y - button_height + 1);
    canvas_draw_line(canvas, x + button_width + 2, y, x + button_width + 2, y - button_height + 2);

    canvas_invert_color(canvas);
    canvas_draw_icon(
        canvas,
        x + horizontal_offset,
        y - button_height + vertical_offset - 1,
        &I_ButtonCenter_7x7);
    canvas_draw_str(
        canvas, x + horizontal_offset + icon_width_with_offset, y - vertical_offset, str);
    canvas_invert_color(canvas);
}

static void
    subghz_view_transmitter_copy_key_title(const char* key_str, char* title, size_t title_size) {
    furi_assert(title);
    furi_assert(title_size);

    title[0] = '\0';
    if(!key_str || !key_str[0]) {
        strlcpy(title, "SubGHz", title_size);
        return;
    }

    size_t pos = 0;
    while(key_str[pos] && key_str[pos] != '\r' && key_str[pos] != '\n' &&
          pos < (title_size - 1U)) {
        title[pos] = key_str[pos];
        pos++;
    }
    title[pos] = '\0';

    char* bit_suffix = strstr(title, "bit");
    if(bit_suffix && bit_suffix[3] == '\0') {
        char* trim = bit_suffix;
        while(trim > title && trim[-1] >= '0' && trim[-1] <= '9') {
            trim--;
        }
        while(trim > title && trim[-1] == ' ') {
            trim--;
        }
        if(trim > title) {
            *trim = '\0';
        }
    }
}

static bool subghz_view_transmitter_copy_key_field(
    const char* key_str,
    const char* field,
    char* value,
    size_t value_size) {
    furi_assert(field);
    furi_assert(value);
    furi_assert(value_size);

    value[0] = '\0';
    if(!key_str) return false;

    const char* start = strstr(key_str, field);
    if(!start) return false;
    start += strlen(field);
    if(start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
        start += 2;
    }

    size_t pos = 0;
    while(start[pos] && start[pos] != '\r' && start[pos] != '\n' && start[pos] != ' ' &&
          start[pos] != '[' && pos < (value_size - 1U)) {
        value[pos] = start[pos];
        pos++;
    }
    value[pos] = '\0';
    return pos > 0;
}

static bool subghz_view_transmitter_try_copy_fields(
    const char* key_str,
    const char* const* fields,
    size_t field_count,
    char* value,
    size_t value_size) {
    for(size_t i = 0; i < field_count; i++) {
        if(subghz_view_transmitter_copy_key_field(key_str, fields[i], value, value_size)) {
            return true;
        }
    }
    return false;
}

static bool
    subghz_view_transmitter_copy_sn_field(const char* key_str, char* value, size_t value_size) {
    static const char* const sn_fields[] = {
        "Sn:",
        "SN:",
        "UID:",
        "SnA:",
        "SnB:",
        "Fix:",
    };
    return subghz_view_transmitter_try_copy_fields(
        key_str, sn_fields, COUNT_OF(sn_fields), value, value_size);
}

static bool
    subghz_view_transmitter_copy_cnt_field(const char* key_str, char* value, size_t value_size) {
    static const char* const cnt_fields[] = {
        "Cnt:",
        "CNT:",
        "Ctrl:",
    };
    return subghz_view_transmitter_try_copy_fields(
        key_str, cnt_fields, COUNT_OF(cnt_fields), value, value_size);
}

static void subghz_view_transmitter_copy_pp_label(const char* label, char* out, size_t out_size) {
    furi_assert(out);
    furi_assert(out_size);

    if(!label || !label[0]) {
        strlcpy(out, "XXX", out_size);
        return;
    }

    if(!strcmp(label, "Trunk")) {
        label = "Boot";
    }

    size_t pos = 0;
    while(label[pos] && pos < (out_size - 1U)) {
        char ch = label[pos];
        out[pos] = (ch >= 'a' && ch <= 'z') ? (char)(ch - ('a' - 'A')) : ch;
        pos++;
    }
    out[pos] = '\0';
}

static void subghz_view_transmitter_draw_pp_button(
    Canvas* canvas,
    uint8_t btn_id,
    int32_t x,
    int32_t y_center,
    int32_t text_y,
    Align align) {
    SubGhzCustomButtonMapItem item;
    if(!subghz_custom_btn_map_get(btn_id, &item)) return;

    char label[SUBGHZ_CUSTOM_BTN_LABEL_SIZE];
    subghz_view_transmitter_copy_pp_label(item.label, label, sizeof(label));

    const uint16_t width_button = canvas_string_width(canvas, label) + 8U;
    const uint16_t height_button = canvas_current_font_height(canvas);
    int32_t box_x = x;
    if(align == AlignCenter) {
        box_x -= width_button / 2U;
    } else if(align == AlignRight) {
        box_x -= width_button;
    }

    canvas_draw_rbox(
        canvas, box_x, y_center - (height_button / 2U), width_button, height_button, 3);
    canvas_invert_color(canvas);
    canvas_draw_str_aligned(
        canvas, box_x + (width_button / 2U), text_y, AlignCenter, AlignBottom, label);
    canvas_invert_color(canvas);
}

static void
    subghz_view_transmitter_draw_button_map(Canvas* canvas, SubGhzViewTransmitterModel* model) {
    static uint8_t animation_frame = 0;
    animation_frame = (animation_frame + 1U) % 8U;

    const char* key_str = furi_string_get_cstr(model->key_str);
    char title[32];
    char field[16];
    char info_str[24];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_draw_box(canvas, 0, 0, 128, 11);
    canvas_invert_color(canvas);
    canvas_set_font(canvas, FontSecondary);
    subghz_view_transmitter_copy_key_title(key_str, title, sizeof(title));
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, title);
    canvas_invert_color(canvas);

    canvas_set_font(canvas, FontSecondary);

    if(subghz_view_transmitter_copy_sn_field(key_str, field, sizeof(field))) {
        snprintf(info_str, sizeof(info_str), "SN:%s", field);
    } else {
        strlcpy(info_str, "SN:------", sizeof(info_str));
    }
    canvas_draw_str(canvas, 2, 20, info_str);

    snprintf(info_str, sizeof(info_str), "F:%s", furi_string_get_cstr(model->frequency_str));
    canvas_draw_str(canvas, 2, 30, info_str);

    if(subghz_view_transmitter_copy_cnt_field(key_str, field, sizeof(field))) {
        snprintf(info_str, sizeof(info_str), "CNT:%s", field);
    } else {
        strlcpy(info_str, "CNT:----", sizeof(info_str));
    }
    canvas_draw_str(canvas, 68, 20, info_str);

    canvas_draw_str(canvas, 95, 30, furi_string_get_cstr(model->preset_str));

    subghz_view_transmitter_draw_pp_button(canvas, SUBGHZ_CUSTOM_BTN_OK, 64, 45, 49, AlignCenter);
    subghz_view_transmitter_draw_pp_button(canvas, SUBGHZ_CUSTOM_BTN_UP, 64, 33, 37, AlignCenter);
    subghz_view_transmitter_draw_pp_button(canvas, SUBGHZ_CUSTOM_BTN_LEFT, 0, 46, 50, AlignLeft);
    subghz_view_transmitter_draw_pp_button(
        canvas, SUBGHZ_CUSTOM_BTN_RIGHT, 127, 46, 50, AlignRight);
    subghz_view_transmitter_draw_pp_button(
        canvas, SUBGHZ_CUSTOM_BTN_DOWN, 64, 57, 61, AlignCenter);

    if(model->draw_temp_button) {
        canvas_draw_rbox(canvas, 24, 18, 80, 18, 3);
        canvas_invert_color(canvas);

        const uint8_t wave = animation_frame % 3U;
        canvas_draw_str(canvas, 28 + wave * 2U, 25, ")))");

        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, "TX");

        canvas_invert_color(canvas);
    }
}

void subghz_view_transmitter_draw(Canvas* canvas, SubGhzViewTransmitterModel* model) {
    if(model->show_button && subghz_custom_btn_map_is_available()) {
        subghz_view_transmitter_draw_button_map(canvas, model);
        return;
    }

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    elements_multiline_text_aligned(
        canvas, 0, 0, AlignLeft, AlignTop, furi_string_get_cstr(model->key_str));
    canvas_draw_str(canvas, 78, 7, furi_string_get_cstr(model->frequency_str));
    canvas_draw_str(canvas, 113, 7, furi_string_get_cstr(model->preset_str));

    if(model->draw_temp_button && !subghz_custom_btn_map_is_available()) {
        canvas_set_font(canvas, FontBatteryPercent);
        canvas_draw_str(canvas, 117, 40, furi_string_get_cstr(model->temp_button_id));
        canvas_set_font(canvas, FontSecondary);
    }

    if(model->show_button) {
        // TODO
        canvas_draw_str(
            canvas,
            58,
            62,
            (model->device_type == SubGhzRadioDeviceTypeInternal) ? "R: Int" : "R: Ext");
        subghz_view_transmitter_button_right(canvas, "Send");
    }
}

bool subghz_view_transmitter_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewTransmitter* subghz_transmitter = context;
    bool can_be_sent = false;

    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        // Reset view model
        with_view_model(
            subghz_transmitter->view,
            SubGhzViewTransmitterModel * model,
            {
                furi_string_reset(model->frequency_str);
                furi_string_reset(model->preset_str);
                furi_string_reset(model->key_str);
                furi_string_reset(model->temp_button_id);
                model->show_button = false;
                model->draw_temp_button = false;
                model->active_button_id = SUBGHZ_CUSTOM_BTN_OK;
            },
            false);
        return false;
    } // Finish "Back" key processing

    if(event->key == InputKeyBack) {
        return false;
    }

    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            if(model->show_button) {
                can_be_sent = true;
            }
        },
        true);

    if(can_be_sent) {
        if(event->key == InputKeyOk && event->type == InputTypePress) {
            subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_OK);
            with_view_model(
                subghz_transmitter->view,
                SubGhzViewTransmitterModel * model,
                {
                    furi_string_reset(model->temp_button_id);
                    model->active_button_id = SUBGHZ_CUSTOM_BTN_OK;
                    model->draw_temp_button = subghz_custom_btn_map_is_available();
                },
                true);
            subghz_transmitter->callback(
                SubGhzCustomEventViewTransmitterSendStart, subghz_transmitter->context);
            return true;
        } else if(event->key == InputKeyOk && event->type == InputTypeRelease) {
            with_view_model(
                subghz_transmitter->view,
                SubGhzViewTransmitterModel * model,
                {
                    furi_string_reset(model->temp_button_id);
                    model->draw_temp_button = false;
                    model->active_button_id = SUBGHZ_CUSTOM_BTN_OK;
                },
                true);
            subghz_transmitter->callback(
                SubGhzCustomEventViewTransmitterSendStop, subghz_transmitter->context);
            return true;
        } // Finish "OK" key processing

        if(subghz_custom_btn_is_allowed()) {
            uint8_t temp_btn_id;
            if(event->key == InputKeyUp) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_UP;
            } else if(event->key == InputKeyDown) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_DOWN;
            } else if(event->key == InputKeyLeft) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_LEFT;
            } else if(event->key == InputKeyRight) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_RIGHT;
            } else {
                // Finish processing if the button is different
                return true;
            }

            if(event->type == InputTypePress) {
                bool selected = false;
                SubGhzCustomButtonMapItem map_item;
                const bool have_map = subghz_custom_btn_map_get(temp_btn_id, &map_item);
                if(subghz_custom_btn_map_is_available() && !have_map) return true;

                with_view_model(
                    subghz_transmitter->view,
                    SubGhzViewTransmitterModel * model,
                    {
                        furi_string_reset(model->temp_button_id);
                        if(have_map || subghz_custom_btn_has_original()) {
                            selected = subghz_custom_btn_set(temp_btn_id);
                            if(selected) {
                                model->active_button_id = temp_btn_id;
                                if(have_map) {
                                    furi_string_set(model->temp_button_id, map_item.label);
                                } else {
                                    furi_string_printf(
                                        model->temp_button_id,
                                        "%01X",
                                        subghz_custom_btn_get_original());
                                }
                                model->draw_temp_button = true;
                            } else {
                                model->draw_temp_button = false;
                            }
                        }
                    },
                    true);
                if(selected || !subghz_custom_btn_map_is_available()) {
                    subghz_transmitter->callback(
                        SubGhzCustomEventViewTransmitterSendStart, subghz_transmitter->context);
                }
                return true;
            } else if(event->type == InputTypeRelease) {
                with_view_model(
                    subghz_transmitter->view,
                    SubGhzViewTransmitterModel * model,
                    {
                        furi_string_reset(model->temp_button_id);
                        model->draw_temp_button = false;
                        model->active_button_id = SUBGHZ_CUSTOM_BTN_OK;
                    },
                    true);
                subghz_transmitter->callback(
                    SubGhzCustomEventViewTransmitterSendStop, subghz_transmitter->context);
                return true;
            }
        }
    }

    return true;
}

void subghz_view_transmitter_enter(void* context) {
    furi_assert(context);
}

void subghz_view_transmitter_exit(void* context) {
    furi_assert(context);
}

SubGhzViewTransmitter* subghz_view_transmitter_alloc(void) {
    SubGhzViewTransmitter* subghz_transmitter = malloc(sizeof(SubGhzViewTransmitter));

    // View allocation and configuration
    subghz_transmitter->view = view_alloc();
    view_allocate_model(
        subghz_transmitter->view, ViewModelTypeLocking, sizeof(SubGhzViewTransmitterModel));
    view_set_context(subghz_transmitter->view, subghz_transmitter);
    view_set_draw_callback(
        subghz_transmitter->view, (ViewDrawCallback)subghz_view_transmitter_draw);
    view_set_input_callback(subghz_transmitter->view, subghz_view_transmitter_input);
    view_set_enter_callback(subghz_transmitter->view, subghz_view_transmitter_enter);
    view_set_exit_callback(subghz_transmitter->view, subghz_view_transmitter_exit);

    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            model->frequency_str = furi_string_alloc();
            model->preset_str = furi_string_alloc();
            model->key_str = furi_string_alloc();
            model->temp_button_id = furi_string_alloc();
            model->active_button_id = SUBGHZ_CUSTOM_BTN_OK;
        },
        true);
    return subghz_transmitter;
}

void subghz_view_transmitter_free(SubGhzViewTransmitter* subghz_transmitter) {
    furi_assert(subghz_transmitter);

    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            furi_string_free(model->frequency_str);
            furi_string_free(model->preset_str);
            furi_string_free(model->key_str);
            furi_string_free(model->temp_button_id);
        },
        true);
    view_free(subghz_transmitter->view);
    free(subghz_transmitter);
}

View* subghz_view_transmitter_get_view(SubGhzViewTransmitter* subghz_transmitter) {
    furi_assert(subghz_transmitter);
    return subghz_transmitter->view;
}
