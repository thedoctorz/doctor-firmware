#include "../subghz_i.h"

#include <lib/subghz/blocks/custom_btn.h>
#include <lib/subghz/protocols/fiat_v1.h>
#include <lib/subghz/protocols/renault_v1.h>
#include <lib/subghz/protocols/psa.h>

#include "../helpers/subghz_psa_bf.h"
#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include <lib/subghz/blocks/generic.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>

#define TAG "SubGhzSceneReceiverInfo"

static bool
    subghz_scene_receiver_info_read_uint32(FlipperFormat* ff, const char* key, uint32_t* value) {
    flipper_format_rewind(ff);
    return flipper_format_read_uint32(ff, key, value, 1);
}

static bool
    subghz_scene_receiver_info_read_float(FlipperFormat* ff, const char* key, float* value) {
    flipper_format_rewind(ff);
    return flipper_format_read_float(ff, key, value, 1);
}

// Append a "GPS: <distance> <dir> (<sats> sat)" line describing where the stored
// signal was captured relative to the current live position, if both are known.
static void subghz_scene_receiver_info_append_gps(SubGhz* subghz, FuriString* text) {
    if(!subghz->gps) return;
    FlipperFormat* ff = subghz_get_current_signal_data(subghz);
    if(!ff) return;

    float stored_lat = 0;
    float stored_lon = 0;
    if(!subghz_scene_receiver_info_read_float(ff, "Lat", &stored_lat) ||
       !subghz_scene_receiver_info_read_float(ff, "Lon", &stored_lon)) {
        return;
    }

    float cur_lat = 0;
    float cur_lon = 0;
    if(!subghz_gps_get_location(subghz->gps, &cur_lat, &cur_lon)) return;
    SubGhzGpsSource source = subghz_gps_get_source(subghz->gps);

    float distance = subghz_gps_distance_km(cur_lat, cur_lon, stored_lat, stored_lon);
    const char* dir = subghz_gps_direction(cur_lat, cur_lon, stored_lat, stored_lon);

    char origin[16];
    if(source == SubGhzGpsSourceCompanion) {
        snprintf(origin, sizeof(origin), "phone");
    } else {
        snprintf(origin, sizeof(origin), "%u sat", subghz_gps_get_satellites(subghz->gps));
    }

    if(distance < 1.0f) {
        furi_string_cat_printf(
            text, "\r\nGPS: %.0fm %s (%s)", (double)(distance * 1000.0f), dir, origin);
    } else {
        furi_string_cat_printf(text, "\r\nGPS: %.2fkm %s (%s)", (double)distance, dir, origin);
    }
}

static void subghz_scene_receiver_info_configure_button_map(FlipperFormat* ff) {
    FuriString* protocol = furi_string_alloc();
    uint32_t type = 0;
    uint32_t rolling = 0;

    if(flipper_format_rewind(ff) && flipper_format_read_string(ff, "Protocol", protocol)) {
        subghz_scene_receiver_info_read_uint32(ff, "Type", &type);
        subghz_scene_receiver_info_read_uint32(ff, "Rolling", &rolling);
        subghz_custom_btn_configure_map(
            furi_string_get_cstr(protocol),
            type,
            rolling ? SUBGHZ_CUSTOM_BTN_PROTOCOL_ROLL : SUBGHZ_CUSTOM_BTN_PROTOCOL_NONE);
    } else {
        subghz_custom_btn_map_reset();
    }

    furi_string_free(protocol);
}

static bool subghz_scene_receiver_info_prepare_custom_buttons(SubGhz* subghz) {
    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(subghz_txrx_get_decoder(subghz->txrx), text);
    furi_string_free(text);

    subghz_scene_receiver_info_configure_button_map(subghz_get_current_signal_data(subghz));
    return subghz_custom_btn_is_allowed();
}

static void subghz_scene_receiver_info_copy_history_to_transmitter(SubGhz* subghz) {
    FlipperFormat* source = subghz_get_current_signal_data(subghz);
    FlipperFormat* dest = subghz_txrx_get_fff_data(subghz->txrx);
    Stream* source_stream = flipper_format_get_raw_stream(source);
    Stream* dest_stream = flipper_format_get_raw_stream(dest);

    stream_clean(dest_stream);
    stream_rewind(source_stream);
    stream_copy_full(source_stream, dest_stream);
    stream_rewind(source_stream);
    stream_rewind(dest_stream);
}

void subghz_scene_receiver_info_callback(GuiButtonType result, InputType type, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;

    if((result == GuiButtonTypeCenter) && (type == InputTypePress)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStart);
    } else if((result == GuiButtonTypeCenter) && (type == InputTypeRelease)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStop);
    } else if((result == GuiButtonTypeRight) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoSave);
    } else if((result == GuiButtonTypeLeft) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoBruteforceStart);
    }
}

static bool subghz_scene_receiver_info_update_parser(void* context) {
    SubGhz* subghz = context;
    const bool from_file = subghz_current_signal_is_from_file(subghz);

    FlipperFormat* ff = subghz_get_current_signal_data(subghz);
    if(!ff) return false;

    const char* protocol_name = NULL;
    FuriString* protocol_str = NULL;
    if(from_file) {
        protocol_str = furi_string_alloc();
        flipper_format_rewind(ff);
        if(!flipper_format_read_string(ff, "Protocol", protocol_str)) {
            furi_string_free(protocol_str);
            return false;
        }
        protocol_name = furi_string_get_cstr(protocol_str);
    } else {
        protocol_name = subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen);
    }

    bool ok = false;
    if(subghz_txrx_load_decoder_by_name_protocol(subghz->txrx, protocol_name)) {
        SubGhzProtocolStatus status =
            subghz_protocol_decoder_base_deserialize(subghz_txrx_get_decoder(subghz->txrx), ff);
        if(status != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Signal deserialize failed: %d", status);
        } else if(from_file) {
            SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
            subghz_txrx_set_tx_power(preset.data, preset.data_size, subghz->tx_power);
            ok = true;
        } else {
            SubGhzRadioPreset* preset =
                subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);

            //Edit TX power, if necessary.
            subghz_txrx_set_tx_power(preset->data, preset->data_size, subghz->tx_power);

            subghz_txrx_set_preset(
                subghz->txrx,
                furi_string_get_cstr(preset->name),
                preset->frequency,
                preset->data,
                preset->data_size);
            ok = true;
        }
    }

    if(protocol_str) furi_string_free(protocol_str);
    return ok;
}

void subghz_scene_receiver_info_draw_widget(SubGhz* subghz) {
    if(subghz_scene_receiver_info_update_parser(subghz)) {
        FuriString* frequency_str = furi_string_alloc();
        FuriString* modulation_str = furi_string_alloc();
        FuriString* text = furi_string_alloc();

        subghz_txrx_get_frequency_and_modulation(
            subghz->txrx, frequency_str, modulation_str, false);
        widget_add_string_element(
            subghz->widget,
            78,
            0,
            AlignLeft,
            AlignTop,
            FontSecondary,
            furi_string_get_cstr(frequency_str));

        widget_add_string_element(
            subghz->widget,
            113,
            0,
            AlignLeft,
            AlignTop,
            FontSecondary,
            furi_string_get_cstr(modulation_str));
        subghz_protocol_decoder_base_get_string(subghz_txrx_get_decoder(subghz->txrx), text);
        subghz_scene_receiver_info_append_gps(subghz, text);
        subghz_scene_receiver_info_configure_button_map(subghz_get_current_signal_data(subghz));
        widget_add_string_multiline_element(
            subghz->widget, 0, 0, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));

        furi_string_free(frequency_str);
        furi_string_free(modulation_str);
        furi_string_free(text);

        if(subghz_txrx_protocol_is_serializable(subghz->txrx)) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeRight,
                "Save",
                subghz_scene_receiver_info_callback,
                subghz);
        }
        // Removed static check
        bool show_send = subghz_txrx_protocol_is_transmittable(subghz->txrx, false);
        if(show_send) {
            FlipperFormat* ff = subghz_get_current_signal_data(subghz);
            const bool needs_hitag2 = ff && (fiat_v1_flipper_needs_hitag2_key(ff) ||
                                             renault_v1_flipper_needs_hitag2_key(ff));
            if(ff && (psa_bf_needs_bruteforce(ff, true) || hitag2_bf_needs_bruteforce(ff, true))) {
                show_send = false;
            }
            if(needs_hitag2) {
                show_send = true;
            }
        }
        if(show_send) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeCenter,
                "Send",
                subghz_scene_receiver_info_callback,
                subghz);
        }
        if(subghz_psa_bf_needs_bruteforce(subghz)) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeLeft,
                "BF",
                subghz_scene_receiver_info_callback,
                subghz);
        }
    } else {
        widget_add_icon_element(subghz->widget, 83, 22, &I_WarningDolphinFlip_45x42);
        widget_add_string_element(
            subghz->widget, 13, 8, AlignLeft, AlignBottom, FontSecondary, "Error history parse.");
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

void subghz_scene_receiver_info_on_enter(void* context) {
    SubGhz* subghz = context;

    subghz_custom_btns_reset();

    subghz_scene_receiver_info_draw_widget(subghz);

    if(subghz_current_signal_is_from_file(subghz)) {
        subghz->state_notifications = SubGhzNotificationStateIDLE;
    } else if(
        !subghz_history_get_text_space_left(subghz->history, NULL) &&
        !scene_manager_has_previous_scene(subghz->scene_manager, SubGhzSceneDecodeRAW)) {
        subghz->state_notifications = SubGhzNotificationStateRx;
    }
}

bool subghz_scene_receiver_info_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneReceiverInfoTxStart) {
            if(!subghz_scene_receiver_info_update_parser(subghz)) {
                return false;
            }
            FlipperFormat* ff = subghz_get_current_signal_data(subghz);
            if(!subghz_block_generic_global.hitag2_replay_only &&
               (fiat_v1_flipper_needs_hitag2_key(ff) || renault_v1_flipper_needs_hitag2_key(ff))) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneFiatV1Key);
                return true;
            }
            if(subghz_current_signal_is_from_file(subghz)) {
                subghz->state_notifications = SubGhzNotificationStateIDLE;
                subghz_block_generic_global.endless_tx = false;
                subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
                subghz_txrx_stop(subghz->txrx);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
                return true;
            }
            if(subghz_scene_receiver_info_prepare_custom_buttons(subghz)) {
                subghz_scene_receiver_info_copy_history_to_transmitter(subghz);
                subghz->state_notifications = SubGhzNotificationStateIDLE;
                subghz_block_generic_global.endless_tx = false;
                subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
                subghz_txrx_stop(subghz->txrx);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
                return true;
            }
            //CC1101 Stop RX -> Start TX
            subghz_txrx_hopper_pause(subghz->txrx);
            // key concept: we start endless TX until user release OK button, and after this we send last
            // protocols repeats - this guarantee that one press OK will
            // be guarantee send the required minimum protocol data packets
            // for all of this we use subghz_block_generic_global.endless_tx in protocols _yield function.
            subghz->state_notifications = SubGhzNotificationStateTx;
            subghz_block_generic_global.endless_tx = true;
            if(!subghz_tx_start(subghz, subghz_get_current_signal_data(subghz))) {
                subghz_txrx_rx_start(subghz->txrx);
                subghz_txrx_hopper_unpause(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
                subghz_block_generic_global.endless_tx = false;
            }
            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoTxStop) {
            //CC1101 Stop Tx -> next tick event Start RX
            // user release OK
            // we switch off endless_tx - that mean protocols yield finish endless transmission,
            // send upload "repeat=xx" times, and after will be stoped by the tick event down in this code
            subghz->state_notifications = SubGhzNotificationStateTxWait;
            subghz_block_generic_global.endless_tx = false;

            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoBruteforceStart) {
            subghz_psa_bf_start(subghz);
            return true;
        } else if(event.event == SubGhzCustomEventSceneFiatV1KeyTxStart) {
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStart);
            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoBfDone) {
            widget_reset(subghz->widget);
            subghz_scene_receiver_info_draw_widget(subghz);
            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoSave) {
            //CC1101 Stop RX -> Save
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);

            subghz_txrx_stop(subghz->txrx);
            if(!subghz_scene_receiver_info_update_parser(subghz)) {
                return false;
            }

            if(subghz_txrx_protocol_is_serializable(subghz->txrx)) {
                if(scene_manager_has_previous_scene(subghz->scene_manager, SubGhzSceneDecodeRAW)) {
                    if(subghz_path_is_file(subghz->file_path)) {
                        furi_string_set(subghz->file_path_tmp, subghz->file_path);
                    }
                    furi_string_set(subghz->file_path, SUBGHZ_APP_FOLDER);
                } else {
                    subghz_file_name_clear(subghz);
                }

                if(subghz_current_signal_is_from_file(subghz)) {
                    subghz->save_datetime_set = false;
                } else {
                    subghz->save_datetime =
                        subghz_history_get_datetime(subghz->history, subghz->idx_menu_chosen);
                    subghz->save_datetime_set = true;
                }
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);
            }
            return true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(subghz_psa_bf_handle_back()) {
            widget_reset(subghz->widget);
            subghz_scene_receiver_info_draw_widget(subghz);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz_psa_bf_handle_tick(subghz)) {
            return true;
        }
        if(subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF) {
            subghz_txrx_hopper_update(subghz->txrx, subghz->last_settings->hopping_threshold);
        }
        switch(subghz->state_notifications) {
        case SubGhzNotificationStateTx:
            notification_message(subghz->notifications, &sequence_blink_magenta_10);
            break;
        case SubGhzNotificationStateRx:
            notification_message(subghz->notifications, &sequence_blink_cyan_10);
            break;
        case SubGhzNotificationStateRxDone:
            notification_message(subghz->notifications, &sequence_blink_green_100);
            subghz->state_notifications = SubGhzNotificationStateRx;
            break;
        case SubGhzNotificationStateTxWait:
            // we wait until hardware TX finished and after stop TX and start RX, else just blink led
            if(!subghz_devices_is_async_complete_tx(subghz->txrx->radio_device)) {
                notification_message(subghz->notifications, &sequence_blink_magenta_10);
            } else {
                subghz_txrx_stop(subghz->txrx);
                // update screen
                widget_reset(subghz->widget);
                subghz_scene_receiver_info_draw_widget(subghz);

                subghz->state_notifications = SubGhzNotificationStateIDLE;

                if(!scene_manager_has_previous_scene(subghz->scene_manager, SubGhzSceneDecodeRAW)) {
                    subghz_txrx_rx_start(subghz->txrx);
                    subghz_txrx_hopper_unpause(subghz->txrx);
                    if(!subghz_history_get_text_space_left(subghz->history, NULL)) {
                        subghz->state_notifications = SubGhzNotificationStateRx;
                    }
                }
            }
            break;
        default:
            break;
        }
    }
    return false;
}

void subghz_scene_receiver_info_on_exit(void* context) {
    SubGhz* subghz = context;

    subghz_block_generic_global.endless_tx = false;
    subghz_txrx_stop(subghz->txrx);
    subghz_psa_bf_on_exit();
    widget_reset(subghz->widget);
    subghz_txrx_reset_dynamic_and_custom_btns(subghz->txrx);
}
