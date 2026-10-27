#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include <lib/toolbox/value_index.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/drivers/subghz/si4463_ext/si4463_ext_interconnect.h>
#include <string.h>

#define RADIO_DEVICE_COUNT 2
const char* const radio_device_text[RADIO_DEVICE_COUNT] = {
    "Internal",
    "External",
};

const uint32_t radio_device_value[RADIO_DEVICE_COUNT] = {
    SubGhzRadioDeviceTypeInternal,
    SubGhzRadioDeviceTypeExternalCC1101,
};

#define ON_OFF_COUNT 2
const char* const on_off_text[ON_OFF_COUNT] = {
    "OFF",
    "ON",
};

#define DEBUG_P_COUNT 2
const char* const debug_pin_text[DEBUG_P_COUNT] = {
    "OFF",
    "17(1W)",
};

#define DEBUG_COUNTER_COUNT 17
const char* const debug_counter_text[DEBUG_COUNTER_COUNT] = {
    "+1",
    "+2",
    "+3",
    "+4",
    "+5",
    "+10",
    "+50",
    "OVFL",
    "OFEX",
    "No",
    "-1",
    "-2",
    "-3",
    "-4",
    "-5",
    "-10",
    "-50",
};
const int32_t debug_counter_val[DEBUG_COUNTER_COUNT] = {
    1,
    2,
    3,
    4,
    5,
    10,
    50,
    65535,
    -2147483647,
    0,
    -1,
    -2,
    -3,
    -4,
    -5,
    -10,
    -50,
};

//TX Power
#define TX_POWER_COUNT 9
const char* const tx_power_text[TX_POWER_COUNT] = {
    "Preset",
    "10dBm +",
    "7dBm",
    "5dBm",
    "0dBm",
    "-10dBm",
    "-15dBm",
    "-20dBm",
    "-30dBm",
};

static const char* subghz_scene_radio_settings_ext_text(const char* name) {
    if(name && strcmp(name, SUBGHZ_DEVICE_SI4463_EXT_NAME) == 0) return "Ext Si4463";
    if(name && strcmp(name, SUBGHZ_DEVICE_CC1101_EXT_NAME) == 0) return "Ext CC1101";
    return "External";
}

static void subghz_scene_radio_settings_set_device(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    SubGhzRadioDeviceType applied =
        subghz_txrx_radio_device_set(subghz->txrx, radio_device_value[index]);
    if(applied != radio_device_value[index]) {
        index = 0;
        variable_item_set_current_value_index(item, index);
    }
    subghz->last_settings->preset_index =
        subghz_txrx_preset_make_valid(subghz->txrx, subghz->tx_power);
    const uint32_t frequency = subghz_txrx_get_frequency_hz(subghz->txrx);
    const uint32_t valid_frequency = subghz_txrx_frequency_make_valid(subghz->txrx, frequency);
    if(valid_frequency != frequency) {
        subghz_txrx_set_frequency(subghz->txrx, valid_frequency);
        subghz->last_settings->frequency = valid_frequency;
        subghz_setting_set_default_frequency(
            subghz_txrx_get_setting(subghz->txrx), valid_frequency);
    }
    if(applied == SubGhzRadioDeviceTypeExternalCC1101) {
        variable_item_set_current_value_text(
            item,
            subghz_scene_radio_settings_ext_text(subghz_txrx_radio_device_get_name(subghz->txrx)));
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRadioConnected);
    } else {
        variable_item_set_current_value_text(item, radio_device_text[index]);
    }
}

static void subghz_scene_radio_settings_set_tx_power(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    //Update the Menu Item on screen
    variable_item_set_current_value_text(item, tx_power_text[index]);

    //Set TX power and remember setting
    subghz->last_settings->tx_power = subghz->tx_power = index;

    //Save the settings now, this is the convention here!
    subghz_last_settings_save(subghz->last_settings);
}

static void subghz_scene_receiver_config_set_debug_pin(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, debug_pin_text[index]);

    subghz_txrx_set_debug_pin_state(subghz->txrx, index == 1);
}

static void subghz_scene_reciever_config_set_ext_amp_leds_control(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, on_off_text[index]);
    subghz->last_settings->leds_and_amp = index == 1;
    // Set globally in furi hal
    furi_hal_subghz_set_ext_leds_and_amp(subghz->last_settings->leds_and_amp);
    subghz_last_settings_save(subghz->last_settings);
    // reinit external device
    const SubGhzRadioDeviceType current = subghz_txrx_radio_device_get(subghz->txrx);
    if(current != SubGhzRadioDeviceTypeInternal) {
        subghz_txrx_radio_device_set(subghz->txrx, SubGhzRadioDeviceTypeInternal);
        subghz_txrx_radio_device_set(subghz->txrx, current);
    }
}

static void subghz_scene_receiver_config_set_debug_counter(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, debug_counter_text[index]);
    furi_hal_subghz_set_rolling_counter_mult(debug_counter_val[index]);
}

static void subghz_scene_receiver_config_set_timestamp_file_names(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, on_off_text[index]);

    subghz->last_settings->protocol_file_names = (index == 1);
    subghz_last_settings_save(subghz->last_settings);
}

static void subghz_scene_radio_settings_set_protocol_folders(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, on_off_text[index]);

    subghz->last_settings->protocol_folders = (index == 1);
    subghz_last_settings_save(subghz->last_settings);
}

void subghz_scene_radio_settings_on_enter(void* context) {
    SubGhz* subghz = context;

    VariableItemList* variable_item_list = subghz->variable_item_list;
    int32_t value_index;
    VariableItem* item;

    const char* ext_name = subghz_txrx_radio_device_detect_external(subghz->txrx);
    uint8_t value_count_device = ext_name ? RADIO_DEVICE_COUNT : 1;
    item = variable_item_list_add(
        subghz->variable_item_list,
        "Module",
        value_count_device,
        subghz_scene_radio_settings_set_device,
        subghz);
    value_index = value_index_uint32(
        subghz_txrx_radio_device_get(subghz->txrx), radio_device_value, value_count_device);
    variable_item_set_current_value_index(item, value_index);
    if(value_index == 1) {
        variable_item_set_current_value_text(item, subghz_scene_radio_settings_ext_text(ext_name));
    } else {
        variable_item_set_current_value_text(item, radio_device_text[value_index]);
    }

    //Add TX Power
    item = variable_item_list_add(
        subghz->variable_item_list,
        "TX Power",
        TX_POWER_COUNT,
        subghz_scene_radio_settings_set_tx_power,
        subghz);

    value_index = subghz->tx_power;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, tx_power_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Protocol Names",
        ON_OFF_COUNT,
        subghz_scene_receiver_config_set_timestamp_file_names,
        subghz);
    value_index = subghz->last_settings->protocol_file_names;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, on_off_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Protocol Folders",
        ON_OFF_COUNT,
        subghz_scene_radio_settings_set_protocol_folders,
        subghz);
    value_index = subghz->last_settings->protocol_folders;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, on_off_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Counter Incr.",
        furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) ? DEBUG_COUNTER_COUNT : 3,
        subghz_scene_receiver_config_set_debug_counter,
        subghz);
    value_index = value_index_int32(
        furi_hal_subghz_get_rolling_counter_mult(),
        debug_counter_val,
        furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) ? DEBUG_COUNTER_COUNT : 3);
    furi_hal_subghz_set_rolling_counter_mult(debug_counter_val[value_index]);

    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, debug_counter_text[value_index]);

    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
        item = variable_item_list_add(
            variable_item_list,
            "Ext Amp & LEDs",
            ON_OFF_COUNT,
            subghz_scene_reciever_config_set_ext_amp_leds_control,
            subghz);
        value_index = subghz->last_settings->leds_and_amp ? 1 : 0;
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, on_off_text[value_index]);

        item = variable_item_list_add(
            variable_item_list,
            "Debug Pin",
            DEBUG_P_COUNT,
            subghz_scene_receiver_config_set_debug_pin,
            subghz);
        value_index = subghz_txrx_get_debug_pin_state(subghz->txrx);
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, debug_pin_text[value_index]);
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_radio_settings_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventSceneRadioConnected) {
        subghz_ext_radio_notice(subghz);
        return true;
    }
    return false;
}

void subghz_scene_radio_settings_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_set_selected_item(subghz->variable_item_list, 0);
    variable_item_list_reset(subghz->variable_item_list);
}
