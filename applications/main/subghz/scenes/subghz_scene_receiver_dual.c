#include "../subghz_i.h"
#include <dolphin/dolphin.h>
#include <math.h>

#define TAG "SubGhzSceneReceiverDual"

// Dual-RX default configuration.
#define SUBGHZ_DUAL_DEFAULT_FREQ    433920000
#define SUBGHZ_DUAL_DEFAULT_INT_MOD "AM650"
#define SUBGHZ_DUAL_DEFAULT_EXT_MOD "FM476"

// Radio source markers stored in history and shown as the list prefix.
#define SUBGHZ_DUAL_SOURCE_INT 0
#define SUBGHZ_DUAL_SOURCE_EXT 1

static const char* subghz_scene_receiver_dual_prefix(uint8_t source) {
    return (source == SUBGHZ_DUAL_SOURCE_EXT) ? "[E] " : "[I] ";
}

// Refresh the merged signal count shown in the list status bar / waveform.
static void subghz_scene_receiver_dual_update_statusbar(SubGhz* subghz) {
    FuriString* stat = furi_string_alloc();
    subghz_history_get_text_space_left(subghz->history, stat);
    subghz_view_receiver_add_data_statusbar(
        subghz->subghz_receiver, "", "", furi_string_get_cstr(stat), false, false, false, false, 0);
    furi_string_free(stat);
}

// Set the two waveform pane labels (radio frequency + modulation).
static void subghz_scene_receiver_dual_set_labels(SubGhz* subghz) {
    FuriString* freq = furi_string_alloc();
    FuriString* mod = furi_string_alloc();
    FuriString* int_str = furi_string_alloc();
    FuriString* ext_str = furi_string_alloc();

    subghz_txrx_dual_get_frequency_and_modulation(subghz->txrx, false, freq, mod, true);
    furi_string_printf(int_str, "%s %s", furi_string_get_cstr(freq), furi_string_get_cstr(mod));
    subghz_txrx_dual_get_frequency_and_modulation(subghz->txrx, true, freq, mod, true);
    furi_string_printf(ext_str, "%s %s", furi_string_get_cstr(freq), furi_string_get_cstr(mod));

    subghz_view_receiver_set_dual(
        subghz->subghz_receiver,
        true,
        furi_string_get_cstr(int_str),
        furi_string_get_cstr(ext_str));

    furi_string_free(ext_str);
    furi_string_free(int_str);
    furi_string_free(mod);
    furi_string_free(freq);
}

void subghz_scene_receiver_dual_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

static void
    subghz_scene_receiver_dual_log_frame(SubGhzProtocolDecoderBase* decoder_base, uint8_t source) {
    if(furi_log_get_level() < FuriLogLevelDebug) return;

    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder_base, text);
    for(size_t i = 0; i < furi_string_size(text); i++) {
        const char c = furi_string_get_char(text, i);
        if(c == '\n' || c == '\r') {
            furi_string_set_char(text, i, ' ');
        }
    }
    FURI_LOG_D(
        TAG,
        "%s%s %s",
        subghz_scene_receiver_dual_prefix(source),
        decoder_base->protocol->name,
        furi_string_get_cstr(text));
    furi_string_free(text);
}

// Shared add-to-history handler
static void subghz_scene_receiver_dual_add(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    SubGhz* subghz,
    bool external) {
    if((decoder_base->protocol->flag & subghz->ignore_filter) != 0) {
        FURI_LOG_D(TAG, "%s protocol ignored", decoder_base->protocol->name);
        subghz_receiver_reset(receiver);
        return;
    }

    const uint8_t source = external ? SUBGHZ_DUAL_SOURCE_EXT : SUBGHZ_DUAL_SOURCE_INT;
    SubGhzHistory* history = subghz->history;
    SubGhzRadioPreset preset = external ? subghz_txrx_dual_get_preset_ext(subghz->txrx) :
                                          subghz_txrx_dual_get_preset_int(subghz->txrx);
    if(subghz_history_add_to_history_source(history, decoder_base, &preset, source)) {
        subghz_scene_receiver_dual_log_frame(decoder_base, source);
    }
    subghz_receiver_reset(receiver);
}

static void subghz_scene_receiver_dual_add_int(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    furi_assert(context);
    subghz_scene_receiver_dual_add(receiver, decoder_base, context, false);
}

static void subghz_scene_receiver_dual_add_ext(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    furi_assert(context);
    subghz_scene_receiver_dual_add(receiver, decoder_base, context, true);
}

static void subghz_scene_receiver_dual_present_pending(SubGhz* subghz) {
    subghz_txrx_dual_lock(subghz->txrx);
    SubGhzHistory* history = subghz->history;
    const uint16_t count = subghz_history_get_item(history);
    const bool added = subghz->rx_presented < count;
    if(added) {
        FuriString* name = furi_string_alloc();
        FuriString* time = furi_string_alloc();
        FuriString* label = furi_string_alloc();
        const uint16_t end = MIN(count, subghz->rx_presented + 4);
        while(subghz->rx_presented < end) {
            const uint16_t idx = subghz->rx_presented++;
            subghz_history_get_text_item_menu(history, name, idx);
            subghz_history_get_time_item_menu(history, time, idx);
            furi_string_printf(
                label,
                "%s%s",
                subghz_scene_receiver_dual_prefix(subghz_history_get_source(history, idx)),
                furi_string_get_cstr(name));
            subghz_view_receiver_add_item_to_menu(
                subghz->subghz_receiver,
                furi_string_get_cstr(label),
                furi_string_get_cstr(time),
                subghz_history_get_type_protocol(history, idx),
                subghz_history_get_button_icon(history, idx));
        }
        furi_string_free(label);
        furi_string_free(time);
        furi_string_free(name);
        subghz->state_notifications = SubGhzNotificationStateRxDone;
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);
        subghz_view_receiver_set_dual_dashboard(subghz->subghz_receiver, false);
    }
    if(subghz->last_settings->delete_old_signals && count >= 55 && subghz->rx_presented) {
        subghz_history_delete_item(history, 0);
        subghz_view_receiver_delete_item(subghz->subghz_receiver, 0);
        subghz->rx_presented--;
        subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
    }
    if(added) subghz_scene_receiver_dual_update_statusbar(subghz);
    subghz_txrx_dual_unlock(subghz->txrx);
}

void subghz_scene_receiver_dual_on_enter(void* context) {
    SubGhz* subghz = context;
    SubGhzHistory* history = subghz->history;
    subghz_txrx_dual_rx_stop(subghz->txrx);

    FuriString* item_name = furi_string_alloc();
    FuriString* item_time = furi_string_alloc();
    FuriString* prefixed = furi_string_alloc();

    if(subghz_rx_key_state_get(subghz) == SubGhzRxKeyStateIDLE) {
        subghz_txrx_dual_set_preset_int(
            subghz->txrx, SUBGHZ_DUAL_DEFAULT_INT_MOD, SUBGHZ_DUAL_DEFAULT_FREQ);
        subghz_txrx_dual_set_preset_ext(
            subghz->txrx, SUBGHZ_DUAL_DEFAULT_EXT_MOD, SUBGHZ_DUAL_DEFAULT_FREQ);

        subghz->filter = SubGhzProtocolFlag_Decodable;
        subghz->ignore_filter = subghz->last_settings->ignore_filter;

        subghz_history_reset(history);
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateStart);
        subghz->idx_menu_chosen = 0;
    }

    subghz_view_receiver_set_mode(subghz->subghz_receiver, SubGhzViewReceiverModeLive);

    // Rebuild the merged list from history (with each item's I/E prefix)
    subghz_view_receiver_exit(subghz->subghz_receiver);
    for(uint16_t i = 0; i < subghz_history_get_item(history); i++) {
        furi_string_reset(item_name);
        furi_string_reset(item_time);
        subghz_history_get_text_item_menu(history, item_name, i);
        subghz_history_get_time_item_menu(history, item_time, i);
        furi_string_printf(
            prefixed,
            "%s%s",
            subghz_scene_receiver_dual_prefix(subghz_history_get_source(history, i)),
            furi_string_get_cstr(item_name));
        subghz_view_receiver_add_item_to_menu(
            subghz->subghz_receiver,
            furi_string_get_cstr(prefixed),
            furi_string_get_cstr(item_time),
            subghz_history_get_type_protocol(history, i),
            subghz_history_get_button_icon(history, i));
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);
    }
    furi_string_free(prefixed);
    furi_string_free(item_name);
    furi_string_free(item_time);

    subghz_view_receiver_set_callback(
        subghz->subghz_receiver, subghz_scene_receiver_dual_callback, subghz);

    if(!subghz_txrx_dual_rx_start(subghz->txrx)) {
        notification_message(subghz->notifications, &sequence_error);
        subghz_dialog_message_no_ext_module(subghz);
        scene_manager_search_and_switch_to_previous_scene(subghz->scene_manager, SubGhzSceneStart);
        return;
    }

    subghz->rx_presented = subghz_history_get_item(history);
    subghz_txrx_dual_set_filter(subghz->txrx, subghz->filter);
    subghz_txrx_dual_set_rx_callback(
        subghz->txrx,
        subghz_scene_receiver_dual_add_int,
        subghz_scene_receiver_dual_add_ext,
        subghz);
    if(!subghz_txrx_dual_begin(subghz->txrx)) {
        notification_message(subghz->notifications, &sequence_error);
        scene_manager_search_and_switch_to_previous_scene(subghz->scene_manager, SubGhzSceneStart);
        return;
    }
    subghz_txrx_dual_lock(subghz->txrx);
    subghz_scene_receiver_dual_set_labels(subghz);
    subghz_scene_receiver_dual_update_statusbar(subghz);

    subghz_view_receiver_set_dual_dashboard(
        subghz->subghz_receiver, subghz_history_get_item(history) == 0);
    subghz_view_receiver_set_idx_menu(subghz->subghz_receiver, subghz->idx_menu_chosen);
    subghz_txrx_dual_unlock(subghz->txrx);

    // Blink the cyan "RX active" LED while dual RX is running (like single RX).
    subghz->state_notifications = SubGhzNotificationStateRx;
    subghz_view_receiver_set_lock(subghz->subghz_receiver, subghz_is_locked(subghz));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdReceiver);
}

bool subghz_scene_receiver_dual_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
        const bool dashboard = subghz_view_receiver_get_dual_dashboard(subghz->subghz_receiver);

        switch(event.event) {
        case SubGhzCustomEventViewReceiverBack:
            if(!dashboard) {
                // History list -> back to the split waveform screen.
                subghz_view_receiver_set_dual_dashboard(subghz->subghz_receiver, true);
                consumed = true;
                break;
            }
            // Waveform screen -> leave dual RX
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_dual_rx_stop(subghz->txrx);
            subghz_txrx_dual_set_rx_callback(subghz->txrx, NULL, NULL, subghz);

            if(subghz_rx_key_state_get(subghz) == SubGhzRxKeyStateAddKey) {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateExit);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneNeedSaving);
            } else {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
                subghz_txrx_set_default_preset(subghz->txrx, subghz->last_settings->frequency);
                scene_manager_search_and_switch_to_previous_scene(
                    subghz->scene_manager, SubGhzSceneStart);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverOK:
            if(dashboard) {
                // Waveform screen: open the merged list if there are signals.
                if(subghz_history_get_item(subghz->history) > 0) {
                    subghz_view_receiver_set_dual_dashboard(subghz->subghz_receiver, false);
                }
            } else {
                subghz_txrx_dual_rx_stop(subghz->txrx);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiverInfo);
                dolphin_deed(DolphinDeedSubGhzReceiverInfo);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverDeleteItem:
            subghz->state_notifications = SubGhzNotificationStateRx;
            subghz_txrx_dual_lock(subghz->txrx);
            subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);
            subghz_history_delete_item(subghz->history, subghz->idx_menu_chosen);
            if(subghz->idx_menu_chosen < subghz->rx_presented) subghz->rx_presented--;
            subghz_view_receiver_delete_item(
                subghz->subghz_receiver,
                subghz_view_receiver_get_idx_menu(subghz->subghz_receiver));
            subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);
            subghz_scene_receiver_dual_update_statusbar(subghz);
            subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
            if(subghz_history_get_last_index(subghz->history) == 0) {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateStart);
                subghz_view_receiver_set_dual_dashboard(subghz->subghz_receiver, true);
            }
            subghz_txrx_dual_unlock(subghz->txrx);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverConfig:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiverDualConfig);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverOffDisplay:
            notification_message(subghz->notifications, &sequence_display_backlight_off);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverUnlock:
            subghz_unlock(subghz);
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        subghz_scene_receiver_dual_present_pending(subghz);
        if(subghz_view_receiver_get_dual_dashboard(subghz->subghz_receiver)) {
            SubGhzThresholdRssiData rssi_int = subghz_threshold_get_rssi_data(
                subghz->threshold_rssi, subghz_txrx_dual_get_rssi_int(subghz->txrx));
            subghz_receiver_rssi(subghz->subghz_receiver, rssi_int.rssi, NAN);
            SubGhzThresholdRssiData rssi_ext = subghz_threshold_get_rssi_data(
                subghz->threshold_rssi, subghz_txrx_dual_get_rssi_ext(subghz->txrx));
            subghz_view_receiver_rssi_ext(subghz->subghz_receiver, rssi_ext.rssi);
        }

        switch(subghz->state_notifications) {
        case SubGhzNotificationStateRx:
            notification_message(subghz->notifications, &sequence_blink_cyan_10);
            break;
        case SubGhzNotificationStateRxDone:
            if(!subghz_is_locked(subghz)) {
                notification_message(subghz->notifications, &subghz_sequence_rx);
            } else {
                notification_message(subghz->notifications, &subghz_sequence_rx_locked);
            }
            subghz->state_notifications = SubGhzNotificationStateRx;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void subghz_scene_receiver_dual_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_dual_rx_stop(subghz->txrx);
    subghz_txrx_dual_set_rx_callback(subghz->txrx, NULL, NULL, subghz);
    subghz_view_receiver_set_dual(subghz->subghz_receiver, false, NULL, NULL);
}
