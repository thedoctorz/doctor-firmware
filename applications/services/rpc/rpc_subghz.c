#include "rpc_i.h"

#include <lib/subghz/subghz_decode_raw.h>
#include <lib/subghz/subghz_rx_session.h>
#include <lib/subghz/subghz_setting.h>
#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <storage/storage.h>
#include <toolbox/path.h>
#include <toolbox/stream/stream.h>

#include <furi.h>
#include <stdlib.h>
#include <string.h>

#define TAG "RpcSubGhz"

typedef struct {
    RpcSession* session;
    FuriThread* decode_thread;
    bool decoding;
    bool decode_stop_requested;
    char* decode_path;
    uint32_t decode_command_id;
    SubGhzRxSession* rx;
} RpcSubGhz;

typedef struct {
    RpcSubGhz* rpc_subghz;
    uint32_t command_id;
} RpcSubGhzDecodeCtx;

static bool rpc_subghz_should_stop(void* context) {
    RpcSubGhzDecodeCtx* ctx = context;
    return ctx->rpc_subghz->decode_stop_requested ||
           rpc_session_is_closed(ctx->rpc_subghz->session);
}

static void rpc_subghz_send_frame(
    RpcSubGhzDecodeCtx* ctx,
    const char* protocol,
    const char* flipper_format,
    uint8_t percent,
    uint32_t hit_count,
    uint32_t activity,
    bool has_next) {
    if(rpc_session_is_closed(ctx->rpc_subghz->session)) return;

    PB_Main* response = rpc_message_alloc();
    response->command_id = ctx->command_id;
    response->command_status = PB_CommandStatus_OK;
    response->has_next = has_next;
    response->which_content = PB_Main_subghz_decode_raw_response_tag;
    response->content.subghz_decode_raw_response.protocol = protocol ? strdup(protocol) : NULL;
    response->content.subghz_decode_raw_response.flipper_format =
        flipper_format ? strdup(flipper_format) : NULL;
    response->content.subghz_decode_raw_response.percent = percent;
    response->content.subghz_decode_raw_response.hit_count = hit_count;
    response->content.subghz_decode_raw_response.activity = activity;
    rpc_send_and_release(ctx->rpc_subghz->session, response);
    free(response);
}

static void
    rpc_subghz_on_progress(uint8_t percent, uint32_t hit_count, uint32_t activity, void* context) {
    RpcSubGhzDecodeCtx* ctx = context;
    rpc_subghz_send_frame(ctx, NULL, NULL, percent, hit_count, activity, percent < 100);
}

static void rpc_subghz_on_hit(
    const char* protocol,
    const char* flipper_format,
    uint8_t percent,
    uint32_t hit_count,
    void* context) {
    RpcSubGhzDecodeCtx* ctx = context;
    rpc_subghz_send_frame(ctx, protocol, flipper_format, percent, hit_count, 0, true);
}

static PB_CommandStatus rpc_subghz_map_decode_status(SubGhzDecodeRawStatus status) {
    switch(status) {
    case SubGhzDecodeRawStatusOk:
        return PB_CommandStatus_OK;
    case SubGhzDecodeRawStatusErrorNotExist:
        return PB_CommandStatus_ERROR_STORAGE_NOT_EXIST;
    case SubGhzDecodeRawStatusErrorInvalid:
        return PB_CommandStatus_ERROR_INVALID_PARAMETERS;
    case SubGhzDecodeRawStatusStopped:
        return PB_CommandStatus_ERROR_CONTINUOUS_COMMAND_INTERRUPTED;
    case SubGhzDecodeRawStatusErrorInternal:
    default:
        return PB_CommandStatus_ERROR;
    }
}

static int32_t rpc_subghz_decode_raw_thread(void* context) {
    RpcSubGhz* rpc_subghz = context;
    RpcSubGhzDecodeCtx ctx = {
        .rpc_subghz = rpc_subghz,
        .command_id = rpc_subghz->decode_command_id,
    };
    const SubGhzDecodeRawCallbacks callbacks = {
        .should_stop = rpc_subghz_should_stop,
        .on_progress = rpc_subghz_on_progress,
        .on_hit = rpc_subghz_on_hit,
    };

    const SubGhzDecodeRawStatus status =
        subghz_decode_raw_file(rpc_subghz->decode_path, &callbacks, &ctx);

    if(!rpc_session_is_closed(rpc_subghz->session) && status != SubGhzDecodeRawStatusOk) {
        rpc_send_and_release_empty(
            rpc_subghz->session, ctx.command_id, rpc_subghz_map_decode_status(status));
    }

    free(rpc_subghz->decode_path);
    rpc_subghz->decode_path = NULL;
    rpc_subghz->decode_stop_requested = false;
    rpc_subghz->decoding = false;
    return 0;
}

static void rpc_subghz_decode_reap(RpcSubGhz* rpc_subghz) {
    if(!rpc_subghz->decode_thread) return;
    if(rpc_subghz->decoding) return;
    furi_thread_join(rpc_subghz->decode_thread);
    furi_thread_free(rpc_subghz->decode_thread);
    rpc_subghz->decode_thread = NULL;
}

static void rpc_subghz_decode_raw_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_subghz_decode_raw_request_tag);

    RpcSubGhz* rpc_subghz = context;
    RpcSession* session = rpc_subghz->session;
    furi_assert(session);

    rpc_subghz_decode_reap(rpc_subghz);

    const char* path = request->content.subghz_decode_raw_request.path;
    if(!path || path[0] == '\0' || !path_contains_only_ascii(path)) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    if(rpc_subghz->decoding || rpc_subghz->rx) {
        rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_ERROR_BUSY);
        return;
    }

    FURI_LOG_I(TAG, "Decode RAW %s", path);
    rpc_subghz->decode_path = strdup(path);
    rpc_subghz->decode_command_id = request->command_id;
    rpc_subghz->decode_stop_requested = false;
    rpc_subghz->decoding = true;

    rpc_subghz->decode_thread =
        furi_thread_alloc_ex("RpcSubGhzDecode", 6144, rpc_subghz_decode_raw_thread, rpc_subghz);
    furi_thread_start(rpc_subghz->decode_thread);
}

static void rpc_subghz_decode_raw_stop_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);

    RpcSubGhz* rpc_subghz = context;
    rpc_subghz->decode_stop_requested = true;
    rpc_send_and_release_empty(rpc_subghz->session, request->command_id, PB_CommandStatus_OK);
}

#define RPC_SUBGHZ_SETTING_FILE EXT_PATH("subghz/assets/setting_user")

static void rpc_subghz_settings_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);

    RpcSubGhz* rpc_subghz = context;
    RpcSession* session = rpc_subghz->session;

    SubGhzSetting* setting = subghz_setting_alloc();
    subghz_setting_load(setting, RPC_SUBGHZ_SETTING_FILE);

    PB_Main* response = rpc_message_alloc();
    response->command_id = request->command_id;
    response->command_status = PB_CommandStatus_OK;
    response->has_next = false;
    response->which_content = PB_Main_subghz_settings_response_tag;
    PB_SubGhz_SettingsResponse* payload = &response->content.subghz_settings_response;

    const size_t frequencies = subghz_setting_get_frequency_count(setting);
    if(frequencies) {
        payload->frequencies = malloc(sizeof(uint32_t) * frequencies);
        for(size_t i = 0; i < frequencies; i++) {
            payload->frequencies[i] = subghz_setting_get_frequency(setting, i);
        }
        payload->frequencies_count = frequencies;
    }

    const size_t hoppers = subghz_setting_get_hopper_frequency_count(setting);
    if(hoppers) {
        payload->hopper_frequencies = malloc(sizeof(uint32_t) * hoppers);
        for(size_t i = 0; i < hoppers; i++) {
            payload->hopper_frequencies[i] = subghz_setting_get_hopper_frequency(setting, i);
        }
        payload->hopper_frequencies_count = hoppers;
    }

    const size_t presets = subghz_setting_get_preset_count(setting);
    if(presets) {
        payload->presets = malloc(sizeof(char*) * presets);
        for(size_t i = 0; i < presets; i++) {
            const char* name = subghz_setting_get_preset_name(setting, i);
            payload->presets[i] = strdup(name ? name : "");
        }
        payload->presets_count = presets;
    }

    payload->default_frequency = subghz_setting_get_default_frequency(setting);

    subghz_setting_free(setting);
    rpc_send_and_release(session, response);
    free(response);
}

static PB_Main* rpc_subghz_rx_event_alloc(PB_SubGhz_RxEventType type) {
    PB_Main* event = rpc_message_alloc();
    event->command_id = 0;
    event->command_status = PB_CommandStatus_OK;
    event->has_next = false;
    event->which_content = PB_Main_subghz_rx_event_tag;
    event->content.subghz_rx_event.type = type;
    return event;
}

static void rpc_subghz_rx_on_rssi(
    const int8_t* samples,
    size_t count,
    uint32_t frequency,
    int32_t noise_floor,
    void* context) {
    RpcSubGhz* rpc_subghz = context;
    if(rpc_session_is_closed(rpc_subghz->session)) return;

    PB_Main* event = rpc_subghz_rx_event_alloc(PB_SubGhz_RxEventType_RX_EVENT_RSSI);
    pb_bytes_array_t* array = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(count));
    array->size = count;
    memcpy(array->bytes, samples, count);
    event->content.subghz_rx_event.rssi_samples = array;
    event->content.subghz_rx_event.frequency = frequency;
    event->content.subghz_rx_event.noise_floor = noise_floor;
    rpc_send_and_release(rpc_subghz->session, event);
    free(event);
}

static void rpc_subghz_rx_on_hit(const SubGhzRxHit* hit, void* context) {
    RpcSubGhz* rpc_subghz = context;
    if(rpc_session_is_closed(rpc_subghz->session)) return;

    PB_Main* event = rpc_subghz_rx_event_alloc(PB_SubGhz_RxEventType_RX_EVENT_HIT);
    PB_SubGhz_RxEvent* payload = &event->content.subghz_rx_event;
    payload->protocol = hit->protocol ? strdup(hit->protocol) : NULL;
    payload->flipper_format = hit->flipper_format ? strdup(hit->flipper_format) : NULL;
    payload->frequency = hit->frequency;
    payload->rssi = hit->rssi;
    payload->timestamp_ms = hit->timestamp_ms;
    payload->transmittable = hit->transmittable;

    if(hit->pulses && hit->pulse_count > 0) {
        const size_t count = hit->pulse_count > SUBGHZ_RX_PULSE_CAPTURE_MAX ?
                                 SUBGHZ_RX_PULSE_CAPTURE_MAX :
                                 hit->pulse_count;
        payload->pulses = malloc(sizeof(uint32_t) * count);
        memcpy(payload->pulses, hit->pulses, sizeof(uint32_t) * count);
        payload->pulses_count = count;
    }

    rpc_send_and_release(rpc_subghz->session, event);
    free(event);
}

static void rpc_subghz_rx_on_stopped(void* context) {
    RpcSubGhz* rpc_subghz = context;
    if(rpc_session_is_closed(rpc_subghz->session)) return;

    PB_Main* event = rpc_subghz_rx_event_alloc(PB_SubGhz_RxEventType_RX_EVENT_STOPPED);
    rpc_send_and_release(rpc_subghz->session, event);
    free(event);
}

static const SubGhzRxSessionCallbacks rpc_subghz_rx_callbacks = {
    .on_rssi = rpc_subghz_rx_on_rssi,
    .on_hit = rpc_subghz_rx_on_hit,
    .on_stopped = rpc_subghz_rx_on_stopped,
};

static PB_CommandStatus rpc_subghz_map_rx_status(SubGhzRxSessionStatus status) {
    switch(status) {
    case SubGhzRxSessionStatusOk:
        return PB_CommandStatus_OK;
    case SubGhzRxSessionStatusErrorBusy:
        return PB_CommandStatus_ERROR_BUSY;
    case SubGhzRxSessionStatusErrorInvalid:
    case SubGhzRxSessionStatusErrorTxForbidden:
        return PB_CommandStatus_ERROR_INVALID_PARAMETERS;
    case SubGhzRxSessionStatusErrorNoDevice:
        return PB_CommandStatus_ERROR_NOT_IMPLEMENTED;
    case SubGhzRxSessionStatusErrorInternal:
    default:
        return PB_CommandStatus_ERROR;
    }
}

static void
    rpc_subghz_send_rx_state(RpcSubGhz* rpc_subghz, uint32_t command_id, SubGhzRxSession* rx) {
    SubGhzRxState state = {0};
    if(rx) subghz_rx_session_get_state(rx, &state);

    PB_Main* response = rpc_message_alloc();
    response->command_id = command_id;
    response->command_status = PB_CommandStatus_OK;
    response->has_next = false;
    response->which_content = PB_Main_subghz_rx_state_response_tag;
    response->content.subghz_rx_state_response.running = state.running;
    response->content.subghz_rx_state_response.frequency = state.frequency;
    response->content.subghz_rx_state_response.preset = state.preset ? strdup(state.preset) : NULL;
    response->content.subghz_rx_state_response.hopping = state.hopping;
    response->content.subghz_rx_state_response.device =
        state.external_device ? PB_SubGhz_RadioDevice_RADIO_DEVICE_EXTERNAL :
                                PB_SubGhz_RadioDevice_RADIO_DEVICE_INTERNAL;
    response->content.subghz_rx_state_response.tx_allowed = state.tx_allowed;
    response->content.subghz_rx_state_response.device_name =
        state.device_name ? strdup(state.device_name) : NULL;
    response->content.subghz_rx_state_response.ignore_filter = state.ignore_filter;
    response->content.subghz_rx_state_response.modulation_hopping = state.modulation_hopping;
    response->content.subghz_rx_state_response.preset_b = state.preset_b ? strdup(state.preset_b) :
                                                                           NULL;
    response->content.subghz_rx_state_response.sound = state.sound;
    rpc_send_and_release(rpc_subghz->session, response);
    free(response);
}

static void rpc_subghz_rx_start_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_subghz_rx_start_request_tag);

    RpcSubGhz* rpc_subghz = context;
    RpcSession* session = rpc_subghz->session;

    if(rpc_subghz->decoding) {
        rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_ERROR_BUSY);
        return;
    }
    if(rpc_subghz->rx) {
        rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_ERROR_BUSY);
        return;
    }

    const PB_SubGhz_RxStartRequest* req = &request->content.subghz_rx_start_request;
    const SubGhzRxConfig config = {
        .frequency = req->frequency,
        .preset = req->preset,
        .rssi_interval_ms = req->rssi_interval_ms,
        .hopping = req->hopping,
        .hop_frequencies = req->hop_frequencies,
        .hop_count = req->hop_frequencies_count,
        .external_device = req->device == PB_SubGhz_RadioDevice_RADIO_DEVICE_EXTERNAL,
        .auto_device = req->device == PB_SubGhz_RadioDevice_RADIO_DEVICE_AUTO,
        .capture_pulses = req->capture_pulses,
        .ignore_filter = req->ignore_filter,
        .modulation_hopping = req->modulation_hopping,
        .preset_b = req->preset_b,
        .sound = req->sound,
    };

    SubGhzRxSession* rx = subghz_rx_session_alloc(&rpc_subghz_rx_callbacks, rpc_subghz);
    const SubGhzRxSessionStatus status = subghz_rx_session_start(rx, &config);
    if(status != SubGhzRxSessionStatusOk) {
        subghz_rx_session_free(rx);
        rpc_send_and_release_empty(session, request->command_id, rpc_subghz_map_rx_status(status));
        return;
    }

    rpc_subghz->rx = rx;
    rpc_subghz_send_rx_state(rpc_subghz, request->command_id, rx);
}

static void rpc_subghz_rx_stop_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);

    RpcSubGhz* rpc_subghz = context;
    if(!rpc_subghz->rx) {
        rpc_send_and_release_empty(
            rpc_subghz->session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    SubGhzRxSession* rx = rpc_subghz->rx;
    rpc_subghz->rx = NULL;
    subghz_rx_session_free(rx);
    rpc_send_and_release_empty(rpc_subghz->session, request->command_id, PB_CommandStatus_OK);
}

static void rpc_subghz_rx_config_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_subghz_rx_config_request_tag);

    RpcSubGhz* rpc_subghz = context;
    RpcSession* session = rpc_subghz->session;
    if(!rpc_subghz->rx) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    const PB_SubGhz_RxConfigRequest* req = &request->content.subghz_rx_config_request;
    const SubGhzRxConfig config = {
        .frequency = req->frequency,
        .preset = req->preset,
        .hopping = req->hopping,
        .hop_frequencies = req->hop_frequencies,
        .hop_count = req->hop_frequencies_count,
        .capture_pulses = req->capture_pulses,
        .ignore_filter = req->ignore_filter,
        .modulation_hopping = req->modulation_hopping,
        .preset_b = req->preset_b,
        .sound = req->sound,
    };

    const SubGhzRxSessionStatus status = subghz_rx_session_reconfigure(rpc_subghz->rx, &config);
    if(status != SubGhzRxSessionStatusOk) {
        rpc_send_and_release_empty(session, request->command_id, rpc_subghz_map_rx_status(status));
        return;
    }
    rpc_subghz_send_rx_state(rpc_subghz, request->command_id, rpc_subghz->rx);
}

static FlipperFormat* rpc_subghz_tx_load(const PB_SubGhz_TxRequest* req) {
    FlipperFormat* format = flipper_format_string_alloc();
    Stream* stream = flipper_format_get_raw_stream(format);

    if(req->flipper_format && req->flipper_format[0] != '\0') {
        stream_clean(stream);
        stream_write_cstring(stream, req->flipper_format);
        return format;
    }

    if(!req->path || req->path[0] == '\0' || !path_contains_only_ascii(req->path)) {
        flipper_format_free(format);
        return NULL;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool ok = false;
    if(flipper_format_file_open_existing(file, req->path)) {
        Stream* src = flipper_format_get_raw_stream(file);
        stream_seek(src, 0, StreamOffsetFromStart);
        stream_clean(stream);
        ok = stream_copy(src, stream, stream_size(src)) > 0;
    }
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    if(!ok) {
        flipper_format_free(format);
        return NULL;
    }
    return format;
}

static void rpc_subghz_tx_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_subghz_tx_request_tag);

    RpcSubGhz* rpc_subghz = context;
    RpcSession* session = rpc_subghz->session;

    if(rpc_subghz->decoding) {
        rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_ERROR_BUSY);
        return;
    }

    const PB_SubGhz_TxRequest* req = &request->content.subghz_tx_request;
    FlipperFormat* format = rpc_subghz_tx_load(req);
    if(!format) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    SubGhzRxSession* rx = rpc_subghz->rx;
    const bool standalone = rx == NULL;
    if(standalone) rx = subghz_rx_session_alloc(&rpc_subghz_rx_callbacks, rpc_subghz);

    uint32_t frequency = 0;
    const SubGhzRxSessionStatus status = subghz_rx_session_transmit(
        rx,
        format,
        req->repeat,
        req->device == PB_SubGhz_RadioDevice_RADIO_DEVICE_EXTERNAL,
        &frequency);

    if(standalone) subghz_rx_session_free(rx);
    flipper_format_free(format);

    if(status != SubGhzRxSessionStatusOk) {
        rpc_send_and_release_empty(session, request->command_id, rpc_subghz_map_rx_status(status));
        return;
    }

    PB_Main* response = rpc_message_alloc();
    response->command_id = request->command_id;
    response->command_status = PB_CommandStatus_OK;
    response->has_next = false;
    response->which_content = PB_Main_subghz_tx_response_tag;
    response->content.subghz_tx_response.repeat = req->repeat ? req->repeat : 3;
    response->content.subghz_tx_response.frequency = frequency;
    rpc_send_and_release(session, response);
    free(response);
}

void* rpc_subghz_alloc(RpcSession* session) {
    furi_assert(session);

    RpcSubGhz* rpc_subghz = malloc(sizeof(RpcSubGhz));
    memset(rpc_subghz, 0, sizeof(RpcSubGhz));
    rpc_subghz->session = session;

    RpcHandler rpc_handler = {
        .message_handler = NULL,
        .decode_submessage = NULL,
        .context = rpc_subghz,
    };

    rpc_handler.message_handler = rpc_subghz_decode_raw_process;
    rpc_add_handler(session, PB_Main_subghz_decode_raw_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_subghz_decode_raw_stop_process;
    rpc_add_handler(session, PB_Main_subghz_decode_raw_stop_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_subghz_rx_start_process;
    rpc_add_handler(session, PB_Main_subghz_rx_start_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_subghz_rx_stop_process;
    rpc_add_handler(session, PB_Main_subghz_rx_stop_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_subghz_rx_config_process;
    rpc_add_handler(session, PB_Main_subghz_rx_config_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_subghz_tx_process;
    rpc_add_handler(session, PB_Main_subghz_tx_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_subghz_settings_process;
    rpc_add_handler(session, PB_Main_subghz_settings_request_tag, &rpc_handler);

    return rpc_subghz;
}

void rpc_subghz_free(void* context) {
    furi_assert(context);
    RpcSubGhz* rpc_subghz = context;

    if(rpc_subghz->rx) {
        subghz_rx_session_free(rpc_subghz->rx);
        rpc_subghz->rx = NULL;
    }

    if(rpc_subghz->decode_thread) {
        rpc_subghz->decode_stop_requested = true;
        furi_thread_join(rpc_subghz->decode_thread);
        furi_thread_free(rpc_subghz->decode_thread);
        rpc_subghz->decode_thread = NULL;
    }
    free(rpc_subghz->decode_path);
    free(rpc_subghz);
}
