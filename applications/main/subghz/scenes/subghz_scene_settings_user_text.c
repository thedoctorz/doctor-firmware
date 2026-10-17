#include "../subghz_i.h"
#include "subghz_settings_user_i.h"
#include <string.h>

#define SUBGHZ_SETTINGS_USER_TEXT_DONE 0x2000

static void subghz_scene_settings_user_text_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SUBGHZ_SETTINGS_USER_TEXT_DONE);
}

void subghz_scene_settings_user_text_on_enter(void* context) {
    SubGhz* subghz = context;
    TextInput* text_input = subghz->text_input;

    text_input_set_header_text(text_input, "Preset name");
    text_input_set_result_callback(
        text_input,
        subghz_scene_settings_user_text_callback,
        subghz,
        subghz->settings_user_text,
        sizeof(subghz->settings_user_text),
        false);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdTextInput);
}

bool subghz_scene_settings_user_text_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SUBGHZ_SETTINGS_USER_TEXT_DONE) {
        SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
        if(strlen(subghz->settings_user_text) > 0 &&
           subghz_setting_rename_preset(
               setting, subghz->settings_user_idx, subghz->settings_user_text)) {
            subghz_settings_user_persist(subghz);
            notification_message(subghz->notifications, &sequence_success);
        } else {
            notification_message(subghz->notifications, &sequence_error);
        }
        scene_manager_search_and_switch_to_previous_scene(
            subghz->scene_manager, SubGhzSceneSettingsUserPresets);
        return true;
    }
    return false;
}

void subghz_scene_settings_user_text_on_exit(void* context) {
    SubGhz* subghz = context;
    text_input_reset(subghz->text_input);
}
