#include <furi.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <furi_hal_rtc.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt_api.h>
#include <notification/notification_messages.h>
#include <notification/notification_messages_notes.h>

#include "../desktop_i.h"
#include "desktop_view_lock_menu.h"

#define DESKTOP_LOCK_MENU_TOGGLE_COUNT 4

static const NotificationSequence sequence_note_c = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

typedef enum {
    DesktopLockMenuIndexLefthandedMode,
    DesktopLockMenuIndexSettings,
    DesktopLockMenuIndexBluetooth,
    DesktopLockMenuIndexLock,
    DesktopLockMenuIndexBrightness,
    DesktopLockMenuIndexVolume,

    DesktopLockMenuIndexTotalCount
} DesktopLockMenuIndex;

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context) {
    furi_assert(lock_menu);
    furi_assert(callback);
    lock_menu->callback = callback;
    lock_menu->context = context;
}

void desktop_lock_menu_set_stealth_mode_state(DesktopLockMenuView* lock_menu, bool stealth_mode) {
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        { model->stealth_mode = stealth_mode; },
        true);
}

void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx) {
    furi_assert(idx < DesktopLockMenuIndexTotalCount);
    with_view_model(
        lock_menu->view, DesktopLockMenuViewModel * model, { model->idx = idx; }, true);
}

static void desktop_lock_menu_apply_brightness(DesktopLockMenuView* lock_menu) {
    notification_message(lock_menu->notification, &sequence_display_backlight_force_on);
    rgb_backlight_update(
        lock_menu->notification->settings.display_brightness *
        lock_menu->notification->current_night_shift);
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontBatteryPercent);

    int8_t x, y, w, h;
    bool selected, toggle;
    bool enabled = false;
    uint8_t value = 0;
    int8_t total = 58;
    const Icon* icon = NULL;
    for(size_t i = 0; i < DesktopLockMenuIndexTotalCount; ++i) {
        selected = m->idx == i;
        toggle = i < DESKTOP_LOCK_MENU_TOGGLE_COUNT;
        if(toggle) {
            x = 2 + 32 * (i / 2);
            y = 2 + 32 * (i % 2);
            w = 28;
            h = 28;
            enabled = false;
        } else {
            x = 98 + 16 * (i % 2);
            y = 2;
            w = 12;
            h = 60;
            value = 0;
        }

        switch(i) {
        case DesktopLockMenuIndexLefthandedMode:
            icon = &I_CC_LefthandedMode_16x16;
            enabled = furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient);
            break;
        case DesktopLockMenuIndexSettings:
            icon = &I_CC_Settings_16x16;
            break;
        case DesktopLockMenuIndexBluetooth:
            icon = &I_CC_Bluetooth_16x16;
            enabled = m->lock_menu->bt->bt_settings.enabled;
            break;
        case DesktopLockMenuIndexLock:
            icon = &I_CC_Lock_16x16;
            break;
        case DesktopLockMenuIndexBrightness:
            icon = &I_Pin_star_7x7;
            value = total - m->lock_menu->notification->settings.display_brightness * total;
            break;
        case DesktopLockMenuIndexVolume:
            icon = m->stealth_mode ? &I_Muted_8x8 : &I_Volup_8x6;
            value = total - m->lock_menu->notification->settings.speaker_volume * total;
            break;
        default:
            break;
        }

        if(selected) {
            elements_bold_rounded_frame(canvas, x - 1, y - 1, w + 1, h + 1);
        } else {
            canvas_draw_rframe(canvas, x, y, w, h, 5);
        }

        if(toggle) {
            if(enabled) {
                canvas_draw_rbox(canvas, x, y, w, h, 5);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            if(enabled) {
                canvas_set_color(canvas, ColorBlack);
            }
        } else {
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            canvas_set_color(canvas, ColorXOR);
            canvas_draw_box(canvas, x + 1, y + 1 + value, w - 2, h - 2 - value);
            if(selected) {
                canvas_set_color(canvas, ColorBlack);
            } else {
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_dot(canvas, x + 1, y + 1);
            canvas_draw_dot(canvas, x + 1, y + h - 2);
            canvas_draw_dot(canvas, x + w - 2, y + 1);
            canvas_draw_dot(canvas, x + w - 2, y + h - 2);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rframe(canvas, x, y, w, h, 5);
        }
    }
}

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu) {
    furi_assert(lock_menu);
    return lock_menu->view;
}

bool desktop_lock_menu_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    DesktopLockMenuView* lock_menu = context;
    uint8_t idx = 0;
    bool stealth_mode = false;
    bool consumed = true;

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            stealth_mode = model->stealth_mode;
            if((event->type == InputTypeShort) || (event->type == InputTypeRepeat)) {
                if(model->idx < DESKTOP_LOCK_MENU_TOGGLE_COUNT) {
                    if(event->key == InputKeyUp || event->key == InputKeyDown) {
                        if(model->idx % 2) {
                            model->idx--;
                        } else {
                            model->idx++;
                        }
                    } else if(event->key == InputKeyLeft) {
                        if(model->idx < 2) {
                            model->idx = DesktopLockMenuIndexVolume;
                        } else {
                            model->idx -= 2;
                        }
                    } else if(event->key == InputKeyRight) {
                        if(model->idx >= 2) {
                            model->idx = DesktopLockMenuIndexBrightness;
                        } else {
                            model->idx += 2;
                        }
                    }
                } else {
                    if(event->key == InputKeyLeft) {
                        model->idx--;
                    } else if(event->key == InputKeyRight) {
                        if(model->idx >= DesktopLockMenuIndexVolume) {
                            model->idx = DesktopLockMenuIndexSettings;
                        } else {
                            model->idx++;
                        }
                    }
                }
            }
            idx = model->idx;
        },
        true);

    DesktopEvent desktop_event = 0;
    if(event->key == InputKeyBack) {
        consumed = false;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        switch(idx) {
        case DesktopLockMenuIndexLefthandedMode:
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient)) {
                furi_hal_rtc_reset_flag(FuriHalRtcFlagHandOrient);
            } else {
                furi_hal_rtc_set_flag(FuriHalRtcFlagHandOrient);
            }
            break;
        case DesktopLockMenuIndexSettings:
            desktop_event = DesktopLockMenuEventSettings;
            break;
        case DesktopLockMenuIndexBluetooth:
            lock_menu->bt->bt_settings.enabled = !lock_menu->bt->bt_settings.enabled;
            if(lock_menu->bt->bt_settings.enabled) {
                furi_hal_bt_start_advertising();
            } else {
                furi_hal_bt_stop_advertising();
            }
            lock_menu->save_bt = true;
            break;
        case DesktopLockMenuIndexLock:
            desktop_event = DesktopLockMenuEventLockKeypad;
            break;
        case DesktopLockMenuIndexBrightness:
            desktop_event = DesktopLockMenuEventScreenSettings;
            break;
        case DesktopLockMenuIndexVolume:
            desktop_event = stealth_mode ? DesktopLockMenuEventStealthModeOff :
                                           DesktopLockMenuEventStealthModeOn;
            break;
        default:
            break;
        }
    } else if(
        idx >= DesktopLockMenuIndexBrightness &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        int8_t offset = 0;
        if(event->key == InputKeyUp) {
            offset = 1;
        } else if(event->key == InputKeyDown) {
            offset = -1;
        }
        if(offset) {
            float value;
            switch(idx) {
            case DesktopLockMenuIndexBrightness:
                value = lock_menu->notification->settings.display_brightness + 0.05f * offset;
                lock_menu->notification->settings.display_brightness =
                    value < 0.00f ? 0.00f : (value > 1.00f ? 1.00f : value);
                lock_menu->save_notification = true;
                desktop_lock_menu_apply_brightness(lock_menu);
                break;
            case DesktopLockMenuIndexVolume:
                value = lock_menu->notification->settings.speaker_volume + 0.05f * offset;
                lock_menu->notification->settings.speaker_volume =
                    value < 0.00f ? 0.00f : (value > 1.00f ? 1.00f : value);
                lock_menu->save_notification = true;
                notification_message(lock_menu->notification, &sequence_note_c);
                break;
            default:
                break;
            }
        }
    }
    if(desktop_event) {
        lock_menu->callback(desktop_event, lock_menu->context);
    }

    return consumed;
}

DesktopLockMenuView* desktop_lock_menu_alloc(void) {
    DesktopLockMenuView* lock_menu = malloc(sizeof(DesktopLockMenuView));
    lock_menu->bt = furi_record_open(RECORD_BT);
    lock_menu->notification = furi_record_open(RECORD_NOTIFICATION);
    lock_menu->view = view_alloc();
    view_allocate_model(lock_menu->view, ViewModelTypeLocking, sizeof(DesktopLockMenuViewModel));
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        { model->lock_menu = lock_menu; },
        false);
    view_set_context(lock_menu->view, lock_menu);
    view_set_draw_callback(lock_menu->view, (ViewDrawCallback)desktop_lock_menu_draw_callback);
    view_set_input_callback(lock_menu->view, desktop_lock_menu_input_callback);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_assert(lock_menu_view);

    view_free(lock_menu_view->view);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_BT);
    free(lock_menu_view);
}
