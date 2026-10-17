#include "../subghz_i.h"
#include "subghz_settings_user_i.h"
#include <string.h>

typedef enum {
    SubGhzSettingsUserItemRename,
    SubGhzSettingsUserItemDelete,
} SubGhzSettingsUserItemAction;

static void subghz_scene_settings_user_item_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_settings_user_item_on_enter(void* context) {
    SubGhz* subghz = context;
    Submenu* submenu = subghz->submenu;
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);

    submenu_set_header(
        submenu, subghz_setting_get_preset_name(setting, subghz->settings_user_idx));
    submenu_add_item(
        submenu,
        "Rename",
        SubGhzSettingsUserItemRename,
        subghz_scene_settings_user_item_callback,
        subghz);
    submenu_add_item(
        submenu,
        "Delete",
        SubGhzSettingsUserItemDelete,
        subghz_scene_settings_user_item_callback,
        subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_settings_user_item_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type != SceneManagerEventTypeCustom) return false;

    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);

    if(event.event == SubGhzSettingsUserItemRename) {
        strlcpy(
            subghz->settings_user_text,
            subghz_setting_get_preset_name(setting, subghz->settings_user_idx),
            sizeof(subghz->settings_user_text));
        scene_manager_set_scene_state(
            subghz->scene_manager, SubGhzSceneSettingsUserText, SubGhzSettingsUserCtxPreset);
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSettingsUserText);
        return true;
    }
    // Delete preset (confirm first)
    FuriString* text = furi_string_alloc();
    furi_string_printf(
        text,
        "Preset \"%s\"\nwill be deleted.",
        subghz_setting_get_preset_name(setting, subghz->settings_user_idx));
    bool ok = subghz_settings_user_confirm(subghz, "Delete preset?", furi_string_get_cstr(text));
    furi_string_free(text);
    if(!ok) {
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
        return true;
    }
    subghz_setting_delete_preset(setting, subghz->settings_user_idx);
    subghz_settings_user_clamp_preset_index(subghz);
    subghz_settings_user_persist(subghz);
    scene_manager_search_and_switch_to_previous_scene(
        subghz->scene_manager, SubGhzSceneSettingsUserPresets);
    return true;
}

void subghz_scene_settings_user_item_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
