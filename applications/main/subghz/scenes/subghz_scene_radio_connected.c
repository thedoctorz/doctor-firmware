#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/drivers/subghz/si4463_ext/si4463_ext_interconnect.h>
#include <string.h>

static void subghz_scene_radio_connected_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneRadioConnected);
}

static const char* subghz_scene_radio_connected_body(SubGhz* subghz) {
    const char* name = subghz_txrx_radio_device_get_name(subghz->txrx);
    if(name && strcmp(name, SUBGHZ_DEVICE_SI4463_EXT_NAME) == 0) {
        return "Ext Si4463\nconnected.";
    }
    if(name && strcmp(name, SUBGHZ_DEVICE_CC1101_EXT_NAME) == 0) {
        return "Ext CC1101\nconnected.";
    }
    return "External radio\nconnected.";
}

static void subghz_scene_radio_connected_close(SubGhz* subghz) {
    if(!scene_manager_previous_scene(subghz->scene_manager)) {
        scene_manager_stop(subghz->scene_manager);
        view_dispatcher_stop(subghz->view_dispatcher);
    }
}

void subghz_scene_radio_connected_on_enter(void* context) {
    SubGhz* subghz = context;
    Popup* popup = subghz->popup;

    popup_set_header(popup, "External module", 64, 3, AlignCenter, AlignTop);
    popup_set_text(popup, subghz_scene_radio_connected_body(subghz), 4, 22, AlignLeft, AlignTop);
    popup_set_icon(popup, 83, 22, &I_WarningDolphinFlip_45x42);
    popup_set_timeout(popup, 1500);
    popup_set_context(popup, subghz);
    popup_set_callback(popup, subghz_scene_radio_connected_callback);
    popup_enable_timeout(popup);
    notification_message(subghz->notifications, &sequence_display_backlight_on);
    notification_message(subghz->notifications, &sequence_set_green_255);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);
}

bool subghz_scene_radio_connected_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventSceneRadioConnected) {
        subghz_scene_radio_connected_close(subghz);
        return true;
    } else if(event.type == SceneManagerEventTypeBack) {
        subghz_scene_radio_connected_close(subghz);
        return true;
    }
    return false;
}

void subghz_scene_radio_connected_on_exit(void* context) {
    SubGhz* subghz = context;
    notification_message(subghz->notifications, &sequence_reset_rgb);
    popup_reset(subghz->popup);
}
