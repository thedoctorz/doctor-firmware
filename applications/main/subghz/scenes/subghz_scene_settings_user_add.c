#include "../subghz_i.h"
#include "subghz_settings_user_i.h"

enum NumberInputResult {
    NumberInputResultOk,
    NumberInputResultError,
};

static void subghz_scene_settings_user_add_number_input_callback(void* context, int32_t number) {
    SubGhz* subghz = context;

    uint32_t value = (uint32_t)number * 1000UL;
    if(!furi_hal_subghz_is_frequency_valid(value)) {
        view_dispatcher_send_custom_event(subghz->view_dispatcher, NumberInputResultError);
        return;
    }

    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    bool is_hopper =
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneSettingsUserAdd);
    bool added = is_hopper ? subghz_setting_add_user_hopper_frequency(setting, value) :
                             subghz_setting_add_user_frequency(setting, value);
    if(!added) {
        view_dispatcher_send_custom_event(subghz->view_dispatcher, NumberInputResultError);
        return;
    }
    subghz_settings_user_persist(subghz);
    view_dispatcher_send_custom_event(subghz->view_dispatcher, NumberInputResultOk);
}

void subghz_scene_settings_user_add_on_enter(void* context) {
    SubGhz* subghz = context;
    NumberInput* number_input = subghz->number_input;

    number_input_set_header_text(number_input, "Use kHz values, like 433920");

    number_input_set_result_callback(
        number_input,
        subghz_scene_settings_user_add_number_input_callback,
        subghz,
        0,
        100000,
        999999);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdNumberInput);
}

static void subghz_scene_settings_user_add_popup_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdNumberInput);
}

bool subghz_scene_settings_user_add_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case NumberInputResultOk:
            scene_manager_previous_scene(subghz->scene_manager);
            break;
        case NumberInputResultError:
            popup_reset(subghz->popup);
            popup_set_header(
                subghz->popup, "Invalid frequency!", 64, 18, AlignCenter, AlignCenter);
            popup_set_text(
                subghz->popup,
                "Must be 281-361,\n"
                "378-481, 749-962 MHz",
                64,
                40,
                AlignCenter,
                AlignCenter);
            popup_set_callback(subghz->popup, subghz_scene_settings_user_add_popup_callback);
            popup_set_context(subghz->popup, subghz);
            popup_set_timeout(subghz->popup, 1000);
            popup_enable_timeout(subghz->popup);
            view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void subghz_scene_settings_user_add_on_exit(void* context) {
    SubGhz* subghz = context;
    number_input_set_result_callback(subghz->number_input, NULL, NULL, 0, 0, 0);
    number_input_set_header_text(subghz->number_input, "");
    popup_reset(subghz->popup);
}
