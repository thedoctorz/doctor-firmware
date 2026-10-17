#include "../subghz_i.h"
#include <dolphin/dolphin.h>
#include <toolbox/name_generator.h>
#include <math.h>

#define SUBGHZ_AUTOSAVE_FOLDER SUBGHZ_APP_FOLDER "/autosaved"

#define SUBGHZ_RECEIVER_HISTORY_TRIM 54

#define TAG "SubGhzSceneReceiver"

const NotificationSequence subghz_sequence_rx = {
    &message_green_255,

    &message_display_backlight_on,

    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_50,
    NULL,
};

const NotificationSequence subghz_sequence_rx_locked = {
    &message_green_255,

    &message_display_backlight_on,

    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_500,

    &message_display_backlight_off,
    NULL,
};

static void subghz_scene_receiver_update_statusbar(void* context) {
    SubGhz* subghz = context;
    FuriString* history_stat_str = furi_string_alloc();
    SubGhzGpsSource gps_source = subghz->gps ? subghz_gps_get_source(subghz->gps) :
                                               SubGhzGpsSourceNone;
    bool gps_present = subghz->gps && subghz_gps_is_present(subghz->gps);
    bool gps_companion = gps_source == SubGhzGpsSourceCompanion;
    uint8_t gps_sats = gps_present ? subghz_gps_get_satellites(subghz->gps) : 0;

    if(!subghz_history_get_text_space_left(subghz->history, history_stat_str)) {
        FuriString* frequency_str = furi_string_alloc();
        FuriString* modulation_str = furi_string_alloc();

#ifdef SUBGHZ_EXT_PRESET_NAME
        if(subghz_history_get_last_index(subghz->history) > 0) {
            subghz_txrx_get_frequency_and_modulation(
                subghz->txrx, frequency_str, modulation_str, false);
        } else {
            FuriString* temp_str = furi_string_alloc();

            subghz_txrx_get_frequency_and_modulation(subghz->txrx, frequency_str, temp_str, true);
            furi_string_set(modulation_str, temp_str);
            furi_string_free(temp_str);
        }
#else
        subghz_txrx_get_frequency_and_modulation(
            subghz->txrx, frequency_str, modulation_str, false);
#endif

        subghz_view_receiver_add_data_statusbar(
            subghz->subghz_receiver,
            furi_string_get_cstr(frequency_str),
            furi_string_get_cstr(modulation_str),
            furi_string_get_cstr(history_stat_str),
            subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF,
            READ_BIT(subghz->filter, SubGhzProtocolFlag_BinRAW) > 0,
            gps_present,
            gps_companion,
            gps_sats);

        furi_string_free(frequency_str);
        furi_string_free(modulation_str);
    } else {
        subghz_view_receiver_add_data_statusbar(
            subghz->subghz_receiver,
            furi_string_get_cstr(history_stat_str),
            "",
            "",
            subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF,
            READ_BIT(subghz->filter, SubGhzProtocolFlag_BinRAW) > 0,
            gps_present,
            gps_companion,
            gps_sats);
        subghz->state_notifications = SubGhzNotificationStateIDLE;
    }
    furi_string_free(history_stat_str);

    subghz_view_receiver_set_radio_device_type(
        subghz->subghz_receiver, subghz_txrx_radio_device_get(subghz->txrx));
}

static uint32_t subghz_scene_receiver_live_frequency(void* context) {
    SubGhz* subghz = context;
    return subghz_txrx_get_frequency_hz(subghz->txrx);
}

void subghz_scene_receiver_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}
#define SUBGHZ_RECEIVER_TUNE_DEBOUNCE_TICKS 2

static void subghz_scene_receiver_schedule_tune(SubGhz* subghz, bool needs_preset_reload) {
    if(needs_preset_reload) {
        subghz->receiver_tune_needs_preset_reload = true;
    }
    subghz->receiver_settings_dirty = true;
    subghz->receiver_tune_debounce = SUBGHZ_RECEIVER_TUNE_DEBOUNCE_TICKS;
}

static void subghz_scene_receiver_apply_pending_tune(SubGhz* subghz) {
    const bool reload_preset = subghz->receiver_tune_needs_preset_reload;
    const bool dirty = subghz->receiver_settings_dirty;

    subghz->receiver_tune_debounce = 0;
    subghz->receiver_tune_needs_preset_reload = false;

    SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
    if(reload_preset) {
        subghz_txrx_rx_start(subghz->txrx);
    } else {
        subghz_txrx_rx_retune(subghz->txrx, preset.frequency);
    }

    if(dirty) {
        subghz_last_settings_save(subghz->last_settings);
        subghz->receiver_settings_dirty = false;
    }

    subghz_scene_receiver_update_statusbar(subghz);
}

static void subghz_scene_receiver_cycle_frequency(SubGhz* subghz, bool next) {
    if(subghz_txrx_hopper_owns_frequency(subghz->txrx)) return;

    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
    const size_t count = subghz_setting_get_frequency_count(setting);
    if(count == 0) return;

    size_t index = 0;
    for(size_t i = 0; i < count; i++) {
        if(preset.frequency == subghz_setting_get_frequency(setting, i)) {
            index = i;
            break;
        }
    }
    index = next ? (index + 1) % count : (index + count - 1) % count;
    index = subghz_txrx_frequency_valid_index(subghz->txrx, index, next);
    uint32_t frequency = subghz_setting_get_frequency(setting, index);

    subghz_txrx_set_frequency(subghz->txrx, frequency);

    subghz->last_settings->frequency = frequency;
    subghz_setting_set_default_frequency(setting, frequency);

    subghz_scene_receiver_schedule_tune(subghz, false);
    subghz_scene_receiver_update_statusbar(subghz);
}

static void subghz_scene_receiver_cycle_preset(SubGhz* subghz, bool next) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);

    if(subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF &&
       subghz_txrx_hopper_get_mode(subghz->txrx) == SubGhzHopperModePreset) {
        return;
    }

    SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
    const size_t count = subghz_setting_get_preset_count(setting);
    if(count == 0) return;

    size_t index = 0;
    for(size_t i = 0; i < count; i++) {
        if(!strcmp(subghz_setting_get_preset_name(setting, i), furi_string_get_cstr(preset.name))) {
            index = i;
            break;
        }
    }
    index = next ? (index + 1) % count : (index + count - 1) % count;

    const char* preset_name = subghz_setting_get_preset_name(setting, index);
    uint8_t* preset_data = subghz_setting_get_preset_data(setting, index);
    size_t preset_data_size = subghz_setting_get_preset_data_size(setting, index);

    subghz_txrx_set_tx_power(preset_data, preset_data_size, subghz->tx_power);
    subghz_txrx_set_preset(
        subghz->txrx, preset_name, preset.frequency, preset_data, preset_data_size);

    subghz->last_settings->preset_index = index;

    subghz_scene_receiver_schedule_tune(subghz, true);
    subghz_scene_receiver_update_statusbar(subghz);
}

static void subghz_scene_log_decoder_frame(SubGhzProtocolDecoderBase* decoder_base) {
    if(furi_log_get_level() < FuriLogLevelDebug) return;

    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder_base, text);
    for(size_t i = 0; i < furi_string_size(text); i++) {
        const char c = furi_string_get_char(text, i);
        if(c == '\n' || c == '\r') {
            furi_string_set_char(text, i, ' ');
        }
    }
    FURI_LOG_D(TAG, "%s %s", decoder_base->protocol->name, furi_string_get_cstr(text));
    furi_string_free(text);
}

static void subghz_scene_receiver_autosave(SubGhz* subghz, uint16_t idx) {
    if(!subghz->last_settings->autosave) return;

    FlipperFormat* raw = subghz_history_get_raw_data(subghz->history, idx);
    if(!raw) return;

    const SubGhzProtocol* protocol = subghz_history_get_protocol(subghz->history, idx);
    if(protocol == NULL) return;
    if((protocol->flag & SubGhzProtocolFlag_Save) == 0) return;

    const char* protocol_name = protocol->name;

    FuriString* safe_proto = furi_string_alloc();
    for(const char* p = protocol_name; *p != '\0'; p++) {
        char c = *p;
        if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|') {
            c = '_';
        }
        furi_string_push_back(safe_proto, c);
    }

    FuriString* folder = furi_string_alloc_printf(
        "%s/%s", SUBGHZ_AUTOSAVE_FOLDER, furi_string_get_cstr(safe_proto));

    const char* prefix = subghz->last_settings->protocol_file_names ?
                             furi_string_get_cstr(safe_proto) :
                             SUBGHZ_APP_FILENAME_PREFIX;

    char file_name[SUBGHZ_MAX_LEN_NAME] = {0};
    DateTime datetime = subghz_history_get_datetime(subghz->history, idx);
    name_generator_make_auto_datetime(file_name, SUBGHZ_MAX_LEN_NAME, prefix, &datetime);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_simply_mkdir(storage, SUBGHZ_AUTOSAVE_FOLDER)) {
        furi_record_close(RECORD_STORAGE);

        FURI_LOG_E(TAG, "Autosave: cannot create " SUBGHZ_AUTOSAVE_FOLDER);
        furi_string_free(folder);
        furi_string_free(safe_proto);
        return;
    }

    FuriString* name = furi_string_alloc_set(file_name);
    storage_get_next_filename(
        storage,
        furi_string_get_cstr(folder),
        file_name,
        SUBGHZ_APP_FILENAME_EXTENSION,
        name,
        SUBGHZ_MAX_LEN_NAME);
    furi_record_close(RECORD_STORAGE);

    FuriString* path = furi_string_alloc();
    furi_string_printf(
        path,
        "%s/%s%s",
        furi_string_get_cstr(folder),
        furi_string_get_cstr(name),
        SUBGHZ_APP_FILENAME_EXTENSION);

    // subghz_save_protocol_to_file creates the folder (incl. protocol subfolder) if needed
    subghz_save_protocol_to_file(subghz, raw, furi_string_get_cstr(path));

    furi_string_free(name);
    furi_string_free(path);
    furi_string_free(folder);
    furi_string_free(safe_proto);
}

static void subghz_scene_receiver_present_pending(SubGhz* subghz) {
    SubGhzHistory* history = subghz->history;
    const uint16_t count = subghz_history_get_last_index(history);
    const bool added = subghz->rx_presented < count;

    while(subghz->rx_presented < count) {
        const uint16_t idx = subghz->rx_presented;

        FuriString* item_name = furi_string_alloc();
        FuriString* item_time = furi_string_alloc();
        subghz_history_get_text_item_menu(history, item_name, idx);
        subghz_history_get_time_item_menu(history, item_time, idx);
        subghz_view_receiver_add_item_to_menu(
            subghz->subghz_receiver,
            furi_string_get_cstr(item_name),
            furi_string_get_cstr(item_time),
            subghz_history_get_type_protocol(history, idx),
            subghz_history_get_button_icon(history, idx));
        furi_string_free(item_name);
        furi_string_free(item_time);

        subghz->rx_presented++;
    }

    while(subghz->rx_autosaved < count) {
        subghz_scene_receiver_autosave(subghz, subghz->rx_autosaved);
        subghz->rx_autosaved++;
    }

    if(!added) return;

    subghz->state_notifications = SubGhzNotificationStateRxDone;
    subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);

    if(subghz->last_settings->delete_old_signals) {
        uint16_t remaining = subghz_history_get_last_index(history);
        while(remaining > SUBGHZ_RECEIVER_HISTORY_TRIM) {
            subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);
            subghz_history_delete_item(history, 0);
            subghz_view_receiver_delete_item(subghz->subghz_receiver, 0);
            subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);
            if(subghz->rx_presented > 0) subghz->rx_presented--;
            if(subghz->rx_autosaved > 0) subghz->rx_autosaved--;
            subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);

            const uint16_t now_remaining = subghz_history_get_last_index(history);
            if(now_remaining >= remaining) break;
            remaining = now_remaining;
        }
    }

    subghz_scene_receiver_update_statusbar(subghz);
    if(subghz_history_get_text_space_left(history, NULL)) {
        notification_message(subghz->notifications, &sequence_error);
    }
}

static void subghz_scene_add_to_history_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;

    // The check can be moved to /lib/subghz/receiver.c, but may result in false positives
    if((decoder_base->protocol->flag & subghz->ignore_filter) == 0) {
        SubGhzHistory* history = subghz->history;

        FuriString* preset_name = furi_string_alloc();
        subghz_txrx_hopper_lock(subghz->txrx);
        SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
        furi_string_set(preset_name, preset.name);
        preset.name = preset_name;
        subghz_txrx_hopper_unlock(subghz->txrx);

        float gps_lat = 0;
        float gps_lon = 0;
        if(subghz->gps && subghz_gps_get_geotag(subghz->gps, &gps_lat, &gps_lon)) {
            subghz_history_set_location(history, true, gps_lat, gps_lon);
        } else {
            subghz_history_set_location(history, false, 0, 0);
        }

        if(subghz_history_add_to_history(history, decoder_base, &preset)) {
            subghz_scene_log_decoder_frame(decoder_base);
        }
        subghz_txrx_hopper_note_decode(subghz->txrx);
        subghz_receiver_reset(receiver);
        furi_string_free(preset_name);
    } else {
        FURI_LOG_D(TAG, "%s protocol ignored", decoder_base->protocol->name);
    }
}

void subghz_scene_receiver_on_enter(void* context) {
    SubGhz* subghz = context;
    SubGhzHistory* history = subghz->history;

    FuriString* item_name = furi_string_alloc();
    FuriString* item_time = furi_string_alloc();

    if(subghz_rx_key_state_get(subghz) == SubGhzRxKeyStateIDLE) {
        if(subghz->gps) subghz_gps_reset_rx_session(subghz->gps);
        subghz_txrx_set_preset_internal(
            subghz->txrx,
            subghz->last_settings->frequency,
            subghz->last_settings->preset_index,
            subghz->last_settings->tx_power);

        subghz->filter = subghz->last_settings->filter;
        subghz_txrx_receiver_set_filter(subghz->txrx, subghz->filter);
        subghz->ignore_filter = subghz->last_settings->ignore_filter;
        subghz->tx_power = subghz->last_settings->tx_power;

        subghz_history_reset(history);
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateStart);
        subghz->idx_menu_chosen = 0;
        subghz->rx_presented = 0;
        subghz->rx_autosaved = 0;
    }
    if(subghz->gps) subghz_gps_set_rx_session(subghz->gps, true);

    subghz_view_receiver_set_mode(subghz->subghz_receiver, SubGhzViewReceiverModeLive);

    // Load history to receiver
    subghz_view_receiver_exit(subghz->subghz_receiver);
    for(uint16_t i = 0; i < subghz_history_get_item(history); i++) {
        furi_string_reset(item_name);
        furi_string_reset(item_time);
        subghz_history_get_text_item_menu(history, item_name, i);
        subghz_history_get_time_item_menu(history, item_time, i);
        subghz_view_receiver_add_item_to_menu(
            subghz->subghz_receiver,
            furi_string_get_cstr(item_name),
            furi_string_get_cstr(item_time),
            subghz_history_get_type_protocol(history, i),
            subghz_history_get_button_icon(history, i));
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);
    }
    furi_string_free(item_name);
    furi_string_free(item_time);

    subghz->rx_presented = subghz_history_get_item(history);

    subghz_view_receiver_set_callback(
        subghz->subghz_receiver, subghz_scene_receiver_callback, subghz);
    subghz_view_receiver_set_frequency_callback(
        subghz->subghz_receiver, subghz_scene_receiver_live_frequency);
    subghz_txrx_set_rx_callback(subghz->txrx, subghz_scene_add_to_history_callback, subghz);

    if(!subghz_history_get_text_space_left(subghz->history, NULL)) {
        subghz->state_notifications = SubGhzNotificationStateRx;
    }

    subghz_txrx_hopper_set_presets(
        subghz->txrx, subghz->last_settings->hopping_mod_a, subghz->last_settings->hopping_mod_b);
    subghz_txrx_hopper_set_mode(
        subghz->txrx, (SubGhzHopperMode)subghz->last_settings->hopping_mode);

    // Check if hopping was enabled
    if(subghz->last_settings->enable_hopping) {
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateRunning);
    } else {
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
    }

    subghz_txrx_rx_start(subghz->txrx);

    subghz_txrx_hopper_update(subghz->txrx, subghz->last_settings->hopping_threshold);

    subghz_txrx_hopper_resume(subghz->txrx);

    subghz_view_receiver_set_idx_menu(subghz->subghz_receiver, subghz->idx_menu_chosen);

    subghz_scene_receiver_update_statusbar(subghz);

    subghz_view_receiver_set_lock(subghz->subghz_receiver, subghz_is_locked(subghz));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdReceiver);
}

bool subghz_scene_receiver_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        // Save cursor position before going to any other dialog
        subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);

        switch(event.event) {
        case SubGhzCustomEventViewReceiverBack:
            // Stop CC1101 Rx
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_stop(subghz->txrx);
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
            subghz_txrx_set_rx_callback(subghz->txrx, NULL, subghz);

            if(subghz_rx_key_state_get(subghz) == SubGhzRxKeyStateAddKey) {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateExit);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneNeedSaving);
            } else {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
                if(subghz->gps) subghz_gps_reset_rx_session(subghz->gps);
                subghz_txrx_set_default_preset(subghz->txrx, subghz->last_settings->frequency);
                scene_manager_search_and_switch_to_previous_scene(
                    subghz->scene_manager, SubGhzSceneStart);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverOK:
            // Show file info, scene: receiver_info
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiverInfo);
            dolphin_deed(DolphinDeedSubGhzReceiverInfo);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverDeleteItem:
            subghz->state_notifications = SubGhzNotificationStateRx;

            subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);

            subghz_history_delete_item(subghz->history, subghz->idx_menu_chosen);
            subghz_view_receiver_delete_item(
                subghz->subghz_receiver,
                subghz_view_receiver_get_idx_menu(subghz->subghz_receiver));
            subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);

            if(subghz->rx_presented > 0) subghz->rx_presented--;
            if(subghz->rx_autosaved > 0) subghz->rx_autosaved--;

            subghz_scene_receiver_update_statusbar(subghz);
            subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
            if(subghz_history_get_last_index(subghz->history) == 0) {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateStart);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverConfig:
            // Actually signals are received but SubGhzNotificationStateRx is not working inside Config Scene
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzViewIdReceiver, SubGhzCustomEventManagerSet);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiverConfig);
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
        case SubGhzCustomEventViewReceiverNextFrequency:
            subghz_scene_receiver_cycle_frequency(subghz, true);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverPrevFrequency:
            subghz_scene_receiver_cycle_frequency(subghz, false);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverNextPreset:
            subghz_scene_receiver_cycle_preset(subghz, true);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverPrevPreset:
            subghz_scene_receiver_cycle_preset(subghz, false);
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        subghz_scene_receiver_present_pending(subghz);

        if(subghz->receiver_tune_debounce > 0) {
            subghz->receiver_tune_debounce--;
            if(subghz->receiver_tune_debounce == 0) {
                subghz_scene_receiver_apply_pending_tune(subghz);
            }
        }
        if(subghz->gps) {
            subghz_gps_tick_stale(subghz->gps);
        }
        const bool hopping = subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF;
        if(hopping) {
            subghz_txrx_hopper_update(subghz->txrx, subghz->last_settings->hopping_threshold);
        }
        if(subghz->gps || hopping) {
            subghz_scene_receiver_update_statusbar(subghz);
        }

        float hop_rssi = 0.0f;
        float hop_floor = NAN;
        const bool from_hopper = subghz_txrx_hopper_get_level(subghz->txrx, &hop_rssi, &hop_floor);

        SubGhzThresholdRssiData ret_rssi = subghz_threshold_get_rssi_data(
            subghz->threshold_rssi,
            from_hopper ? hop_rssi : subghz_txrx_radio_device_get_rssi(subghz->txrx));

        subghz_receiver_rssi(
            subghz->subghz_receiver, ret_rssi.rssi, from_hopper ? hop_floor : NAN);

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

void subghz_scene_receiver_on_exit(void* context) {
    SubGhz* subghz = context;

    subghz_txrx_hopper_suspend(subghz->txrx);

    if(subghz->receiver_settings_dirty) {
        subghz_last_settings_save(subghz->last_settings);
        subghz->receiver_settings_dirty = false;
    }
    subghz->receiver_tune_debounce = 0;
    subghz->receiver_tune_needs_preset_reload = false;
}
