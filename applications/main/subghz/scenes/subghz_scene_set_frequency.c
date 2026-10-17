#include "../subghz_i.h"

#define TAG "SubGhzSceneSetFrequency"

enum {
    SubGhzSetFrequencyResultEntered,
};

static bool subghz_scene_set_frequency_update_file(const char* path, uint32_t frequency_hz) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool updated = false;

    if(flipper_format_file_open_existing(file, path)) {
        updated = flipper_format_update_uint32(file, "Frequency", &frequency_hz, 1);
        flipper_format_file_close(file);
    }

    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return updated;
}

static bool subghz_scene_set_frequency_is_allowed(SubGhz* subghz, uint32_t frequency_hz) {
    return subghz_txrx_radio_device_is_frequency_valid(subghz->txrx, frequency_hz) &&
           furi_hal_subghz_is_tx_allowed(frequency_hz);
}

static bool subghz_scene_set_frequency_apply(SubGhz* subghz, uint32_t frequency_hz) {
    const char* path = furi_string_get_cstr(subghz->file_path);
    const uint32_t previous_hz = subghz_txrx_get_frequency_hz(subghz->txrx);

    if(!subghz_scene_set_frequency_update_file(path, frequency_hz)) {
        FURI_LOG_E(TAG, "Failed to update Frequency in %s", path);
        return false;
    }

    if(subghz_key_load(subghz, path, false)) {
        return true;
    }

    FURI_LOG_E(TAG, "Reload failed, restoring %lu Hz", (unsigned long)previous_hz);
    if(!subghz_scene_set_frequency_update_file(path, previous_hz) ||
       !subghz_key_load(subghz, path, false)) {
        FURI_LOG_E(TAG, "Failed to restore previous frequency");
    }
    return false;
}

static void subghz_scene_set_frequency_number_input_callback(void* context, int32_t number) {
    SubGhz* subghz = context;
    scene_manager_set_scene_state(
        subghz->scene_manager, SubGhzSceneSetFrequency, (uint32_t)number);
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzSetFrequencyResultEntered);
}

static void subghz_scene_set_frequency_popup_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdNumberInput);
}

void subghz_scene_set_frequency_on_enter(void* context) {
    SubGhz* subghz = context;
    NumberInput* number_input = subghz->number_input;
    const int32_t current_khz = (int32_t)(subghz_txrx_get_frequency_hz(subghz->txrx) / 1000UL);

    number_input_set_header_text(number_input, "Use kHz values, like 433920");
    number_input_set_result_callback(
        number_input,
        subghz_scene_set_frequency_number_input_callback,
        subghz,
        current_khz,
        100000,
        999999);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdNumberInput);
}

bool subghz_scene_set_frequency_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case SubGhzSetFrequencyResultEntered: {
            const uint32_t frequency_hz =
                scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneSetFrequency) *
                1000UL;

            if(!subghz_path_is_file(subghz->file_path)) {
                dialog_message_show_storage_error(subghz->dialogs, "Cannot update\nfrequency");
                scene_manager_previous_scene(subghz->scene_manager);
            } else if(!subghz_scene_set_frequency_is_allowed(subghz, frequency_hz)) {
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
                popup_set_callback(subghz->popup, subghz_scene_set_frequency_popup_callback);
                popup_set_context(subghz->popup, subghz);
                popup_set_timeout(subghz->popup, 1000);
                popup_enable_timeout(subghz->popup);
                view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);
            } else if(subghz_scene_set_frequency_apply(subghz, frequency_hz)) {
                scene_manager_previous_scene(subghz->scene_manager);
            } else {
                dialog_message_show_storage_error(subghz->dialogs, "Cannot update\nfrequency");
                scene_manager_previous_scene(subghz->scene_manager);
            }
            break;
        }
        default:
            break;
        }
    }

    return consumed;
}

void subghz_scene_set_frequency_on_exit(void* context) {
    SubGhz* subghz = context;
    number_input_set_result_callback(subghz->number_input, NULL, NULL, 0, 0, 0);
    number_input_set_header_text(subghz->number_input, "");
    popup_reset(subghz->popup);
}
