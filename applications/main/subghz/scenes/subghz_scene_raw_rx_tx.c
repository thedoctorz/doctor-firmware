#include "../subghz_i.h"
#include "../helpers/subghz_txrx_i.h"
#include <lib/subghz/protocols/raw.h>
#include <storage/storage.h>
#include <math.h>

#define RAW_RX_TX_FOLDER    EXT_PATH("subghz/jam")
#define RAW_RX_TX_MAX_FILES 64

typedef struct {
    FuriString* path;
    SubGhzRadioPreset preset;
} SubGhzRawRxTxFile;

typedef struct {
    SubGhzRawRxTxFile* files;
    size_t count;
    size_t selected;
    bool transmitting;
    SubGhzRadioPreset saved_preset;
    FlipperFormat* tx_data;
} SubGhzRawRxTx;

static void subghz_raw_rx_tx_file_clear(SubGhzRawRxTxFile* file) {
    furi_string_free(file->path);
    furi_string_free(file->preset.name);
    free(file->preset.data);
}

static bool subghz_raw_rx_tx_load(SubGhz* subghz, const char* path, SubGhzRawRxTxFile* file) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* text = furi_string_alloc();
    uint32_t version = 0;
    uint32_t frequency = 0;
    bool valid = false;
    memset(file, 0, sizeof(*file));
    file->path = furi_string_alloc_set(path);
    file->preset.name = furi_string_alloc();
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    do {
        if(!flipper_format_file_open_existing(ff, path) ||
           !flipper_format_read_header(ff, text, &version) ||
           strcmp(furi_string_get_cstr(text), SUBGHZ_RAW_FILE_TYPE) ||
           version != SUBGHZ_KEY_FILE_VERSION ||
           !flipper_format_read_uint32(ff, "Frequency", &frequency, 1) ||
           !subghz_txrx_radio_device_is_frequency_valid(subghz->txrx, frequency) ||
           !flipper_format_read_string(ff, "Preset", text))
            break;
        const char* name = subghz_txrx_get_preset_name(subghz->txrx, furi_string_get_cstr(text));
        if(!*name) break;
        furi_string_set(file->preset.name, name);
        file->preset.frequency = frequency;
        if(!strcmp(name, "CUSTOM")) {
            uint32_t size = 0;
            if(!flipper_format_read_string(ff, "Custom_preset_module", text) ||
               strcmp(furi_string_get_cstr(text), "CC1101") ||
               !flipper_format_get_value_count(ff, "Custom_preset_data", &size) || size < 10 ||
               size > SUBGHZ_SETTING_MAX_PRESET_DATA_SIZE || size % 2)
                break;
            file->preset.data_size = size;
            file->preset.data = malloc(size);
            if(!flipper_format_read_hex(ff, "Custom_preset_data", file->preset.data, size)) break;
            /* CC1101 register/value pairs must terminate before the eight PA bytes. */
            size_t end = 0;
            while(end + 1 < size && file->preset.data[end])
                end += 2;
            if(end + 10 != size) break;
            if(strstr(subghz_txrx_radio_device_get_name(subghz->txrx), "si4463")) break;
        } else {
            int index = subghz_setting_get_inx_preset_by_name(setting, name);
            if(index < 0 || !subghz_txrx_preset_fits_radio(subghz->txrx, index)) break;
            file->preset.data_size = subghz_setting_get_preset_data_size(setting, index);
            if(file->preset.data_size < 10) break;
            file->preset.data = malloc(file->preset.data_size);
            memcpy(
                file->preset.data,
                subghz_setting_get_preset_data(setting, index),
                file->preset.data_size);
        }
        if(!flipper_format_read_string(ff, "Protocol", text) ||
           strcmp(furi_string_get_cstr(text), "RAW"))
            break;
        int32_t sample = 0;
        if(!flipper_format_read_int32(ff, "RAW_Data", &sample, 1) || !sample) break;
        subghz_txrx_set_tx_power(file->preset.data, file->preset.data_size, 0);
        valid = true;
    } while(false);
    flipper_format_free(ff);
    furi_string_free(text);
    furi_record_close(RECORD_STORAGE);
    if(!valid) subghz_raw_rx_tx_file_clear(file);
    return valid;
}

static int subghz_raw_rx_tx_compare(const void* a, const void* b) {
    const SubGhzRawRxTxFile* left = a;
    const SubGhzRawRxTxFile* right = b;
    if(left->preset.frequency != right->preset.frequency)
        return left->preset.frequency < right->preset.frequency ? -1 : 1;
    return strcmp(furi_string_get_cstr(left->path), furi_string_get_cstr(right->path));
}

static void subghz_raw_rx_tx_update(SubGhz* subghz) {
    SubGhzRawRxTx* state = subghz->raw_rx_tx;
    if(!state->count) return;
    SubGhzRawRxTxFile* file = &state->files[state->selected];
    FuriString* frequency = furi_string_alloc();
    FuriString* modulation = furi_string_alloc();
    subghz_txrx_get_frequency_and_modulation(subghz->txrx, frequency, modulation, true);
    subghz_view_receiver_add_data_statusbar(
        subghz->subghz_receiver,
        furi_string_get_cstr(frequency),
        furi_string_get_cstr(modulation),
        "",
        false,
        false,
        false,
        false,
        0);
    subghz_view_receiver_set_radio_device_type(
        subghz->subghz_receiver, subghz_txrx_radio_device_get(subghz->txrx));
    const char* path = furi_string_get_cstr(file->path);
    const char* filename = strrchr(path, '/');
    subghz_view_receiver_set_raw_rx_tx(
        subghz->subghz_receiver, true, state->transmitting, filename ? filename + 1 : path);
    furi_string_free(frequency);
    furi_string_free(modulation);
}

static void subghz_raw_rx_tx_receive(SubGhz* subghz) {
    SubGhzRawRxTx* state = subghz->raw_rx_tx;
    subghz_txrx_stop(subghz->txrx);
    state->transmitting = false;
    SubGhzRadioPreset* preset = &state->files[state->selected].preset;
    subghz_txrx_set_preset(
        subghz->txrx,
        furi_string_get_cstr(preset->name),
        preset->frequency,
        preset->data,
        preset->data_size);
    subghz_txrx_rssi_start(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateRx;
    notification_message(subghz->notifications, &sequence_blink_stop);
    notification_message(subghz->notifications, &sequence_blink_start_cyan);
    subghz_raw_rx_tx_update(subghz);
}

static void subghz_raw_rx_tx_callback(SubGhzCustomEvent event, void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_raw_rx_tx_on_enter(void* context) {
    SubGhz* subghz = context;
    SubGhzRawRxTx* state = calloc(1, sizeof(*state));
    subghz->raw_rx_tx = state;
    state->saved_preset = subghz_txrx_get_preset(subghz->txrx);
    state->saved_preset.name = furi_string_alloc_set(state->saved_preset.name);
    state->tx_data = flipper_format_string_alloc();
    state->files = calloc(RAW_RX_TX_MAX_FILES, sizeof(*state->files));
    subghz_txrx_hopper_suspend(subghz->txrx);
    subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
    subghz_txrx_stop(subghz->txrx);
    subghz_txrx_set_rx_callback(subghz->txrx, NULL, NULL);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    subghz_view_receiver_exit(subghz->subghz_receiver);
    subghz_view_receiver_set_mode(subghz->subghz_receiver, SubGhzViewReceiverModeLive);
    subghz_view_receiver_set_lock(subghz->subghz_receiver, false);
    subghz_view_receiver_set_frequency_callback(subghz->subghz_receiver, NULL);
    subghz_view_receiver_set_callback(subghz->subghz_receiver, subghz_raw_rx_tx_callback, subghz);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    FileInfo info;
    char name[256];
    FuriString* path = furi_string_alloc();
    if(storage_dir_open(dir, RAW_RX_TX_FOLDER)) {
        while(state->count < RAW_RX_TX_MAX_FILES &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            size_t length = strlen(name);
            if(file_info_is_dir(&info) || length < 4 || strcmp(name + length - 4, ".sub"))
                continue;
            furi_string_printf(path, "%s/%s", RAW_RX_TX_FOLDER, name);
            if(subghz_raw_rx_tx_load(
                   subghz, furi_string_get_cstr(path), &state->files[state->count]))
                state->count++;
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(path);
    if(!state->count) {
        dialog_message_show_storage_error(
            subghz->dialogs, "No usable RAW files\nin /ext/subghz/tx");
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventViewReceiverBack);
        return;
    }
    qsort(state->files, state->count, sizeof(*state->files), subghz_raw_rx_tx_compare);
    subghz_raw_rx_tx_receive(subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdReceiver);
}

bool subghz_scene_raw_rx_tx_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    SubGhzRawRxTx* state = subghz->raw_rx_tx;
    if(event.type == SceneManagerEventTypeBack ||
       (event.type == SceneManagerEventTypeCustom &&
        event.event == SubGhzCustomEventViewReceiverBack)) {
        scene_manager_previous_scene(subghz->scene_manager);
        return true;
    }
    if(!state->count) return false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewReceiverConfig) {
            if(state->transmitting) {
                subghz_raw_rx_tx_receive(subghz);
            } else {
                SubGhzRawRxTxFile* file = &state->files[state->selected];
                subghz_protocol_raw_gen_fff_data(
                    state->tx_data,
                    furi_string_get_cstr(file->path),
                    subghz_txrx_radio_device_get_name(subghz->txrx));
                uint32_t loop = 1;
                flipper_format_write_uint32(state->tx_data, "Loop", &loop, 1);
                state->transmitting = subghz_tx_start(subghz, state->tx_data);
                if(state->transmitting) {
                    subghz->state_notifications = SubGhzNotificationStateTx;
                    notification_message(subghz->notifications, &sequence_blink_stop);
                    notification_message(subghz->notifications, &sequence_blink_start_magenta);
                } else {
                    subghz_raw_rx_tx_receive(subghz);
                }
                subghz_raw_rx_tx_update(subghz);
            }
            return true;
        }
        if(!state->transmitting && (event.event == SubGhzCustomEventViewReceiverNextFrequency ||
                                    event.event == SubGhzCustomEventViewReceiverPrevFrequency)) {
            state->selected = event.event == SubGhzCustomEventViewReceiverNextFrequency ?
                                  (state->selected + 1) % state->count :
                                  (state->selected + state->count - 1) % state->count;
            subghz_raw_rx_tx_receive(subghz);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(state->transmitting) {
            if(subghz_devices_is_async_complete_tx(subghz->txrx->radio_device)) {
                subghz_raw_rx_tx_receive(subghz);
                dialog_message_show_storage_error(
                    subghz->dialogs, "RAW playback stopped\nCheck file / SD card");
            }
        } else {
            SubGhzThresholdRssiData rssi = subghz_threshold_get_rssi_data(
                subghz->threshold_rssi, subghz_txrx_radio_device_get_rssi(subghz->txrx));
            subghz_receiver_rssi(subghz->subghz_receiver, rssi.rssi, NAN);
        }
        return true;
    }
    return false;
}

void subghz_scene_raw_rx_tx_on_exit(void* context) {
    SubGhz* subghz = context;
    SubGhzRawRxTx* state = subghz->raw_rx_tx;
    subghz_txrx_stop(subghz->txrx);
    subghz_txrx_set_preset(
        subghz->txrx,
        furi_string_get_cstr(state->saved_preset.name),
        state->saved_preset.frequency,
        state->saved_preset.data,
        state->saved_preset.data_size);
    subghz_view_receiver_set_raw_rx_tx(subghz->subghz_receiver, false, false, NULL);
    subghz_view_receiver_exit(subghz->subghz_receiver);
    for(size_t i = 0; i < state->count; i++)
        subghz_raw_rx_tx_file_clear(&state->files[i]);
    free(state->files);
    flipper_format_free(state->tx_data);
    furi_string_free(state->saved_preset.name);
    free(state);
    subghz->raw_rx_tx = NULL;
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    notification_message(subghz->notifications, &sequence_blink_stop);
}
