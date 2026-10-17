#include "subghz_decode_raw.h"

#include "environment.h"
#include "receiver.h"
#include "subghz_protocol_registry.h"
#include "subghz_raw_file_reader.h"
#include "types.h"
#include "protocols/base.h"

#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/stream.h>

#include <string.h>

#define TAG "SubGhzDecodeRaw"

#define SUBGHZ_DECODE_RAW_DURATION_MAX             1000000U
#define SUBGHZ_DECODE_RAW_SAMPLES_PER_YIELD        2048U
#define SUBGHZ_DECODE_RAW_MAX_SEEN                 64U
#define SUBGHZ_DECODE_RAW_BUCKETS                  100U
#define SUBGHZ_DECODE_RAW_SAMPLES_PER_BUCKET_CHECK 64U
#define SUBGHZ_DECODE_RAW_DEFAULT_FREQUENCY        433920000U
#define SUBGHZ_DECODE_RAW_DEFAULT_PRESET           "FuriHalSubGhzPresetOok650Async"

typedef struct {
    SubGhzRadioPreset preset;
    const SubGhzDecodeRawCallbacks* callbacks;
    void* context;
    FuriString* seen[SUBGHZ_DECODE_RAW_MAX_SEEN];
    size_t seen_count;
    size_t seen_next;
    uint32_t hit_count;
    uint8_t percent;
    uint32_t bucket_edges;
    SubGhzReceiver* receiver;
} SubGhzDecodeRawCtx;

static bool subghz_decode_raw_should_stop(const SubGhzDecodeRawCtx* ctx) {
    if(!ctx->callbacks || !ctx->callbacks->should_stop) return false;
    return ctx->callbacks->should_stop(ctx->context);
}

static void subghz_decode_raw_emit_progress(SubGhzDecodeRawCtx* ctx, uint8_t percent) {
    ctx->percent = percent;
    const uint32_t activity = ctx->bucket_edges;
    ctx->bucket_edges = 0;
    if(ctx->callbacks && ctx->callbacks->on_progress) {
        ctx->callbacks->on_progress(percent, ctx->hit_count, activity, ctx->context);
    }
}

static uint32_t subghz_decode_raw_slice(const SubGhzRawFileReader* reader) {
    if(reader->file_size <= reader->data_start_offset) return 0;
    const uint64_t span = reader->file_size - reader->data_start_offset;
    const uint64_t done = reader->stream_pos > reader->data_start_offset ?
                              reader->stream_pos - reader->data_start_offset :
                              0;
    return (uint32_t)(done * SUBGHZ_DECODE_RAW_BUCKETS / span);
}

static bool subghz_decode_raw_is_raw_protocol(const SubGhzProtocol* protocol) {
    if(!protocol) return true;
    if(protocol->type == SubGhzProtocolTypeRAW) return true;
    if(protocol->type == SubGhzProtocolTypeBinRAW) return true;
    if(protocol->name && strcmp(protocol->name, "RAW") == 0) return true;
    return false;
}

static bool subghz_decode_raw_seen(SubGhzDecodeRawCtx* ctx, const char* text) {
    for(size_t i = 0; i < ctx->seen_count; i++) {
        if(furi_string_cmp_str(ctx->seen[i], text) == 0) return true;
    }
    return false;
}

static void subghz_decode_raw_remember(SubGhzDecodeRawCtx* ctx, const char* text) {
    if(ctx->seen_count < SUBGHZ_DECODE_RAW_MAX_SEEN) {
        ctx->seen[ctx->seen_count++] = furi_string_alloc_set_str(text);
        return;
    }
    furi_string_set_str(ctx->seen[ctx->seen_next], text);
    ctx->seen_next = (ctx->seen_next + 1) % SUBGHZ_DECODE_RAW_MAX_SEEN;
}

static char* subghz_decode_raw_dump_format(FlipperFormat* format) {
    Stream* stream = flipper_format_get_raw_stream(format);
    if(!stream) return NULL;
    stream_seek(stream, 0, StreamOffsetFromStart);
    const size_t size = stream_size(stream);
    char* buf = malloc(size + 1);
    if(!buf) return NULL;
    const size_t read = stream_read(stream, (uint8_t*)buf, size);
    buf[read] = '\0';
    return buf;
}

static void subghz_decode_raw_on_rx(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    SubGhzDecodeRawCtx* ctx = context;
    if(subghz_decode_raw_should_stop(ctx)) {
        subghz_receiver_reset(receiver);
        return;
    }

    if(subghz_decode_raw_is_raw_protocol(decoder_base->protocol)) {
        subghz_receiver_reset(receiver);
        return;
    }

    FlipperFormat* format = flipper_format_string_alloc();
    char* serialized = NULL;
    do {
        if(subghz_protocol_decoder_base_serialize(decoder_base, format, &ctx->preset) !=
           SubGhzProtocolStatusOk) {
            break;
        }
        serialized = subghz_decode_raw_dump_format(format);
        if(!serialized || serialized[0] == '\0') break;
        if(subghz_decode_raw_seen(ctx, serialized)) break;

        ctx->hit_count++;
        subghz_decode_raw_remember(ctx, serialized);
        if(ctx->callbacks && ctx->callbacks->on_hit) {
            const char* protocol = decoder_base->protocol && decoder_base->protocol->name ?
                                       decoder_base->protocol->name :
                                       "";
            ctx->callbacks->on_hit(
                protocol, serialized, ctx->percent, ctx->hit_count, ctx->context);
        }
    } while(false);

    free(serialized);
    flipper_format_free(format);
    subghz_receiver_reset(receiver);
}

static SubGhzDecodeRawStatus
    subghz_decode_raw_load_preset(const char* path, SubGhzRadioPreset* preset) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info = {0};
    const FS_Error stat = storage_common_stat(storage, path, &info);
    if(stat == FSE_NOT_EXIST) {
        furi_record_close(RECORD_STORAGE);
        return SubGhzDecodeRawStatusErrorNotExist;
    }
    if(stat != FSE_OK) {
        furi_record_close(RECORD_STORAGE);
        return SubGhzDecodeRawStatusErrorInternal;
    }

    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* temp = furi_string_alloc();
    uint32_t version = 0;
    uint32_t frequency = SUBGHZ_DECODE_RAW_DEFAULT_FREQUENCY;
    SubGhzDecodeRawStatus status = SubGhzDecodeRawStatusErrorInvalid;

    do {
        if(!flipper_format_file_open_existing(file, path)) {
            if(storage_common_stat(storage, path, &info) == FSE_NOT_EXIST) {
                status = SubGhzDecodeRawStatusErrorNotExist;
            }
            break;
        }
        if(!flipper_format_read_header(file, temp, &version)) break;
        furi_string_trim(temp);
        if(furi_string_cmp_str(temp, SUBGHZ_RAW_FILE_TYPE) != 0 ||
           version != SUBGHZ_RAW_FILE_VERSION) {
            break;
        }
        if(flipper_format_read_uint32(file, "Frequency", &frequency, 1)) {
            preset->frequency = frequency;
        }
        flipper_format_rewind(file);
        if(flipper_format_read_string(file, "Preset", temp)) {
            furi_string_trim(temp);
            if(furi_string_size(temp) > 0) {
                furi_string_set(preset->name, temp);
            }
        }
        status = SubGhzDecodeRawStatusOk;
    } while(false);

    furi_string_free(temp);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return status;
}

static SubGhzEnvironment* subghz_decode_raw_environment_alloc(void) {
    SubGhzEnvironment* environment = subghz_environment_alloc();
    subghz_environment_load_keystore(environment, SUBGHZ_KEYSTORE_DIR_NAME);
    subghz_environment_load_keystore(environment, SUBGHZ_KEYSTORE_DIR_USER_NAME);
    subghz_environment_load_keystore(environment, SUBGHZ_KEYSTORE_KIA_DIR_NAME);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        environment, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(
        environment, SUBGHZ_NICE_FLOR_S_DIR_NAME);
    subghz_environment_set_protocol_registry(environment, &subghz_protocol_registry);
    return environment;
}

SubGhzDecodeRawStatus subghz_decode_raw_file(
    const char* path,
    const SubGhzDecodeRawCallbacks* callbacks,
    void* context) {
    if(!path || path[0] == '\0') return SubGhzDecodeRawStatusErrorInvalid;

    SubGhzDecodeRawCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.callbacks = callbacks;
    ctx.context = context;
    ctx.preset.name = furi_string_alloc_set_str(SUBGHZ_DECODE_RAW_DEFAULT_PRESET);
    ctx.preset.frequency = SUBGHZ_DECODE_RAW_DEFAULT_FREQUENCY;
    ctx.preset.data = NULL;
    ctx.preset.data_size = 0;

    SubGhzDecodeRawStatus status = subghz_decode_raw_load_preset(path, &ctx.preset);
    if(status != SubGhzDecodeRawStatusOk) {
        furi_string_free(ctx.preset.name);
        return status;
    }

    SubGhzRawFileReader* reader = subghz_raw_file_reader_alloc();
    if(!reader || !subghz_raw_file_reader_open(reader, path)) {
        if(reader) subghz_raw_file_reader_free(reader);
        furi_string_free(ctx.preset.name);
        return SubGhzDecodeRawStatusErrorInvalid;
    }

    SubGhzEnvironment* environment = subghz_decode_raw_environment_alloc();
    ctx.receiver = subghz_receiver_alloc_init(environment);
    subghz_receiver_set_filter(ctx.receiver, SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_rx_callback(ctx.receiver, subghz_decode_raw_on_rx, &ctx);

    subghz_decode_raw_emit_progress(&ctx, 0);

    uint32_t since_yield = 0;
    uint32_t last_bucket = 0;
    uint32_t since_bucket = 0;
    bool level = false;
    uint32_t duration = 0;
    status = SubGhzDecodeRawStatusOk;

    while(subghz_raw_file_reader_get_next(reader, &level, &duration)) {
        if(subghz_decode_raw_should_stop(&ctx)) {
            status = SubGhzDecodeRawStatusStopped;
            break;
        }
        if(duration > SUBGHZ_DECODE_RAW_DURATION_MAX) {
            duration = SUBGHZ_DECODE_RAW_DURATION_MAX;
        }
        subghz_receiver_decode(ctx.receiver, level, duration);
        ctx.bucket_edges++;

        since_yield++;
        since_bucket++;

        if(since_bucket >= SUBGHZ_DECODE_RAW_SAMPLES_PER_BUCKET_CHECK) {
            since_bucket = 0;
            const uint32_t slice = subghz_decode_raw_slice(reader);
            if(slice > last_bucket && slice < SUBGHZ_DECODE_RAW_BUCKETS) {
                last_bucket = slice;
                const uint8_t percent = subghz_raw_file_reader_get_progress(reader);
                subghz_decode_raw_emit_progress(&ctx, percent < 100 ? percent : 99);
            }
        }

        if(since_yield >= SUBGHZ_DECODE_RAW_SAMPLES_PER_YIELD) {
            since_yield = 0;
            furi_thread_yield();
        }
    }

    if(status == SubGhzDecodeRawStatusOk && !subghz_raw_file_reader_is_finished(reader) &&
       !subghz_decode_raw_should_stop(&ctx)) {
        FURI_LOG_E(TAG, "RAW read interrupted");
        status = SubGhzDecodeRawStatusErrorInternal;
    }

    if(status == SubGhzDecodeRawStatusOk && !subghz_decode_raw_should_stop(&ctx)) {
        subghz_decode_raw_emit_progress(&ctx, 100);
    } else if(status == SubGhzDecodeRawStatusStopped) {
    }

    subghz_receiver_free(ctx.receiver);
    subghz_environment_free(environment);
    subghz_raw_file_reader_free(reader);
    for(size_t i = 0; i < ctx.seen_count; i++) {
        furi_string_free(ctx.seen[i]);
    }
    furi_string_free(ctx.preset.name);
    return status;
}
