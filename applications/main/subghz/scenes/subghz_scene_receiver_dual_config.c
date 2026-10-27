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

static size_t subghz_scene_receiver_dual_config_walk(
    SubGhz* subghz,
    bool si4463,
    uint8_t mode,
    size_t arg) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    size_t seen = 0;
    for(size_t i = 0; i < subghz_setting_get_preset_count(setting); i++) {
        if(!subghz_txrx_preset_fits_kind(subghz->txrx, i, si4463)) continue;
        if((mode == 0 && seen == arg) || (mode == 2 && i == arg)) return mode == 0 ? i : seen;
        seen++;
    }
    return mode == 1 ? seen : (size_t)-1;
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

static void subghz_scene_receiver_dual_config_set_mod(VariableItem* item, bool external) {
    SubGhz* subghz = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    const bool si4463 = external && subghz_txrx_dual_ext_is_si4463(subghz->txrx);
    const size_t preset_idx = subghz_scene_receiver_dual_config_walk(subghz, si4463, 0, index);
    if(preset_idx == (size_t)-1) return;

    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    const char* preset_name = subghz_setting_get_preset_name(setting, preset_idx);
    SubGhzRadioPreset preset = external ? subghz_txrx_dual_get_preset_ext(subghz->txrx) :
                                          subghz_txrx_dual_get_preset_int(subghz->txrx);
    if(external) {
        subghz_txrx_dual_set_preset_ext(subghz->txrx, preset_name, preset.frequency);
    } else {
        subghz_txrx_dual_set_preset_int(subghz->txrx, preset_name, preset.frequency);
    }
    variable_item_set_current_value_text(item, preset_name);
}

static void subghz_scene_receiver_dual_config_set_int_mod(VariableItem* item) {
    subghz_scene_receiver_dual_config_set_mod(item, false);
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
    subghz_scene_receiver_dual_config_set_mod(item, true);
}

static void subghz_scene_receiver_dual_config_add_mod(
    SubGhz* subghz,
    const char* label,
    bool si4463,
    const char* preset_name,
    VariableItemChangeCallback callback) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    size_t count = subghz_scene_receiver_dual_config_walk(subghz, si4463, 1, 0);
    if(count == 0) count = 1;
    if(count > 255) count = 255;

    VariableItem* item = variable_item_list_add(
        subghz->variable_item_list, label, (uint8_t)count, callback, subghz);
    const int absolute = subghz_setting_get_inx_preset_by_name(setting, preset_name);
    size_t pos = (size_t)-1;
    if(absolute >= 0) {
        pos = subghz_scene_receiver_dual_config_walk(subghz, si4463, 2, (size_t)absolute);
    }
    if(pos == (size_t)-1 || pos >= count) pos = 0;
    variable_item_set_current_value_index(item, (uint8_t)pos);

    const size_t shown = subghz_scene_receiver_dual_config_walk(subghz, si4463, 0, pos);
    variable_item_set_current_value_text(
        item,
        shown == (size_t)-1 ? preset_name : subghz_setting_get_preset_name(setting, shown));
}

void subghz_scene_receiver_dual_config_on_enter(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_dual_rx_stop(subghz->txrx);
    const bool ext_si4463 = subghz_txrx_dual_ext_is_si4463(subghz->txrx);
    subghz_txrx_dual_coerce_presets(subghz->txrx, ext_si4463);
    variable_item_list_reset(subghz->variable_item_list);
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

    subghz_scene_receiver_dual_config_add_mod(
        subghz,
        "Int Modulation",
        false,
        furi_string_get_cstr(preset_int.name),
        subghz_scene_receiver_dual_config_set_int_mod);

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

    subghz_scene_receiver_dual_config_add_mod(
        subghz,
        "Ext Modulation",
        ext_si4463,
        furi_string_get_cstr(preset_ext.name),
        subghz_scene_receiver_dual_config_set_ext_mod);

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
