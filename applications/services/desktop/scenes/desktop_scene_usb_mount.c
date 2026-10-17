#include <furi.h>
#include <gui/scene_manager.h>
#include <loader/loader.h>

#include "desktop_scene.h"
#include "../desktop_i.h"

#define USB_MOUNT_PROMPT_TIMEOUT_MS 5000

static void desktop_scene_usb_mount_dialog_callback(DialogExResult result, void* context) {
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(
        desktop->view_dispatcher,
        (result == DialogExResultRight) ? DesktopUsbMountEventEnable : DesktopUsbMountEventCancel);
}

static void desktop_scene_usb_mount_timer_callback(void* context) {
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopUsbMountEventCancel);
}

void desktop_scene_usb_mount_on_enter(void* context) {
    Desktop* desktop = context;
    DialogEx* dialog = desktop->usb_mount_dialog;

    dialog_ex_set_context(dialog, desktop);
    dialog_ex_set_header(
        dialog, "USB Connected", 64, 8 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignCenter);
    dialog_ex_set_text(
        dialog,
        "Share the SD card as\nUSB mass storage?",
        64,
        24 + STATUS_BAR_Y_SHIFT,
        AlignCenter,
        AlignCenter);
    dialog_ex_set_left_button_text(dialog, "Cancel");
    dialog_ex_set_right_button_text(dialog, "Enable");
    dialog_ex_set_result_callback(dialog, desktop_scene_usb_mount_dialog_callback);

    FuriTimer* timer =
        furi_timer_alloc(desktop_scene_usb_mount_timer_callback, FuriTimerTypeOnce, desktop);
    scene_manager_set_scene_state(desktop->scene_manager, DesktopSceneUsbMount, (uint32_t)timer);
    furi_timer_start(timer, furi_ms_to_ticks(USB_MOUNT_PROMPT_TIMEOUT_MS));

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdUsbMount);
}

bool desktop_scene_usb_mount_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopUsbMountEventEnable:
            // Leave the prompt first so the desktop is back underneath the app,
            // then hand off to the tested USB Storage app (unmounts SD, exposes it,
            // remounts on exit).
            scene_manager_previous_scene(desktop->scene_manager);
            loader_start_detached_with_gui_error(
                desktop->loader, "/ext/apps/USB/usb_msc.fap", NULL);
            consumed = true;
            break;
        case DesktopUsbMountEventCancel:
        case DesktopUsbMountEventDisconnected:
            scene_manager_previous_scene(desktop->scene_manager);
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_usb_mount_on_exit(void* context) {
    Desktop* desktop = context;
    FuriTimer* timer =
        (FuriTimer*)scene_manager_get_scene_state(desktop->scene_manager, DesktopSceneUsbMount);
    if(timer) {
        furi_timer_stop(timer);
        furi_timer_free(timer);
        scene_manager_set_scene_state(desktop->scene_manager, DesktopSceneUsbMount, 0);
    }
    dialog_ex_reset(desktop->usb_mount_dialog);
}
