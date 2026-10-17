#include "../subghz_i.h"
#include "subghz_settings_user_i.h"

enum VarItemListIndex {
    VarItemListIndexStaticFrequency,
    VarItemListIndexRemoveStaticFreq,
    VarItemListIndexAddStaticFreq,
};

static void subghz_scene_settings_user_freq_enter_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

static void subghz_scene_settings_user_freq_frequency_changed(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    size_t count = subghz_setting_get_user_frequency_count(setting);
    if(!count) return;

    subghz->settings_user_static_index = variable_item_get_current_value_index(item);
    uint32_t value =
        subghz_setting_get_user_frequency(setting, subghz->settings_user_static_index);
    char text[10] = {0};
    subghz_settings_user_format_freq_buf(text, sizeof(text), value);
    variable_item_set_current_value_text(item, text);
}

void subghz_scene_settings_user_freq_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItemList* var_item_list = subghz->variable_item_list;
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    VariableItem* item;

    size_t count = subghz_setting_get_user_frequency_count(setting);
    item = variable_item_list_add(
        var_item_list,
        "Static Freq",
        subghz_settings_user_values_count(count),
        subghz_scene_settings_user_freq_frequency_changed,
        subghz);
    subghz->settings_user_static_index = 0;
    variable_item_set_current_value_index(item, subghz->settings_user_static_index);
    if(count) {
        uint32_t value =
            subghz_setting_get_user_frequency(setting, subghz->settings_user_static_index);
        char text[10] = {0};
        subghz_settings_user_format_freq_buf(text, sizeof(text), value);
        variable_item_set_current_value_text(item, text);
    } else {
        variable_item_set_current_value_text(item, "None");
    }

    variable_item_list_add(var_item_list, "Remove Static Freq", 0, NULL, subghz);
    variable_item_list_add(var_item_list, "Add Static Freq", 0, NULL, subghz);

    variable_item_list_set_enter_callback(
        var_item_list, subghz_scene_settings_user_freq_enter_callback, subghz);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneSettingsUserFreq));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_settings_user_freq_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(
            subghz->scene_manager, SubGhzSceneSettingsUserFreq, event.event);
        consumed = true;
        SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
        switch(event.event) {
        case VarItemListIndexRemoveStaticFreq: {
            size_t count = subghz_setting_get_user_frequency_count(setting);
            if(!count) break;
            uint32_t value =
                subghz_setting_get_user_frequency(setting, subghz->settings_user_static_index);
            size_t removed = subghz_setting_delete_user_frequency_value(setting, value);
            if(removed) subghz_settings_user_persist(subghz);
            VariableItem* item = variable_item_list_get(
                subghz->variable_item_list, VarItemListIndexStaticFrequency);
            count = subghz_setting_get_user_frequency_count(setting);
            variable_item_set_values_count(item, subghz_settings_user_values_count(count));
            if(count) {
                subghz->settings_user_static_index -=
                    MIN(removed, subghz->settings_user_static_index);
                value =
                    subghz_setting_get_user_frequency(setting, subghz->settings_user_static_index);
                char text[10] = {0};
                subghz_settings_user_format_freq_buf(text, sizeof(text), value);
                variable_item_set_current_value_text(item, text);
            } else {
                subghz->settings_user_static_index = 0;
                variable_item_set_current_value_text(item, "None");
            }
            variable_item_set_current_value_index(item, subghz->settings_user_static_index);
            break;
        }
        case VarItemListIndexAddStaticFreq:
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneSettingsUserAdd, false);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSettingsUserAdd);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void subghz_scene_settings_user_freq_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_reset(subghz->variable_item_list);
}
