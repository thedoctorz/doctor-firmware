#include "../subghz_i.h"

#define TAG "SubGhzSceneReceiverDualConfig"

// Independent frequency + modulation for the internal and external radios.
enum SubGhzDualSettingIndex {
    SubGhzDualSettingIndexIntFrequency,
    SubGhzDualSettingIndexIntModulation,
    SubGhzDualSettingIndexExtFrequency,
    SubGhzDualSettingIndexExtModulation,
};

static uint8_t subghz_scene_receiver_dual_config_freq_index(SubGhz* subghz, uint32_t value) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    uint8_t index = subghz_setting_get_frequency_default_index(setting);
    for(size_t i = 0; i < subghz_setting_get_frequency_count(setting); i++) {
        if(value == subghz_setting_get_frequency(setting, i)) {
            index = i;
            break;
        }
    }
    return index;
}

static uint8_t subghz_scene_receiver_dual_config_preset_index(SubGhz* subghz, const char* name) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    for(size_t i = 0; i < subghz_setting_get_preset_count(setting); i++) {
        if(!strcmp(subghz_setting_get_preset_name(setting, i), name)) {
            return i;
        }
    }
    return 0;
}

static void
    subghz_scene_receiver_dual_config_set_freq_text(VariableItem* item, uint32_t frequency) {
    char text_buf[10] = {0};
    snprintf(
        text_buf,
        sizeof(text_buf),
        "%lu.%02lu",
        frequency / 1000000,
        (frequency % 1000000) / 10000);
    variable_item_set_current_value_text(item, text_buf);
}

static void subghz_scene_receiver_dual_config_set_int_freq(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    uint32_t frequency = subghz_setting_get_frequency(setting, index);

    SubGhzRadioPreset preset = subghz_txrx_dual_get_preset_int(subghz->txrx);
    FuriString* name = furi_string_alloc_set(preset.name);
    subghz_txrx_dual_set_preset_int(subghz->txrx, furi_string_get_cstr(name), frequency);
    furi_string_free(name);

    subghz_scene_receiver_dual_config_set_freq_text(item, frequency);
}

static void subghz_scene_receiver_dual_config_set_int_mod(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    const char* preset_name = subghz_setting_get_preset_name(setting, index);

    SubGhzRadioPreset preset = subghz_txrx_dual_get_preset_int(subghz->txrx);
    subghz_txrx_dual_set_preset_int(subghz->txrx, preset_name, preset.frequency);

    variable_item_set_current_value_text(item, preset_name);
}

static void subghz_scene_receiver_dual_config_set_ext_freq(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    uint32_t frequency = subghz_setting_get_frequency(setting, index);

    SubGhzRadioPreset preset = subghz_txrx_dual_get_preset_ext(subghz->txrx);
    FuriString* name = furi_string_alloc_set(preset.name);
    subghz_txrx_dual_set_preset_ext(subghz->txrx, furi_string_get_cstr(name), frequency);
    furi_string_free(name);

    subghz_scene_receiver_dual_config_set_freq_text(item, frequency);
}

static void subghz_scene_receiver_dual_config_set_ext_mod(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    const char* preset_name = subghz_setting_get_preset_name(setting, index);

    SubGhzRadioPreset preset = subghz_txrx_dual_get_preset_ext(subghz->txrx);
    subghz_txrx_dual_set_preset_ext(subghz->txrx, preset_name, preset.frequency);

    variable_item_set_current_value_text(item, preset_name);
}

void subghz_scene_receiver_dual_config_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItem* item;
    uint8_t value_index;
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);

    SubGhzRadioPreset preset_int = subghz_txrx_dual_get_preset_int(subghz->txrx);
    SubGhzRadioPreset preset_ext = subghz_txrx_dual_get_preset_ext(subghz->txrx);

    // Internal radio
    item = variable_item_list_add(
        subghz->variable_item_list,
        "Int Frequency",
        subghz_setting_get_frequency_count(setting),
        subghz_scene_receiver_dual_config_set_int_freq,
        subghz);
    value_index = subghz_scene_receiver_dual_config_freq_index(subghz, preset_int.frequency);
    variable_item_set_current_value_index(item, value_index);
    subghz_scene_receiver_dual_config_set_freq_text(
        item, subghz_setting_get_frequency(setting, value_index));

    item = variable_item_list_add(
        subghz->variable_item_list,
        "Int Modulation",
        subghz_setting_get_preset_count(setting),
        subghz_scene_receiver_dual_config_set_int_mod,
        subghz);
    value_index = subghz_scene_receiver_dual_config_preset_index(
        subghz, furi_string_get_cstr(preset_int.name));
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(
        item, subghz_setting_get_preset_name(setting, value_index));

    // External radio
    item = variable_item_list_add(
        subghz->variable_item_list,
        "Ext Frequency",
        subghz_setting_get_frequency_count(setting),
        subghz_scene_receiver_dual_config_set_ext_freq,
        subghz);
    value_index = subghz_scene_receiver_dual_config_freq_index(subghz, preset_ext.frequency);
    variable_item_set_current_value_index(item, value_index);
    subghz_scene_receiver_dual_config_set_freq_text(
        item, subghz_setting_get_frequency(setting, value_index));

    item = variable_item_list_add(
        subghz->variable_item_list,
        "Ext Modulation",
        subghz_setting_get_preset_count(setting),
        subghz_scene_receiver_dual_config_set_ext_mod,
        subghz);
    value_index = subghz_scene_receiver_dual_config_preset_index(
        subghz, furi_string_get_cstr(preset_ext.name));
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(
        item, subghz_setting_get_preset_name(setting, value_index));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_receiver_dual_config_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_receiver_dual_config_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_set_selected_item(subghz->variable_item_list, 0);
    variable_item_list_reset(subghz->variable_item_list);
}
