#include "../subghz_i.h"
#include "subghz_settings_user_i.h"

enum VarItemListIndex {
    VarItemListIndexUseDefaults,
    VarItemListIndexStaticFreqs,
    VarItemListIndexHopperFreqs,
    VarItemListIndexCustomPresets,
    VarItemListIndexReset,
};

static void subghz_scene_settings_user_enter_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

static void subghz_scene_settings_user_use_defaults_changed(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    subghz_setting_set_add_standard_frequencies(subghz_txrx_get_setting(subghz->txrx), value);
    subghz_settings_user_persist(subghz);
}

void subghz_scene_settings_user_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItemList* var_item_list = subghz->variable_item_list;
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    VariableItem* item;

    bool use_defaults = subghz_setting_get_add_standard_frequencies(setting);
    item = variable_item_list_add(
        var_item_list, "Use Defaults", 2, subghz_scene_settings_user_use_defaults_changed, subghz);
    variable_item_set_current_value_index(item, use_defaults);
    variable_item_set_current_value_text(item, use_defaults ? "ON" : "OFF");

    item = variable_item_list_add(var_item_list, "Static Freqs", 0, NULL, subghz);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Hopper Freqs", 0, NULL, subghz);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Custom Presets", 0, NULL, subghz);
    variable_item_set_current_value_text(item, ">");

    variable_item_list_add(var_item_list, "Reset All to Defaults", 0, NULL, subghz);

    variable_item_list_set_enter_callback(
        var_item_list, subghz_scene_settings_user_enter_callback, subghz);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneSettingsUser));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_settings_user_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneSettingsUser, event.event);
        switch(event.event) {
        case VarItemListIndexStaticFreqs:
            scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneSettingsUserFreq, 0);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSettingsUserFreq);
            return true;
        case VarItemListIndexHopperFreqs:
            scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneSettingsUserHopper, 0);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSettingsUserHopper);
            return true;
        case VarItemListIndexCustomPresets:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSettingsUserPresets);
            return true;
        case VarItemListIndexReset:
            if(subghz_settings_user_confirm(
                   subghz,
                   "Reset to defaults?",
                   "Frequencies, hopper and\ncustom presets will be\nrestored to firmware defaults.")) {
                subghz_setting_reset_to_default(subghz_txrx_get_setting(subghz->txrx));
                subghz_settings_user_persist(subghz);
                notification_message(subghz->notifications, &sequence_success);
            }
            variable_item_list_reset(subghz->variable_item_list);
            subghz_scene_settings_user_on_enter(subghz);
            return true;
        default:
            break;
        }
        return true;
    }
    return false;
}

void subghz_scene_settings_user_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_reset(subghz->variable_item_list);
}
