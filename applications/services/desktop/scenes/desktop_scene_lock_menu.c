#include <gui/scene_manager.h>
#include <applications.h>
#include <furi_hal.h>
#include <toolbox/saved_struct.h>
#include <stdbool.h>
#include <loader/loader.h>
#include <bt/bt_service/bt_api.h>

#include "../desktop_i.h"
#include <desktop/desktop_settings.h>
#include "../views/desktop_view_lock_menu.h"
#include "desktop_scene.h"

#define TAG "DesktopSceneLock"

static void desktop_scene_lock_menu_save_settings(Desktop* desktop) {
    if(desktop->lock_menu->save_notification) {
        notification_message_save_settings(desktop->lock_menu->notification);
        desktop->lock_menu->save_notification = false;
    }
    if(desktop->lock_menu->save_bt) {
        bt_set_settings(desktop->lock_menu->bt, &desktop->lock_menu->bt->bt_settings);
        desktop->lock_menu->save_bt = false;
    }
}

void desktop_scene_lock_menu_callback(DesktopEvent event, void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, event);
}

void desktop_scene_lock_menu_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    desktop_lock_menu_set_callback(desktop->lock_menu, desktop_scene_lock_menu_callback, desktop);
    desktop_lock_menu_set_stealth_mode_state(
        desktop->lock_menu, furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode));
    desktop_lock_menu_set_idx(desktop->lock_menu, 3);

    gui_set_hide_statusbar(desktop->gui, true);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
}

bool desktop_scene_lock_menu_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopLockMenuEventSettings:
            desktop_scene_lock_menu_save_settings(desktop);
            loader_show_settings_menu(desktop->loader);
            consumed = true;
            break;
        case DesktopLockMenuEventLockKeypad:
            desktop_scene_lock_menu_save_settings(desktop);
            desktop_lock(desktop);
            consumed = true;
            break;
        case DesktopLockMenuEventScreenSettings:
            desktop_scene_lock_menu_save_settings(desktop);
            loader_start_detached_with_gui_error(desktop->loader, "LCD and Notifications", NULL);
            consumed = true;
            break;
        case DesktopLockMenuEventStealthModeOn:
            desktop_set_stealth_mode_state(desktop, true);
            desktop_lock_menu_set_stealth_mode_state(desktop->lock_menu, true);
            consumed = true;
            break;
        case DesktopLockMenuEventStealthModeOff:
            desktop_set_stealth_mode_state(desktop, false);
            desktop_lock_menu_set_stealth_mode_state(desktop->lock_menu, false);
            consumed = true;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void desktop_scene_lock_menu_on_exit(void* context) {
    Desktop* desktop = (Desktop*)context;
    desktop_scene_lock_menu_save_settings(desktop);
    gui_set_hide_statusbar(desktop->gui, false);
}
