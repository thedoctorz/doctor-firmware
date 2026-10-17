#include "../subghz_i.h"
#include "subghz_settings_user_i.h"

static void subghz_scene_settings_user_presets_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_settings_user_presets_on_enter(void* context) {
    SubGhz* subghz = context;
    Submenu* submenu = subghz->submenu;
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);

    submenu_set_header(submenu, "Custom Presets");

    size_t builtin = subghz_setting_get_builtin_preset_count(setting);
    size_t total = subghz_setting_get_preset_count(setting);

    if(total <= builtin) {
        submenu_add_item(
            submenu, "(no user presets)", 0, subghz_scene_settings_user_presets_callback, subghz);
    } else {
        for(size_t i = builtin; i < total; i++) {
            submenu_add_item(
                submenu,
                subghz_setting_get_preset_name(setting, i),
                i,
                subghz_scene_settings_user_presets_callback,
                subghz);
        }
    }

    submenu_set_selected_item(
        submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneSettingsUserPresets));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_settings_user_presets_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
        if(subghz_setting_get_preset_count(setting) <=
           subghz_setting_get_builtin_preset_count(setting)) {
            // "(no user presets)" placeholder; nothing to edit.
            return true;
        }
        subghz->settings_user_idx = (uint16_t)event.event;
        scene_manager_set_scene_state(
            subghz->scene_manager, SubGhzSceneSettingsUserPresets, event.event);
        scene_manager_set_scene_state(
            subghz->scene_manager, SubGhzSceneSettingsUserItem, SubGhzSettingsUserCtxPreset);
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSettingsUserItem);
        return true;
    }
    return false;
}

void subghz_scene_settings_user_presets_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
