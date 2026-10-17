#include "kia_v3_v4.h"
#include "aes_common.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/toolbox/manchester_decoder.h>
#include <furi_hal.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static SubGhzProtocolStatus
    kia_v3_v4_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
    if(!ff || !expected_name) return SubGhzProtocolStatusError;
    FuriString* tmp = furi_string_alloc();
    if(!tmp) return SubGhzProtocolStatusError;
    SubGhzProtocolStatus result = SubGhzProtocolStatusError;
    flipper_format_rewind(ff);
    if(!flipper_format_read_string(ff, "Protocol", tmp))
        result = SubGhzProtocolStatusErrorParserOthers;
    else if(subghz_protocol_names_match(furi_string_get_cstr(tmp), expected_name))
        result = SubGhzProtocolStatusOk;
    furi_string_free(tmp);
    return result;
}

static void kia_v3_v4_encoder_read_fields(
    FlipperFormat* ff,
    uint32_t* serial_out,
    uint32_t* btn_out,
    uint32_t* cnt_out,
    uint32_t* type_out) {
    if(!ff) return;
    uint32_t tmp = 0;
    if(serial_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Serial", &tmp, 1)) *serial_out = tmp;
    }
    if(btn_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Btn", &tmp, 1)) *btn_out = tmp;
    }
    if(cnt_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Cnt", &tmp, 1)) *cnt_out = tmp;
    }
    if(type_out) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Type", &tmp, 1)) *type_out = tmp;
    }
}

static uint32_t kia_v3_v4_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus kia_v3_v4_encoder_read_bit(
    FlipperFormat* ff,
    const uint16_t* allowed_bits,
    size_t allowed_bits_count,
    uint32_t* out_bit) {
    if(!ff || !out_bit) return SubGhzProtocolStatusError;
    flipper_format_rewind(ff);
    uint32_t bit = 0;
    if(!flipper_format_read_uint32(ff, "Bit", &bit, 1))
        return SubGhzProtocolStatusErrorValueBitCount;
    if(allowed_bits && allowed_bits_count) {
        bool ok = false;
        for(size_t i = 0; i < allowed_bits_count; i++) {
            if((uint32_t)allowed_bits[i] == bit) {
                ok = true;
                break;
            }
        }
        if(!ok) return SubGhzProtocolStatusError;
    }
    *out_bit = bit;
    return SubGhzProtocolStatusOk;
}

static SubGhzProtocolStatus kia_v3_v4_serialize_fields(
    FlipperFormat* ff,
    bool write_serial,
    bool write_btn,
    bool write_cnt,
    bool write_type,
    uint32_t serial,
    uint32_t btn,
    uint32_t cnt,
    uint32_t type) {
    if(!ff) return SubGhzProtocolStatusError;
    if(write_serial && !flipper_format_write_uint32(ff, "Serial", &serial, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    if(write_btn && !flipper_format_write_uint32(ff, "Btn", &btn, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    if(write_cnt && !flipper_format_write_uint32(ff, "Cnt", &cnt, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    if(write_type && !flipper_format_write_uint32(ff, "Type", &type, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

static void kia_v3_v4_get_string_setup_editor(
    uint8_t btn,
    uint32_t cnt,
    uint8_t btn_bits,
    uint8_t cnt_bits,
    uint8_t max_btns) {
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = btn_bits;
    subghz_block_generic_global.cnt_length_bit = cnt_bits;
    subghz_block_generic_global.current_btn = btn;
    subghz_block_generic_global.current_cnt = cnt;
    if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original(btn);
    subghz_custom_btn_set_max(max_btns);
}

static inline size_t
    kia_v3_v4_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static inline ManchesterEvent
    kia_v3_v4_manchester_event(uint32_t duration, bool level, const SubGhzBlockConst* t) {
    if(DURATION_DIFF(duration, t->te_short) < t->te_delta) {
        return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    }
    if(DURATION_DIFF(duration, t->te_long) < t->te_delta) {
        return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }
    return ManchesterEventReset;
}

static uint8_t kia_v3_v4_reverse_bits8(uint8_t value) {
    reverse_bits_in_bytes(&value, 1);
    return value;
}

static void kia_v3_v4_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t kia_v3_v4_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static SubGhzProtocolStatus
    kia_v3_v4_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
    if(!ff || !protocol_name || !suffix) return SubGhzProtocolStatusError;
    FuriString* display = furi_string_alloc();
    if(!display) return SubGhzProtocolStatusError;
    furi_string_printf(display, "%s - %s", protocol_name, suffix);
    SubGhzProtocolStatus status =
        flipper_format_write_string_cstr(ff, "Disp", furi_string_get_cstr(display)) ?
            SubGhzProtocolStatusOk :
            SubGhzProtocolStatusErrorParserOthers;
    furi_string_free(display);
    return status;
}

#pragma GCC diagnostic pop

static const char* kia_v3_v4_get_button_name(uint8_t btn) {
    switch(btn) {
    case 0x1:
        return "Close";
    case 0x2:
        return "Open";
    case 0x3:
        return "Boot";
    default:
        return NULL;
    }
}

#include "keeloq_common.h"
#include "keys.h"
#include <lib/subghz/blocks/math.h>

#define TAG "KiaV3V4"

static const char* kia_version_names[] = {"Kia V4", "Kia V3"};

#define KIA_V3_V4_PREAMBLE_PAIRS  12U
#define KIA_V3_V4_BIT_COUNT       64U
#define KIA_V3_V4_CRC_BIT_COUNT   4U
#define KIA_V3_V4_CRC_SWEEP_COUNT 16U
#define KIA_V3_V4_SYNC_DURATION   1200U
#define KIA_V3_V4_END_MARKER_US   800U
#define KIA_V3_V4_DEFAULT_REPEAT  KIA_V3_V4_CRC_SWEEP_COUNT

#define KIA_V3_V4_DATA_OFFSET   ((KIA_V3_V4_PREAMBLE_PAIRS * 2U) + 2U) // 26
#define KIA_V3_V4_CRC_OFFSET    (KIA_V3_V4_DATA_OFFSET + (KIA_V3_V4_BIT_COUNT * 2U))
#define KIA_V3_V4_END_OFFSET    (KIA_V3_V4_CRC_OFFSET + (KIA_V3_V4_CRC_BIT_COUNT * 2U))
#define KIA_V3_V4_BURST_ENTRIES (KIA_V3_V4_END_OFFSET + 2U)

#define KIA_V3_V4_UPLOAD_CAPACITY KIA_V3_V4_BURST_ENTRIES

static const SubGhzBlockConst subghz_protocol_kia_v3_v4_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 150,
    .min_count_bit_for_found = 68,
};

typedef struct SubGhzProtocolDecoderKiaV3V4 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    uint8_t raw_bits[32];
    uint16_t raw_bit_count;
    bool is_v3_sync;

    uint32_t encrypted;
    uint32_t decrypted;
    uint8_t crc;
    uint8_t version;
} SubGhzProtocolDecoderKiaV3V4;

typedef struct SubGhzProtocolEncoderKiaV3V4 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t serial;
    uint8_t btn;
    uint16_t cnt;
    uint8_t version;

    uint32_t encrypted;
    uint32_t decrypted;

    uint8_t crc_iter;
    uint8_t bursts_sent;
} SubGhzProtocolEncoderKiaV3V4;

typedef enum {
    KiaV3V4DecoderStepReset = 0,
    KiaV3V4DecoderStepCheckPreamble,
    KiaV3V4DecoderStepCollectRawBits,
} KiaV3V4DecoderStep;

static void kia_v3_v4_add_raw_bit(SubGhzProtocolDecoderKiaV3V4* instance, bool bit) {
    if(instance->raw_bit_count < 256) {
        uint16_t byte_idx = instance->raw_bit_count / 8;
        uint8_t bit_idx = 7 - (instance->raw_bit_count % 8);
        if(bit) {
            instance->raw_bits[byte_idx] |= (1 << bit_idx);
        } else {
            instance->raw_bits[byte_idx] &= ~(1 << bit_idx);
        }
        instance->raw_bit_count++;
    }
}

static uint8_t kia_v3_v4_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x01;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x02;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x03;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x04;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x08;
    default:
        return subghz_custom_btn_get_original();
    }
}

static inline void kia_v3_v4_emit_bit_pwm(LevelDuration* upload, size_t* idx, bool bit, bool v4) {
    const uint32_t te_short = subghz_protocol_kia_v3_v4_const.te_short;
    const uint32_t te_long = subghz_protocol_kia_v3_v4_const.te_long;
    const uint32_t first_us = bit ? te_short : te_long;
    const uint32_t second_us = bit ? te_long : te_short;

    if(v4) {
        upload[(*idx)++] = level_duration_make(false, (int32_t)first_us);
        upload[(*idx)++] = level_duration_make(true, (int32_t)second_us);
    } else {
        upload[(*idx)++] = level_duration_make(true, (int32_t)first_us);
        upload[(*idx)++] = level_duration_make(false, (int32_t)second_us);
    }
}
static uint64_t kia_v3_v4_build_tx_bitstream(SubGhzProtocolEncoderKiaV3V4* instance) {
    const uint32_t serial_btn = (instance->serial & 0x0FFFFFFFU) |
                                ((uint32_t)(instance->btn & 0x0FU) << 28);
    const uint64_t key = ((uint64_t)serial_btn << 32) | (uint64_t)instance->encrypted;
    return subghz_protocol_blocks_reverse_key(key, 64);
}

static bool kia_v3_v4_process_buffer(SubGhzProtocolDecoderKiaV3V4* instance) {
    if(instance->raw_bit_count < subghz_protocol_kia_v3_v4_const.min_count_bit_for_found) {
        return false;
    }

    uint8_t* b = instance->raw_bits;

    if(instance->is_v3_sync) {
        uint16_t num_bytes = (instance->raw_bit_count + 7) / 8;
        for(uint16_t i = 0; i < num_bytes; i++) {
            b[i] = ~b[i];
        }
    }

    uint8_t crc = (b[8] >> 4) & 0x0F;

    uint32_t encrypted = ((uint32_t)kia_v3_v4_reverse_bits8(b[3]) << 24) |
                         ((uint32_t)kia_v3_v4_reverse_bits8(b[2]) << 16) |
                         ((uint32_t)kia_v3_v4_reverse_bits8(b[1]) << 8) |
                         (uint32_t)kia_v3_v4_reverse_bits8(b[0]);

    uint32_t serial = ((uint32_t)kia_v3_v4_reverse_bits8(b[7] & 0xF0) << 24) |
                      ((uint32_t)kia_v3_v4_reverse_bits8(b[6]) << 16) |
                      ((uint32_t)kia_v3_v4_reverse_bits8(b[5]) << 8) |
                      (uint32_t)kia_v3_v4_reverse_bits8(b[4]);

    uint8_t btn = (kia_v3_v4_reverse_bits8(b[7]) & 0xF0) >> 4;
    uint8_t our_serial_lsb = serial & 0xFF;

    uint32_t decrypted = subghz_protocol_keeloq_common_decrypt(encrypted, get_kia_mf_key());
    uint8_t dec_btn = (decrypted >> 28) & 0x0F;
    uint8_t dec_serial_lsb = (decrypted >> 16) & 0xFF;

    if(dec_btn != btn || dec_serial_lsb != our_serial_lsb) {
        return false;
    }

    instance->encrypted = encrypted;
    instance->decrypted = decrypted;
    instance->crc = crc;
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = decrypted & 0xFFFF;
    instance->version = instance->is_v3_sync ? 1 : 0;

    uint64_t key_data = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) |
                        ((uint64_t)b[3] << 32) | ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                        ((uint64_t)b[6] << 8) | (uint64_t)b[7];
    instance->generic.data = key_data;
    instance->generic.data_count_bit = 68;

    return true;
}

void subghz_protocol_decoder_kia_v3_v4_free(void* context) {
    furi_assert(context);
    free(context);
}

uint8_t subghz_protocol_decoder_kia_v3_v4_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_encoder_kia_v3_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    free(instance->encoder.upload);
    free(instance);
}
const SubGhzProtocolDecoder subghz_protocol_kia_v3_v4_decoder = {
    .alloc = subghz_protocol_decoder_kia_v3_v4_alloc,
    .free = subghz_protocol_decoder_kia_v3_v4_free,
    .feed = subghz_protocol_decoder_kia_v3_v4_feed,
    .reset = subghz_protocol_decoder_kia_v3_v4_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v3_v4_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v3_v4_serialize,
    .deserialize = subghz_protocol_decoder_kia_v3_v4_deserialize,
    .get_string = subghz_protocol_decoder_kia_v3_v4_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v3_v4_encoder = {
    .alloc = subghz_protocol_encoder_kia_v3_v4_alloc,
    .free = subghz_protocol_encoder_kia_v3_v4_free,
    .deserialize = subghz_protocol_encoder_kia_v3_v4_deserialize,
    .stop = subghz_protocol_encoder_kia_v3_v4_stop,
    .yield = subghz_protocol_encoder_kia_v3_v4_yield,
};

const SubGhzProtocol subghz_protocol_kia_v3_v4 = {
    .name = SUBGHZ_PROTOCOL_KIA_V3_V4_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_v3_v4_decoder,
    .encoder = &subghz_protocol_kia_v3_v4_encoder,
};

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================
void* subghz_protocol_encoder_kia_v3_v4_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolEncoderKiaV3V4* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV3V4));
    furi_check(instance);

    if(environment) {
        kia_keys_load(environment);
    }

    instance->base.protocol = &subghz_protocol_kia_v3_v4;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->serial = 0;
    instance->btn = 0;
    instance->cnt = 0;
    instance->version = 0;
    instance->crc_iter = 0;
    instance->bursts_sent = 0;

    instance->encoder.size_upload = KIA_V3_V4_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = (int32_t)KIA_V3_V4_DEFAULT_REPEAT;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;

    FURI_LOG_I(TAG, "Encoder allocated at %p", instance);
    return instance;
}

static void subghz_protocol_encoder_kia_v3_v4_build_packet(
    SubGhzProtocolEncoderKiaV3V4* instance,
    uint8_t* raw_bytes) {
    uint32_t plaintext = (uint32_t)(instance->cnt & 0xFFFFU) |
                         ((uint32_t)(instance->serial & 0x3FFU) << 16) |
                         ((uint32_t)(instance->btn & 0x0FU) << 28);

    instance->decrypted = plaintext;

    uint32_t encrypted = subghz_protocol_keeloq_common_encrypt(plaintext, get_kia_mf_key());
    instance->encrypted = encrypted;

    FURI_LOG_I(
        TAG,
        "Encrypt: plain=0x%08lX -> enc=0x%08lX",
        (unsigned long)plaintext,
        (unsigned long)encrypted);

    raw_bytes[0] = kia_v3_v4_reverse_bits8((encrypted >> 0) & 0xFF);
    raw_bytes[1] = kia_v3_v4_reverse_bits8((encrypted >> 8) & 0xFF);
    raw_bytes[2] = kia_v3_v4_reverse_bits8((encrypted >> 16) & 0xFF);
    raw_bytes[3] = kia_v3_v4_reverse_bits8((encrypted >> 24) & 0xFF);

    uint32_t serial_btn = (instance->serial & 0x0FFFFFFFU) |
                          ((uint32_t)(instance->btn & 0x0F) << 28);
    raw_bytes[4] = kia_v3_v4_reverse_bits8((serial_btn >> 0) & 0xFF);
    raw_bytes[5] = kia_v3_v4_reverse_bits8((serial_btn >> 8) & 0xFF);
    raw_bytes[6] = kia_v3_v4_reverse_bits8((serial_btn >> 16) & 0xFF);
    raw_bytes[7] = kia_v3_v4_reverse_bits8((serial_btn >> 24) & 0xFF);

    FURI_LOG_I(
        TAG,
        "TX raw: %02X %02X %02X %02X %02X %02X %02X %02X",
        raw_bytes[0],
        raw_bytes[1],
        raw_bytes[2],
        raw_bytes[3],
        raw_bytes[4],
        raw_bytes[5],
        raw_bytes[6],
        raw_bytes[7]);

    instance->generic.data = ((uint64_t)raw_bytes[0] << 56) | ((uint64_t)raw_bytes[1] << 48) |
                             ((uint64_t)raw_bytes[2] << 40) | ((uint64_t)raw_bytes[3] << 32) |
                             ((uint64_t)raw_bytes[4] << 24) | ((uint64_t)raw_bytes[5] << 16) |
                             ((uint64_t)raw_bytes[6] << 8) | (uint64_t)raw_bytes[7];
    instance->generic.data_count_bit = 68;

    FURI_LOG_I(
        TAG,
        "Packet built: Serial=0x%07lX, Btn=0x%X, Cnt=0x%04X",
        (unsigned long)instance->serial,
        instance->btn,
        instance->cnt);
}
static void subghz_protocol_encoder_kia_v3_v4_patch_crc(SubGhzProtocolEncoderKiaV3V4* instance) {
    if(!instance || !instance->encoder.upload) return;
    const bool v4 = (instance->version == 0);
    const uint8_t crc = instance->crc_iter & 0x0FU;
    size_t idx = KIA_V3_V4_CRC_OFFSET;
    for(int b = 3; b >= 0; b--) {
        const bool bit = (crc >> b) & 1U;
        kia_v3_v4_emit_bit_pwm(instance->encoder.upload, &idx, bit, v4);
    }
}

static void subghz_protocol_encoder_kia_v3_v4_get_upload(SubGhzProtocolEncoderKiaV3V4* instance) {
    furi_check(instance);

    uint8_t raw_bytes[8];
    subghz_protocol_encoder_kia_v3_v4_build_packet(instance, raw_bytes);

    const bool v4 = (instance->version == 0);
    const uint64_t tx_key = kia_v3_v4_build_tx_bitstream(instance);

    size_t idx = 0;
    LevelDuration* upload = instance->encoder.upload;
    const uint32_t te_short = subghz_protocol_kia_v3_v4_const.te_short;

    for(uint32_t i = 0; i < KIA_V3_V4_PREAMBLE_PAIRS; i++) {
        if(v4) {
            upload[idx++] = level_duration_make(false, (int32_t)te_short);
            upload[idx++] = level_duration_make(true, (int32_t)te_short);
        } else {
            upload[idx++] = level_duration_make(true, (int32_t)te_short);
            upload[idx++] = level_duration_make(false, (int32_t)te_short);
        }
    }

    if(v4) {
        upload[idx++] = level_duration_make(false, (int32_t)te_short);
        upload[idx++] = level_duration_make(true, (int32_t)KIA_V3_V4_SYNC_DURATION);
    } else {
        upload[idx++] = level_duration_make(true, (int32_t)te_short);
        upload[idx++] = level_duration_make(false, (int32_t)KIA_V3_V4_SYNC_DURATION);
    }

    for(int i = 63; i >= 0; i--) {
        const bool bit = (tx_key >> i) & 1ULL;
        kia_v3_v4_emit_bit_pwm(upload, &idx, bit, v4);
    }

    const uint8_t crc = instance->crc_iter & 0x0FU;
    for(int b = 3; b >= 0; b--) {
        const bool bit = (crc >> b) & 1U;
        kia_v3_v4_emit_bit_pwm(upload, &idx, bit, v4);
    }

    if(v4) {
        upload[idx++] = level_duration_make(false, (int32_t)KIA_V3_V4_END_MARKER_US);
        upload[idx++] = level_duration_make(true, (int32_t)KIA_V3_V4_END_MARKER_US);
    } else {
        upload[idx++] = level_duration_make(true, (int32_t)KIA_V3_V4_END_MARKER_US);
        upload[idx++] = level_duration_make(false, (int32_t)KIA_V3_V4_END_MARKER_US);
    }

    furi_check(idx == KIA_V3_V4_BURST_ENTRIES);
    instance->encoder.size_upload = idx;
    instance->encoder.front = 0;

    FURI_LOG_I(
        TAG,
        "Upload built: size=%zu %s enc=0x%08lX tx=0x%016llX crc=0x%X",
        instance->encoder.size_upload,
        v4 ? "V4" : "V3",
        (unsigned long)instance->encrypted,
        (unsigned long long)tx_key,
        crc);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    //instance->encoder.repeat = 40;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    flipper_format_rewind(flipper_format);

    do {
        FuriString* temp_str = furi_string_alloc();
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            furi_string_free(temp_str);
            break;
        }

        // Accept "Kia V3/V4", "Kia V3", or "Kia V4"
        const char* proto_str = furi_string_get_cstr(temp_str);
        if(!subghz_protocol_names_match(proto_str, instance->base.protocol->name) &&
           strcmp(proto_str, "Kia V3") != 0 && strcmp(proto_str, "Kia V4") != 0 &&
           strcmp(proto_str, "Korea v3") != 0 && strcmp(proto_str, "Korea v4") != 0) {
            FURI_LOG_E(TAG, "Wrong protocol %s", proto_str);
            furi_string_free(temp_str);
            break;
        }

        // Set version based on protocol name if specific
        bool version_from_protocol_name = false;

        if(strcmp(proto_str, "Kia V3") == 0 || strcmp(proto_str, "Korea v3") == 0) {
            instance->version = 1;
            version_from_protocol_name = true;
            FURI_LOG_I(TAG, "Protocol name indicates V3");
        } else if(strcmp(proto_str, "Kia V4") == 0 || strcmp(proto_str, "Korea v4") == 0) {
            instance->version = 0;
            version_from_protocol_name = true;
            FURI_LOG_I(TAG, "Protocol name indicates V4");
        }

        furi_string_free(temp_str);

        uint32_t bits = 0;
        if(kia_v3_v4_encoder_read_bit(flipper_format, NULL, 0, &bits) != SubGhzProtocolStatusOk)
            break;
        instance->generic.data_count_bit = 68;

        if(!subghz_block_generic_read_key_u64(flipper_format, &instance->generic.data)) {
            break;
        }

        uint32_t serial = UINT32_MAX;
        uint32_t btn = UINT32_MAX;
        uint32_t cnt = UINT32_MAX;
        kia_v3_v4_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        if(serial == UINT32_MAX || btn == UINT32_MAX || cnt == UINT32_MAX) {
            flipper_format_rewind(flipper_format);
            uint32_t temp = 0;
            if(flipper_format_read_uint32(flipper_format, "Encrypted", &temp, 1)) {
                instance->encrypted = temp;
            }
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "Decrypted", &temp, 1)) {
                instance->decrypted = temp;
            }
            if(instance->encrypted == 0 && instance->generic.data) {
                const uint64_t key =
                    subghz_protocol_blocks_reverse_key(instance->generic.data, 64);
                instance->encrypted = (uint32_t)(key & 0xFFFFFFFFU);
                const uint32_t serial_btn = (uint32_t)(key >> 32);
                if(serial == UINT32_MAX) {
                    serial = serial_btn & 0x0FFFFFFFU;
                }
                if(btn == UINT32_MAX) {
                    btn = (serial_btn >> 28) & 0x0FU;
                }
            }
            if(instance->decrypted == 0 && instance->encrypted) {
                instance->decrypted =
                    subghz_protocol_keeloq_common_decrypt(instance->encrypted, get_kia_mf_key());
            }
            if(cnt == UINT32_MAX && instance->decrypted) {
                cnt = instance->decrypted & 0xFFFFU;
            }
            if(btn == UINT32_MAX && instance->decrypted) {
                btn = (instance->decrypted >> 28) & 0x0FU;
            }
        }

        if(!subghz_custom_btn_has_original()) {
            subghz_custom_btn_set_original((uint8_t)btn);
        }
        btn = kia_v3_v4_get_btn_code();
        subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        if(!subghz_block_generic_global_counter_override_get(&cnt)) {
            cnt += furi_hal_subghz_get_rolling_counter_mult();
        }
        if(serial == 0 || btn == UINT32_MAX || cnt == UINT32_MAX) break;
        instance->serial = serial;
        instance->btn = (uint8_t)btn;
        instance->cnt = (uint16_t)cnt;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->btn;
        instance->generic.cnt = instance->cnt;

        // Read version - ONLY use file version if protocol name didn't specify one
        flipper_format_rewind(flipper_format);
        uint32_t version_temp;
        if(flipper_format_read_uint32(flipper_format, "KIAVersion", &version_temp, 1)) {
            if(!version_from_protocol_name) {
                instance->version = (uint8_t)version_temp;
            }
        } else if(!version_from_protocol_name) {
            instance->version = 0;
        }

        instance->encoder.repeat =
            (int32_t)kia_v3_v4_encoder_read_repeat(flipper_format, KIA_V3_V4_DEFAULT_REPEAT);

        instance->crc_iter = 0;
        instance->bursts_sent = 0;

        subghz_protocol_encoder_kia_v3_v4_get_upload(instance);

        flipper_format_rewind(flipper_format);
        char key_out[20];
        snprintf(key_out, sizeof(key_out), "%016llX", (unsigned long long)instance->generic.data);
        flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key_out);
        uint32_t enc_w = instance->encrypted;
        flipper_format_insert_or_update_uint32(flipper_format, "Encrypted", &enc_w, 1);
        uint32_t dec_w = instance->decrypted;
        flipper_format_insert_or_update_uint32(flipper_format, "Decrypted", &dec_w, 1);
        uint32_t serial_w = instance->serial;
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->btn;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->cnt;
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        uint32_t ver_w = instance->version;
        flipper_format_insert_or_update_uint32(flipper_format, "KIAVersion", &ver_w, 1);
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->btn;
        instance->generic.cnt = instance->cnt;
        subghz_block_generic_global.current_cnt = instance->cnt;

        instance->encoder.is_running = true;
        instance->encoder.front = 0;

        FURI_LOG_I(
            TAG,
            "Encoder initialized: Serial=0x%07lX, Btn=0x%X, Cnt=0x%04X, Version=%s",
            (unsigned long)instance->serial,
            instance->btn,
            instance->cnt,
            instance->version == 0 ? "V4" : "V3");

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_kia_v3_v4_stop(void* context) {
    if(!context) return;
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v3_v4_yield(void* context) {
    SubGhzProtocolEncoderKiaV3V4* instance = context;

    if(!instance || !instance->encoder.upload || instance->encoder.repeat == 0 ||
       !instance->encoder.is_running) {
        if(instance) {
            FURI_LOG_D(
                TAG,
                "Encoder yield stopped: repeat=%u, is_running=%d",
                instance->encoder.repeat,
                instance->encoder.is_running);
            instance->encoder.is_running = false;
        }
        return level_duration_reset();
    }

    if(instance->encoder.front >= instance->encoder.size_upload) {
        FURI_LOG_E(
            TAG,
            "Encoder front out of bounds: %zu >= %zu",
            instance->encoder.front,
            instance->encoder.size_upload);
        instance->encoder.is_running = false;
        instance->encoder.front = 0;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->crc_iter = (uint8_t)((instance->crc_iter + 1U) & 0x0FU);
        subghz_protocol_encoder_kia_v3_v4_patch_crc(instance);
        instance->encoder.front = 0;
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        if(instance->bursts_sent < KIA_V3_V4_CRC_SWEEP_COUNT) {
            instance->bursts_sent++;
        }
        if(instance->encoder.repeat == 0) {
            FURI_LOG_I(
                TAG,
                "CRC BF: %u/%u bursts transmitted (~%u ms)",
                instance->bursts_sent,
                KIA_V3_V4_CRC_SWEEP_COUNT,
                (unsigned)(instance->bursts_sent * 94U));
        }
    }

    return ret;
}

void subghz_protocol_encoder_kia_v3_v4_set_button(void* context, uint8_t button) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    instance->btn = button & 0x0F;
    instance->generic.btn = instance->btn;
    subghz_protocol_encoder_kia_v3_v4_get_upload(instance);
    FURI_LOG_I(TAG, "Button set to 0x%X", instance->btn);
}

void subghz_protocol_encoder_kia_v3_v4_set_counter(void* context, uint16_t counter) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    instance->cnt = counter;
    instance->generic.cnt = instance->cnt;
    subghz_protocol_encoder_kia_v3_v4_get_upload(instance);
    FURI_LOG_I(TAG, "Counter set to 0x%04X", instance->cnt);
}

void subghz_protocol_encoder_kia_v3_v4_increment_counter(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    instance->cnt++;
    instance->generic.cnt = instance->cnt;
    subghz_protocol_encoder_kia_v3_v4_get_upload(instance);
    FURI_LOG_I(TAG, "Counter incremented to 0x%04X", instance->cnt);
}

uint16_t subghz_protocol_encoder_kia_v3_v4_get_counter(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    return instance->cnt;
}

uint8_t subghz_protocol_encoder_kia_v3_v4_get_button(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV3V4* instance = context;
    return instance->btn;
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_kia_v3_v4_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderKiaV3V4* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV3V4));
    furi_check(instance);

    if(environment) {
        kia_keys_load(environment);
    }

    instance->base.protocol = &subghz_protocol_kia_v3_v4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v3_v4_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;
    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
    instance->header_count = 0;
    instance->raw_bit_count = 0;
    instance->crc = 0;
    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
}

void subghz_protocol_decoder_kia_v3_v4_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV3V4DecoderStepReset:
        if(level && (DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
                     subghz_protocol_kia_v3_v4_const.te_delta)) {
            instance->decoder.parser_step = KiaV3V4DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
        }
        break;

    case KiaV3V4DecoderStepCheckPreamble:
        if(level) {
            if(DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
               subghz_protocol_kia_v3_v4_const.te_delta) {
                instance->decoder.te_last = duration;
            } else if(duration > 1000 && duration < 1500) {
                if(instance->header_count >= 8) {
                    instance->decoder.parser_step = KiaV3V4DecoderStepCollectRawBits;
                    instance->raw_bit_count = 0;
                    instance->is_v3_sync = false;
                    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
                } else {
                    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
                }
            } else {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        } else {
            if(duration > 1000 && duration < 1500) {
                if(instance->header_count >= 8) {
                    instance->decoder.parser_step = KiaV3V4DecoderStepCollectRawBits;
                    instance->raw_bit_count = 0;
                    instance->is_v3_sync = true;
                    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
                } else {
                    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
                }
            } else if(
                (DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
                 subghz_protocol_kia_v3_v4_const.te_delta) &&
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_kia_v3_v4_const.te_short) <
                 subghz_protocol_kia_v3_v4_const.te_delta)) {
                instance->header_count++;
            } else if(duration > 1500) {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        break;

    case KiaV3V4DecoderStepCollectRawBits:
        if(level) {
            if(duration > 1000 && duration < 1500) {
                if(kia_v3_v4_process_buffer(instance)) {
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_short) <
                subghz_protocol_kia_v3_v4_const.te_delta) {
                kia_v3_v4_add_raw_bit(instance, false);
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v3_v4_const.te_long) <
                subghz_protocol_kia_v3_v4_const.te_delta) {
                kia_v3_v4_add_raw_bit(instance, true);
            } else {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        } else {
            if(duration > 1000 && duration < 1500) {
                if(kia_v3_v4_process_buffer(instance)) {
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            } else if(duration > 1500) {
                if(kia_v3_v4_process_buffer(instance)) {
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v3_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        if(!subghz_block_generic_serialize_preset(flipper_format, preset)) break;

        const char* version_name = (instance->version == 0) ? "Kia V4" : "Kia V3";
        if(!flipper_format_write_string_cstr(
               flipper_format, "Protocol", instance->base.protocol->name)) {
            break;
        }

        // Write bit count
        uint32_t bits = instance->generic.data_count_bit;
        if(!flipper_format_write_uint32(flipper_format, "Bit", &bits, 1)) {
            break;
        }

        // Write key
        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%016llX", (unsigned long long)instance->generic.data);
        if(!flipper_format_write_string_cstr(flipper_format, "Key", key_str)) {
            break;
        }

        // Write all fields needed by encoder
        if(kia_v3_v4_serialize_fields(
               flipper_format,
               true,
               true,
               true,
               false,
               instance->generic.serial,
               instance->generic.btn,
               instance->generic.cnt,
               0) != SubGhzProtocolStatusOk) {
            break;
        }

        // Write protocol-specific fields
        if(!flipper_format_write_uint32(flipper_format, "Encrypted", &instance->encrypted, 1)) {
            break;
        }

        if(!flipper_format_write_uint32(flipper_format, "Decrypted", &instance->decrypted, 1)) {
            break;
        }

        uint32_t temp = instance->version;
        if(!flipper_format_write_uint32(flipper_format, "KIAVersion", &temp, 1)) {
            break;
        }

        temp = instance->crc;
        if(!flipper_format_write_uint32(flipper_format, "CRC", &temp, 1)) {
            break;
        }

        const char* button_name = kia_v3_v4_get_button_name(instance->generic.btn);
        ret = (button_name && button_name[0]) ?
                  kia_v3_v4_write_display(flipper_format, version_name, button_name) :
                  SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV3V4* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    if(instance->generic.data_count_bit !=
           subghz_protocol_kia_v3_v4_const.min_count_bit_for_found &&
       instance->generic.data_count_bit != KIA_V3_V4_BIT_COUNT) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    uint32_t temp = 0;

    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Encrypted", &temp, 1)) {
        instance->encrypted = temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Decrypted", &temp, 1)) {
        instance->decrypted = temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "KIAVersion", &temp, 1)) {
        instance->version = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "CRC", &temp, 1)) {
        instance->crc = (uint8_t)temp;
    }

    uint32_t serial = instance->generic.serial;
    uint32_t btn = instance->generic.btn;
    uint32_t cnt = instance->generic.cnt;
    flipper_format_rewind(flipper_format);
    kia_v3_v4_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
    instance->generic.serial = serial;
    instance->generic.btn = (uint8_t)btn;
    instance->generic.cnt = (uint16_t)cnt;

    return SubGhzProtocolStatusOk;
}

static uint64_t compute_yek(uint64_t key) {
    uint64_t yek = 0;
    for(int i = 0; i < 64; i++) {
        yek |= ((key >> i) & 1) << (63 - i);
    }
    return yek;
}

void subghz_protocol_decoder_kia_v3_v4_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV3V4* instance = context;
    kia_v3_v4_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 16, 5);

    const uint64_t yek = compute_yek(instance->generic.data);
    const uint32_t yek_hi = (uint32_t)(yek >> 32);
    const uint32_t yek_lo = (uint32_t)(yek & 0xFFFFFFFFU);

    furi_string_cat_printf(
        output,
        "%s %ubit\r\n",
        kia_version_names[instance->version],
        instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key:%08lX%08lX\r\n",
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFFU));
    furi_string_cat_printf(
        output, "Yek:%08lX%08lX\r\n", (unsigned long)yek_hi, (unsigned long)yek_lo);
    {
        const char* btn_name = kia_v3_v4_get_button_name(instance->generic.btn);
        if(btn_name && btn_name[0] != '\0') {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X [%s]\r\nCnt:%0*lX\r\n",
                (int)7,
                (unsigned long)instance->generic.serial,
                instance->generic.btn,
                btn_name,
                (int)4,
                (unsigned long)instance->generic.cnt);
        } else {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\nCnt:%0*lX\r\n",
                (int)7,
                (unsigned long)instance->generic.serial,
                instance->generic.btn,
                (int)4,
                (unsigned long)instance->generic.cnt);
        }
    }
    furi_string_cat_printf(output, "CRC:%01X\r\n", instance->crc);
}
