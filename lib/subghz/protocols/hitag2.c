#include "hitag2_i.h"
#include "hitag2_key_db.h"
#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/toolbox/manchester_decoder.h>
#include <furi_hal.h>
#include <stdlib.h>
#include <string.h>

#undef TAG

static size_t hitag2_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static size_t hitag2_emit_manchester(
    LevelDuration* upload,
    size_t index,
    size_t capacity,
    const uint8_t* raw,
    unsigned bits,
    uint32_t te,
    bool invert) {
    for(unsigned bit_index = 0; bit_index < bits; bit_index++) {
        bool level = ((raw[bit_index >> 3] >> (7 - (bit_index & 7))) & 1) != 0;
        level ^= invert;
        index = hitag2_emit(upload, index, capacity, level, te);
        index = hitag2_emit(upload, index, capacity, !level, te);
    }
    return index;
}

static void fiat_v1_get_string_setup_editor(
    uint8_t btn,
    uint32_t cnt,
    uint8_t btn_bits,
    uint8_t cnt_bits,
    uint8_t max_btns) {
    subghz_block_generic_global.current_btn = btn;
    subghz_block_generic_global.current_cnt = cnt;
    if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original(btn);
    if(subghz_block_generic_global.hitag2_replay_only) {
        subghz_block_generic_global.btn_is_available = false;
        subghz_block_generic_global.cnt_is_available = false;
        subghz_custom_btn_set_max(1);
        return;
    }
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = btn_bits;
    subghz_block_generic_global.cnt_length_bit = cnt_bits;
    subghz_custom_btn_set_max(max_btns);
}

#define TAG "FiatProtocolV1"

#define FIAT_V1_TE_SHORT          250U
#define FIAT_V1_TE_LONG           500U
#define FIAT_V1_TE_DELTA          100U
#define FIAT_V1_TE_B_SHORT        100U
#define FIAT_V1_TE_B_LONG         200U
#define FIAT_V1_TE_B_DELTA        50U
#define FIAT_V1_WIRE_BITS         104U
#define FIAT_V1_DEFAULT_TAIL_BITS 2U
#define FIAT_V1_TAIL_BITS_FIELD   "Tail Bits"
#define FIAT_V1_XOR_FIELD         "XOR"

#define FIAT_V1_TE_VARIANT_A       0U
#define FIAT_V1_TE_VARIANT_B       1U
#define FIAT_V1_DEFAULT_TE_VARIANT FIAT_V1_TE_VARIANT_B
#define FIAT_V1_ENC_LEAD_A_US      2033U
#define FIAT_V1_ENC_GAP_A_US       3252U
#define FIAT_V1_ENC_LEAD_B_US      850U
#define FIAT_V1_ENC_GAP_B_US       1230U
#define FIAT_V1_ENC_DEFAULT_REPEAT 6U
#define FIAT_V1_UPLOAD_CAPACITY    240U
static const SubGhzBlockConst subghz_protocol_fiat_v1_const = {
    .te_short = FIAT_V1_TE_SHORT,
    .te_long = FIAT_V1_TE_LONG,
    .te_delta = FIAT_V1_TE_DELTA,
    .min_count_bit_for_found = FIAT_V1_LOGICAL_BITS,
};

static const SubGhzBlockConst subghz_protocol_fiat_v1_const_b = {
    .te_short = FIAT_V1_TE_B_SHORT,
    .te_long = FIAT_V1_TE_B_LONG,
    .te_delta = FIAT_V1_TE_B_DELTA,
    .min_count_bit_for_found = FIAT_V1_LOGICAL_BITS,
};

static const SubGhzBlockConst* fiat_v1_variant_const(uint8_t variant) {
    return (variant == FIAT_V1_TE_VARIANT_A) ? &subghz_protocol_fiat_v1_const :
                                               &subghz_protocol_fiat_v1_const_b;
}

static uint32_t fiat_v1_enc_lead_us(uint8_t variant) {
    return (variant == FIAT_V1_TE_VARIANT_A) ? FIAT_V1_ENC_LEAD_A_US : FIAT_V1_ENC_LEAD_B_US;
}

static uint32_t fiat_v1_enc_gap_us(uint8_t variant) {
    return (variant == FIAT_V1_TE_VARIANT_A) ? FIAT_V1_ENC_GAP_A_US : FIAT_V1_ENC_GAP_B_US;
}

static uint32_t fiat_v1_enc_te_us(uint8_t variant) {
    return fiat_v1_variant_const(variant)->te_short;
}

struct SubGhzProtocolDecoderFiatV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockGeneric generic;

    uint8_t raw_data[FIAT_V1_WIRE_BYTES];
    uint8_t last_raw_data[FIAT_V1_WIRE_BYTES];
    bool last_raw_valid;

    uint32_t uid;
    uint32_t hop;
    uint8_t family;
    uint8_t tail_bits;
    uint8_t frame_xor;

    uint8_t hitag2_key[6];
    uint32_t hitag2_epoch;
    bool hitag2_key_valid;
    uint8_t te_variant;
};

struct SubGhzProtocolEncoderFiatV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t raw_data[FIAT_V1_WIRE_BYTES];
    uint8_t hitag2_key[6];
    uint32_t epoch;
    uint32_t hop;
    uint8_t tail_bits;
    uint8_t frame_xor;
    uint8_t te_variant;
};

static void fiat_v1_build_raw(
    uint8_t raw[FIAT_V1_WIRE_BYTES],
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t auth,
    uint8_t tail_bits);
static void fiat_v1_verify_hitag2_key(SubGhzProtocolDecoderFiatV1* instance);

void subghz_protocol_encoder_fiat_v1_free(void* context);
void subghz_protocol_encoder_fiat_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_fiat_v1_yield(void* context);

void subghz_protocol_decoder_fiat_v1_free(void* context);

const SubGhzProtocolDecoder subghz_protocol_fiat_v1_decoder = {
    .alloc = subghz_protocol_decoder_fiat_v1_alloc,
    .free = subghz_protocol_decoder_fiat_v1_free,
    .feed = NULL,
    .reset = subghz_protocol_decoder_fiat_v1_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_v1_serialize,
    .deserialize = subghz_protocol_decoder_fiat_v1_deserialize,
    .get_string = subghz_protocol_decoder_fiat_v1_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_v1_encoder = {
    .alloc = subghz_protocol_encoder_fiat_v1_alloc,
    .free = subghz_protocol_encoder_fiat_v1_free,
    .deserialize = subghz_protocol_encoder_fiat_v1_deserialize,
    .stop = subghz_protocol_encoder_fiat_v1_stop,
    .yield = subghz_protocol_encoder_fiat_v1_yield,
};

const SubGhzProtocol subghz_protocol_fiat_v1 = {
    .name = SUBGHZ_PROTOCOL_FIAT_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_fiat_v1_decoder,
    .encoder = &subghz_protocol_fiat_v1_encoder,
};

bool fiat_v1_button_valid(uint8_t button) {
    return button == 0x1U || button == 0x2U || button == 0x4U || button == 0x8U;
}

static const char* fiat_v1_button_name(uint8_t button) {
    switch(button) {
    case 0x8U:
        return "Unlock";
    case 0x4U:
        return "Lock";
    case 0x2U:
        return "Trunk";
    case 0x1U:
        return "Close";
    default:
        return "Unknown";
    }
}

static uint8_t fiat_v1_frame_xor(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    uint8_t value = 0x01U;
    for(uint8_t i = 0U; i < FIAT_V1_WIRE_BYTES - 1U; i++) {
        value ^= raw[i];
    }
    return value;
}

static void fiat_v1_build_raw(
    uint8_t raw[FIAT_V1_WIRE_BYTES],
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t auth,
    uint8_t tail_bits) {
    memset(raw, 0, FIAT_V1_WIRE_BYTES);
    raw[1] = 0x01U;
    raw[2] = (uint8_t)(uid >> 24U);
    raw[3] = (uint8_t)(uid >> 16U);
    raw[4] = (uint8_t)(uid >> 8U);
    raw[5] = (uint8_t)uid;
    raw[6] = (uint8_t)(((button & 0x0FU) << 4U) | ((control >> 6U) & 0x0FU));
    raw[7] = (uint8_t)(((control & 0x3FU) << 2U) | ((auth >> 30U) & 0x03U));
    raw[8] = (uint8_t)(auth >> 22U);
    raw[9] = (uint8_t)(auth >> 14U);
    raw[10] = (uint8_t)(auth >> 6U);
    raw[11] = (uint8_t)((auth << 2U) | (tail_bits & 0x03U));
    raw[12] = fiat_v1_frame_xor(raw);
}

uint32_t fiat_v1_uid(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    return ((uint32_t)raw[2] << 24U) | ((uint32_t)raw[3] << 16U) | ((uint32_t)raw[4] << 8U) |
           raw[5];
}

uint32_t fiat_v1_counter(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    return ((uint32_t)(raw[6] & 0x0FU) << 6U) | (raw[7] >> 2U);
}

uint32_t fiat_v1_hop(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    return ((uint32_t)(raw[7] & 0x03U) << 30U) | ((uint32_t)raw[8] << 22U) |
           ((uint32_t)raw[9] << 14U) | ((uint32_t)raw[10] << 6U) | (raw[11] >> 2U);
}

bool fiat_v1_frame_valid(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    if(raw[0] != 0x00U || raw[1] != 0x01U) {
        return false;
    }
    if(fiat_v1_frame_xor(raw) != raw[12]) {
        return false;
    }
    if(!fiat_v1_button_valid(raw[6] >> 4U)) {
        return false;
    }

    const uint32_t uid = fiat_v1_uid(raw);
    return uid != 0U && uid != UINT32_MAX;
}

static void fiat_v1_decode_fields(SubGhzProtocolDecoderFiatV1* instance) {
    instance->family = instance->raw_data[1];
    instance->uid = fiat_v1_uid(instance->raw_data);
    instance->generic.serial = instance->uid;
    instance->generic.btn = instance->raw_data[6] >> 4U;
    instance->generic.cnt = fiat_v1_counter(instance->raw_data);
    instance->hop = fiat_v1_hop(instance->raw_data);
    instance->tail_bits = instance->raw_data[11] & 0x03U;
    instance->frame_xor = instance->raw_data[12];
    instance->generic.data = ((uint64_t)instance->generic.serial << 32U) | instance->hop;
    instance->generic.data_count_bit = FIAT_V1_LOGICAL_BITS;
    fiat_v1_verify_hitag2_key(instance);
}

static bool fiat_v1_commit(
    SubGhzProtocolDecoderFiatV1* instance,
    const uint8_t raw[FIAT_V1_WIRE_BYTES],
    uint8_t te_variant) {
    if(!fiat_v1_frame_valid(raw)) {
        return false;
    }

    if(instance->last_raw_valid && memcmp(instance->last_raw_data, raw, FIAT_V1_WIRE_BYTES) == 0) {
        return true;
    }

    memcpy(instance->raw_data, raw, FIAT_V1_WIRE_BYTES);
    memcpy(instance->last_raw_data, raw, FIAT_V1_WIRE_BYTES);
    instance->last_raw_valid = true;
    instance->te_variant = (te_variant == FIAT_V1_TE_VARIANT_A) ? FIAT_V1_TE_VARIANT_A :
                                                                  FIAT_V1_TE_VARIANT_B;
    fiat_v1_decode_fields(instance);

    FURI_LOG_D(
        TAG,
        "Accepted UID:%08lX Btn:%02X Cnt:%03lX Auth:%08lX XOR:%02X",
        (unsigned long)instance->uid,
        instance->generic.btn,
        (unsigned long)instance->generic.cnt,
        (unsigned long)instance->hop,
        instance->frame_xor);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    return true;
}

static void fiat_v1_rebuild_raw(SubGhzProtocolDecoderFiatV1* instance) {
    fiat_v1_build_raw(
        instance->raw_data,
        instance->generic.serial,
        instance->generic.btn,
        (uint16_t)(instance->generic.cnt & 0x03FFU),
        instance->hop,
        instance->tail_bits);
    if(instance->family && instance->family != 0x01U) {
        instance->raw_data[1] = instance->family;
        instance->raw_data[12] = fiat_v1_frame_xor(instance->raw_data);
    }
    if(instance->frame_xor) {
        instance->raw_data[12] = instance->frame_xor;
    }
}

static void fiat_v1_verify_hitag2_key(SubGhzProtocolDecoderFiatV1* instance) {
    instance->hitag2_key_valid = false;
    instance->hitag2_epoch = 0U;
    memset(instance->hitag2_key, 0, sizeof(instance->hitag2_key));

    instance->hitag2_key_valid = fiat_v1_resolve_hitag2_key(
        NULL,
        instance->uid,
        instance->generic.btn,
        (uint16_t)(instance->generic.cnt & 0x03FFU),
        instance->hop,
        instance->hitag2_key,
        &instance->hitag2_epoch);
}

static bool fiat_v1_encoder_build_upload(SubGhzProtocolEncoderFiatV1* instance) {
    furi_check(instance);
    LevelDuration* upload = instance->encoder.upload;
    if(!upload) {
        return false;
    }

    const uint8_t variant = instance->te_variant;
    const uint32_t te = fiat_v1_enc_te_us(variant);
    size_t index = 0U;
    const size_t cap = FIAT_V1_UPLOAD_CAPACITY;
    index = hitag2_emit(upload, index, cap, true, fiat_v1_enc_lead_us(variant));

    index = hitag2_emit_manchester(
        upload, index, cap, instance->raw_data, FIAT_V1_WIRE_BITS, te, false);

    index = hitag2_emit(upload, index, cap, false, fiat_v1_enc_gap_us(variant));
    furi_check(index <= cap);
    instance->encoder.size_upload = index;
    instance->encoder.front = 0U;
    return true;
}

void* subghz_protocol_encoder_fiat_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFiatV1* instance = calloc(1, sizeof(SubGhzProtocolEncoderFiatV1));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_fiat_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->tail_bits = FIAT_V1_DEFAULT_TAIL_BITS;
    instance->te_variant = FIAT_V1_DEFAULT_TE_VARIANT;
    instance->encoder.repeat = FIAT_V1_ENC_DEFAULT_REPEAT;
    return instance;
}

void subghz_protocol_encoder_fiat_v1_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_fiat_v1_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_fiat_v1_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV1* instance = context;
    if(instance->encoder.repeat == 0 || !instance->encoder.is_running ||
       instance->encoder.size_upload == 0) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

static SubGhzProtocolStatus fiat_v1_encoder_transmit_captured(
    SubGhzProtocolEncoderFiatV1* instance,
    FlipperFormat* flipper_format,
    const uint8_t* raw,
    bool have_raw,
    uint32_t serial,
    uint32_t button,
    uint32_t control,
    uint32_t hop) {
    if(have_raw) {
        memcpy(instance->raw_data, raw, FIAT_V1_WIRE_BYTES);
    } else {
        if(serial == 0U || serial == UINT32_MAX || !fiat_v1_button_valid((uint8_t)button)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        fiat_v1_build_raw(
            instance->raw_data,
            serial,
            (uint8_t)button,
            (uint16_t)(control & 0x03FFU),
            hop,
            instance->tail_bits);
        if(!fiat_v1_frame_valid(instance->raw_data)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
    }

    instance->generic.serial = have_raw ? fiat_v1_uid(instance->raw_data) : serial;
    instance->generic.btn = have_raw ? (uint8_t)(instance->raw_data[6] >> 4U) : (uint8_t)button;
    instance->generic.cnt = have_raw ? fiat_v1_counter(instance->raw_data) : (control & 0x03FFU);
    instance->hop = have_raw ? fiat_v1_hop(instance->raw_data) : hop;
    instance->frame_xor = instance->raw_data[12];
    instance->generic.data = ((uint64_t)instance->generic.serial << 32U) | instance->hop;
    instance->generic.data_count_bit = FIAT_V1_LOGICAL_BITS;

    instance->encoder.repeat =
        fiat_v1_encoder_read_repeat(flipper_format, FIAT_V1_ENC_DEFAULT_REPEAT);
    if(instance->encoder.repeat == 0U) {
        instance->encoder.repeat = FIAT_V1_ENC_DEFAULT_REPEAT;
    }
    if(!instance->encoder.upload || instance->encoder.size_upload < FIAT_V1_UPLOAD_CAPACITY) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(FIAT_V1_UPLOAD_CAPACITY * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = FIAT_V1_UPLOAD_CAPACITY;
    if(!fiat_v1_encoder_build_upload(instance)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    furi_check(flipper_format);
    SubGhzProtocolEncoderFiatV1* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0U;

    flipper_format_rewind(flipper_format);
    if(fiat_v1_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    uint32_t bit_count = 0U;
    static const uint16_t allowed_bits[] = {FIAT_V1_LOGICAL_BITS};
    if(fiat_v1_encoder_read_bit(flipper_format, allowed_bits, 1U, &bit_count) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }
    instance->generic.data_count_bit = bit_count;

    uint32_t key_serial = 0U;
    uint32_t key_button = 0U;
    uint32_t key_control = 0U;
    uint32_t captured_hop = 0U;
    uint8_t raw_from_file[FIAT_V1_WIRE_BYTES] = {0};
    bool have_raw = false;

    flipper_format_rewind(flipper_format);
    if(flipper_format_read_hex(
           flipper_format, FIAT_V1_RAW_FIELD, raw_from_file, sizeof(raw_from_file)) &&
       fiat_v1_frame_valid(raw_from_file)) {
        have_raw = true;
        key_serial = fiat_v1_uid(raw_from_file);
        key_button = raw_from_file[6] >> 4U;
        key_control = fiat_v1_counter(raw_from_file);
        captured_hop = fiat_v1_hop(raw_from_file);
        instance->tail_bits = raw_from_file[11] & 0x03U;
    } else {
        SubGhzBlockGeneric generic = {0};
        flipper_format_rewind(flipper_format);
        if(subghz_block_generic_deserialize_check_count_bit(
               &generic, flipper_format, subghz_protocol_fiat_v1_const.min_count_bit_for_found) ==
           SubGhzProtocolStatusOk) {
            key_serial = (uint32_t)(generic.data >> 32U);
            key_button = generic.btn;
            key_control = generic.cnt;
            captured_hop = (uint32_t)generic.data;
        }
        flipper_format_rewind(flipper_format);
        uint32_t hop_field = 0U;
        if(flipper_format_read_uint32(flipper_format, FIAT_V1_HOP_FIELD, &hop_field, 1U)) {
            captured_hop = hop_field;
        }
    }

    uint32_t serial = key_serial;
    uint32_t button = key_button;
    uint32_t control = key_control;
    uint32_t type = FIAT_V1_DEFAULT_TE_VARIANT;
    fiat_v1_encoder_read_fields(flipper_format, &serial, &button, &control, &type);
    if(!have_raw) {

        key_serial = serial;
        key_control = control;
        if(fiat_v1_button_valid((uint8_t)key_button) == false) {
            key_button = button;
        }
    }

    if(!(subghz_block_generic_global.hitag2_replay_only && have_raw) &&
       (serial == 0U || serial == UINT32_MAX || !fiat_v1_button_valid((uint8_t)button))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    uint32_t tail_bits = instance->tail_bits;
    if(hitag2_flipper_read_u32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, &tail_bits)) {
        instance->tail_bits = (uint8_t)(tail_bits & 0x03U);
    }

    instance->te_variant = (type == FIAT_V1_TE_VARIANT_A) ? FIAT_V1_TE_VARIANT_A :
                                                            FIAT_V1_TE_VARIANT_B;

    if(subghz_block_generic_global.hitag2_replay_only) {
        return fiat_v1_encoder_transmit_captured(
            instance,
            flipper_format,
            raw_from_file,
            have_raw,
            have_raw ? key_serial : serial,
            have_raw ? key_button : button,
            have_raw ? key_control : control,
            captured_hop);
    }

    if(!fiat_v1_resolve_hitag2_key(
           flipper_format,
           key_serial ? key_serial : serial,
           (uint8_t)(key_button ? key_button : button),
           (uint16_t)(key_control & 0x03FFU),
           captured_hop,
           instance->hitag2_key,
           &instance->epoch)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    uint8_t btn = (uint8_t)(button & 0x0FU);
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(have_raw ? (uint8_t)key_button : btn);
    }
    subghz_block_generic_global_button_override_get(&btn);
    if(!fiat_v1_button_valid(btn)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    if(!subghz_block_generic_global_counter_override_get(&control)) {
        control += (uint32_t)furi_hal_subghz_get_rolling_counter_mult();
    }
    control &= 0x03FFU;

    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = control;
    instance->hop = fiat_v1_bcm_generate_authenticator(
        serial, btn, (uint16_t)control, instance->hitag2_key, instance->epoch);
    instance->generic.data = ((uint64_t)serial << 32U) | instance->hop;
    instance->generic.data_count_bit = FIAT_V1_LOGICAL_BITS;

    fiat_v1_build_raw(
        instance->raw_data, serial, btn, (uint16_t)control, instance->hop, instance->tail_bits);
    instance->frame_xor = instance->raw_data[12];

    uint32_t serial_w = serial;
    uint32_t btn_w = btn;
    uint32_t cnt_w = control;
    uint32_t hop_w = instance->hop;
    uint32_t xor_w = instance->frame_xor;
    uint32_t tail_w = instance->tail_bits;
    uint32_t type_w = instance->te_variant;
    uint32_t epoch_w = instance->epoch & 0x3FFFFUL;
    flipper_format_rewind(flipper_format);
    flipper_format_delete_key(flipper_format, FIAT_V1_RAW_FIELD);
    hitag2_flipper_u32(flipper_format, "Serial", serial_w);
    hitag2_flipper_u32(flipper_format, "Btn", btn_w);
    hitag2_flipper_u32(flipper_format, "Cnt", cnt_w);
    hitag2_flipper_u32(flipper_format, "Type", type_w);
    hitag2_flipper_u32(flipper_format, FIAT_V1_HOP_FIELD, hop_w);
    hitag2_flipper_u32(flipper_format, FIAT_V1_XOR_FIELD, xor_w);
    hitag2_flipper_u32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, tail_w);
    flipper_format_insert_or_update_hex(
        flipper_format, FIAT_V1_RAW_FIELD, instance->raw_data, FIAT_V1_WIRE_BYTES);
    uint8_t hitag2_disk_key[6];
    fiat_v1_hitag2_key_cipher_to_disk(instance->hitag2_key, hitag2_disk_key);
    flipper_format_insert_or_update_hex(
        flipper_format, FIAT_V1_HITAG2_KEY_FIELD, hitag2_disk_key, 6U);
    hitag2_flipper_u32(flipper_format, FIAT_V1_HITAG2_EPOCH_FIELD, epoch_w);
    (void)fiat_v1_write_display(
        flipper_format, instance->base.protocol->name, fiat_v1_button_name(btn));

    instance->encoder.repeat =
        fiat_v1_encoder_read_repeat(flipper_format, FIAT_V1_ENC_DEFAULT_REPEAT);
    if(instance->encoder.repeat == 0U) {
        instance->encoder.repeat = FIAT_V1_ENC_DEFAULT_REPEAT;
    }

    if(!instance->encoder.upload || instance->encoder.size_upload < FIAT_V1_UPLOAD_CAPACITY) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(FIAT_V1_UPLOAD_CAPACITY * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = FIAT_V1_UPLOAD_CAPACITY;
    if(!fiat_v1_encoder_build_upload(instance)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    instance->encoder.is_running = true;

    FURI_LOG_I(
        TAG,
        "TX UID:%08lX Btn:%02X Cnt:%03lX Auth:%08lX Epoch:%05lX TE:%u XOR:%02X",
        (unsigned long)serial,
        btn,
        (unsigned long)control,
        (unsigned long)instance->hop,
        (unsigned long)instance->epoch,
        instance->te_variant,
        instance->frame_xor);

    return SubGhzProtocolStatusOk;
}

void* subghz_protocol_decoder_fiat_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFiatV1* instance = calloc(1, sizeof(SubGhzProtocolDecoderFiatV1));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    subghz_protocol_decoder_fiat_v1_reset(instance);
    return instance;
}

void subghz_protocol_decoder_fiat_v1_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_fiat_v1_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV1* instance = context;

    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    memset(instance->last_raw_data, 0, sizeof(instance->last_raw_data));
    instance->last_raw_valid = false;
    instance->generic.data = 0U;
    instance->generic.data_count_bit = 0U;
    instance->generic.serial = 0U;
    instance->generic.btn = 0U;
    instance->generic.cnt = 0U;
    instance->uid = 0U;
    instance->hop = 0U;
    instance->family = 0U;
    instance->tail_bits = FIAT_V1_DEFAULT_TAIL_BITS;
    instance->frame_xor = 0U;
    instance->hitag2_key_valid = false;
    instance->hitag2_epoch = 0U;
    instance->te_variant = FIAT_V1_DEFAULT_TE_VARIANT;
    memset(instance->hitag2_key, 0, sizeof(instance->hitag2_key));
}

uint8_t subghz_protocol_decoder_fiat_v1_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV1* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = 64U,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, 8U) ^
           (uint8_t)(instance->generic.cnt >> 8U) ^ (uint8_t)instance->generic.cnt ^
           instance->generic.btn ^ instance->frame_xor;
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV1* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    flipper_format_rewind(flipper_format);
    flipper_format_delete_key(flipper_format, FIAT_V1_RAW_FIELD);

    uint32_t hop = instance->hop;
    uint32_t frame_xor = instance->frame_xor;
    uint32_t tail_bits = instance->tail_bits;
    uint32_t te_variant = instance->te_variant;
    hitag2_flipper_u32(flipper_format, "Serial", instance->generic.serial);
    hitag2_flipper_u32(flipper_format, "Btn", instance->generic.btn);
    hitag2_flipper_u32(flipper_format, "Cnt", instance->generic.cnt);
    hitag2_flipper_u32(flipper_format, "Type", te_variant);
    hitag2_flipper_u32(flipper_format, FIAT_V1_HOP_FIELD, hop);
    hitag2_flipper_u32(flipper_format, FIAT_V1_XOR_FIELD, frame_xor);
    hitag2_flipper_u32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, tail_bits);

    if(!flipper_format_insert_or_update_hex(
           flipper_format, FIAT_V1_RAW_FIELD, instance->raw_data, FIAT_V1_WIRE_BYTES)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    if(!instance->hitag2_key_valid) {
        uint8_t key[6] = {0};
        uint32_t epoch = 0U;
        if(fiat_v1_resolve_hitag2_key(
               flipper_format,
               instance->uid,
               instance->generic.btn,
               (uint16_t)(instance->generic.cnt & 0x03FFU),
               instance->hop,
               key,
               &epoch)) {
            memcpy(instance->hitag2_key, key, sizeof(instance->hitag2_key));
            instance->hitag2_epoch = epoch;
            instance->hitag2_key_valid = true;
        }
    }

    if(instance->hitag2_key_valid) {
        uint32_t epoch = instance->hitag2_epoch & 0x3FFFFUL;
        uint8_t hitag2_disk_key[6];
        fiat_v1_hitag2_key_cipher_to_disk(instance->hitag2_key, hitag2_disk_key);
        if(!flipper_format_insert_or_update_hex(
               flipper_format, FIAT_V1_HITAG2_KEY_FIELD, hitag2_disk_key, 6U)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        hitag2_flipper_u32(flipper_format, FIAT_V1_HITAG2_EPOCH_FIELD, epoch);
    }

    return fiat_v1_write_display(
        flipper_format,
        instance->generic.protocol_name,
        fiat_v1_button_name(instance->generic.btn));
}

static void
    fiat_v1_load_hitag2_key(SubGhzProtocolDecoderFiatV1* instance, FlipperFormat* flipper_format) {
    instance->hitag2_key_valid = fiat_v1_resolve_hitag2_key(
        flipper_format,
        instance->uid,
        instance->generic.btn,
        (uint16_t)(instance->generic.cnt & 0x03FFU),
        instance->hop,
        instance->hitag2_key,
        &instance->hitag2_epoch);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV1* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_fiat_v1_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }
    if(instance->generic.data_count_bit != FIAT_V1_LOGICAL_BITS) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    flipper_format_rewind(flipper_format);
    uint32_t value = 0U;
    instance->te_variant = FIAT_V1_DEFAULT_TE_VARIANT;
    if(hitag2_flipper_read_u32(flipper_format, "Type", &value)) {
        instance->te_variant = (value == FIAT_V1_TE_VARIANT_A) ? FIAT_V1_TE_VARIANT_A :
                                                                 FIAT_V1_TE_VARIANT_B;
    }

    flipper_format_rewind(flipper_format);
    if(flipper_format_read_hex(
           flipper_format, FIAT_V1_RAW_FIELD, instance->raw_data, FIAT_V1_WIRE_BYTES)) {
        if(!fiat_v1_frame_valid(instance->raw_data)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        fiat_v1_decode_fields(instance);
        fiat_v1_load_hitag2_key(instance, flipper_format);
        return SubGhzProtocolStatusOk;
    }

    instance->generic.serial = (uint32_t)(instance->generic.data >> 32U);
    instance->hop = (uint32_t)instance->generic.data;
    instance->family = 0x01U;
    instance->uid = instance->generic.serial;
    instance->tail_bits = FIAT_V1_DEFAULT_TAIL_BITS;

    if(flipper_format_read_uint32(flipper_format, "Serial", &value, 1)) {
        instance->generic.serial = value;
    }
    if(flipper_format_read_uint32(flipper_format, "Btn", &value, 1)) {
        instance->generic.btn = (uint8_t)value;
    }
    if(flipper_format_read_uint32(flipper_format, "Cnt", &value, 1)) {
        instance->generic.cnt = value;
    }
    if(flipper_format_read_uint32(flipper_format, FIAT_V1_HOP_FIELD, &value, 1)) {
        instance->hop = value;
    }
    if(flipper_format_read_uint32(flipper_format, FIAT_V1_XOR_FIELD, &value, 1)) {
        instance->frame_xor = (uint8_t)value;
    }
    if(flipper_format_read_uint32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, &value, 1)) {
        instance->tail_bits = (uint8_t)(value & 0x03U);
    }

    fiat_v1_rebuild_raw(instance);
    if(!fiat_v1_frame_valid(instance->raw_data)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    fiat_v1_decode_fields(instance);
    fiat_v1_load_hitag2_key(instance, flipper_format);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_fiat_v1_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV1* instance = context;

    fiat_v1_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 12, 3);

    furi_string_cat_printf(
        output,
        "%s %dbit %s\r\n"
        "%08lX %03lX%01X %08lX\r\n"
        "Sync:%02X UID:%08lX Auth:%08lX\r\n"
        "Btn:%02X [%s] Ctrl:%03lX\r\n"
        "TE:%u Tail:%u XOR:%02X\r\n",
        instance->generic.protocol_name,
        FIAT_V1_LOGICAL_BITS,
        instance->hitag2_key_valid ? "KEY:OK" : "KEY:??",
        (unsigned long)instance->generic.serial,
        (unsigned long)instance->generic.cnt,
        instance->generic.btn,
        (unsigned long)instance->hop,
        instance->family,
        (unsigned long)instance->uid,
        (unsigned long)instance->hop,
        instance->generic.btn,
        fiat_v1_button_name(instance->generic.btn),
        (unsigned long)instance->generic.cnt,
        instance->te_variant,
        instance->tail_bits,
        instance->frame_xor);
}

bool fiat_v1_family_frame(void* context, const uint8_t raw[13], uint8_t variant) {
    return fiat_v1_commit(context, raw, variant);
}
void fiat_v1_family_raw(void* context, uint8_t raw[14]) {
    SubGhzProtocolDecoderFiatV1* instance = context;
    memset(raw, 0, 14);
    memcpy(raw, instance->raw_data, FIAT_V1_WIRE_BYTES);
}

bool fiat_v1_family_valid(const uint8_t raw[13]) {
    return fiat_v1_frame_valid(raw);
}

#undef TAG

#define TAG "FiatProtocolV2"

#define FIAT_V2_TE_SHORT           210U
#define FIAT_V2_TE_LONG            420U
#define FIAT_V2_TE_DELTA           100U
#define FIAT_V2_WIRE_BITS          112U
#define FIAT_V2_WIRE_BYTES         14U
#define FIAT_V2_WIRE_CELLS         (FIAT_V2_WIRE_BITS * 2U)
#define FIAT_V2_LOGICAL_BITS       112U
#define FIAT_V2_MARKER0            0x00U
#define FIAT_V2_MARKER1            0x01U
#define FIAT_V2_BTN_SHIFT          6U
#define FIAT_V2_BUTTON_LOCK        0x2U
#define FIAT_V2_BUTTON_UNLOCK      0x3U
#define FIAT_V2_BUTTON_TRUNK       0x1U
#define FIAT_V2_CNT_SHIFT          3U
#define FIAT_V2_FCA_TYPE_NIBBLE    0xD0U
#define FIAT_V2_RAW_FIELD          "Raw"
#define FIAT_V2_HOP_FIELD          "Hop"
#define FIAT_V2_BTN_FIELD          "Btn"
#define FIAT_V2_ENC_GAP_US         2000U
#define FIAT_V2_ENC_DEFAULT_REPEAT 6U
#define FIAT_V2_UPLOAD_CAPACITY    256U

static const SubGhzBlockConst subghz_protocol_fiat_v2_const = {
    .te_short = FIAT_V2_TE_SHORT,
    .te_long = FIAT_V2_TE_LONG,
    .te_delta = FIAT_V2_TE_DELTA,
    .min_count_bit_for_found = FIAT_V2_LOGICAL_BITS,
};

struct SubGhzProtocolDecoderFiatV2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockGeneric generic;

    uint8_t raw_data[FIAT_V2_WIRE_BYTES];
    uint8_t last_raw_data[FIAT_V2_WIRE_BYTES];
    bool last_raw_valid;

    uint32_t uid;
    uint32_t hop;
    uint8_t button;
};

typedef struct SubGhzProtocolEncoderFiatV2 SubGhzProtocolEncoderFiatV2;
struct SubGhzProtocolEncoderFiatV2 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint8_t raw_data[FIAT_V2_WIRE_BYTES];
};

void* subghz_protocol_encoder_fiat_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_fiat_v2_free(void* context);
void subghz_protocol_encoder_fiat_v2_stop(void* context);
LevelDuration subghz_protocol_encoder_fiat_v2_yield(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_v2_deserialize(void* context, FlipperFormat* flipper_format);

static bool fiat_v2_frame_valid(const uint8_t raw[FIAT_V2_WIRE_BYTES]);

const SubGhzProtocolDecoder subghz_protocol_fiat_v2_decoder = {
    .alloc = subghz_protocol_decoder_fiat_v2_alloc,
    .free = subghz_protocol_decoder_fiat_v2_free,
    .feed = NULL,
    .reset = subghz_protocol_decoder_fiat_v2_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_v2_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_v2_serialize,
    .deserialize = subghz_protocol_decoder_fiat_v2_deserialize,
    .get_string = subghz_protocol_decoder_fiat_v2_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_v2_encoder = {
    .alloc = subghz_protocol_encoder_fiat_v2_alloc,
    .free = subghz_protocol_encoder_fiat_v2_free,
    .deserialize = subghz_protocol_encoder_fiat_v2_deserialize,
    .stop = subghz_protocol_encoder_fiat_v2_stop,
    .yield = subghz_protocol_encoder_fiat_v2_yield,
};

const SubGhzProtocol subghz_protocol_fiat_v2 = {
    .name = SUBGHZ_PROTOCOL_FIAT_V2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_fiat_v2_decoder,
    .encoder = &subghz_protocol_fiat_v2_encoder,
};

static bool fiat_v2_button_valid(uint8_t button) {
    const uint8_t sel = button >> FIAT_V2_BTN_SHIFT;
    return sel == FIAT_V2_BUTTON_LOCK || sel == FIAT_V2_BUTTON_UNLOCK ||
           sel == FIAT_V2_BUTTON_TRUNK;
}

static const char* fiat_v2_button_name(uint8_t button) {
    switch(button >> FIAT_V2_BTN_SHIFT) {
    case FIAT_V2_BUTTON_LOCK:
        return "Lock";
    case FIAT_V2_BUTTON_UNLOCK:
        return "Unlock";
    case FIAT_V2_BUTTON_TRUNK:
        return "Trunk";
    default:
        return "Unknown";
    }
}

static uint32_t fiat_v2_uid(const uint8_t raw[FIAT_V2_WIRE_BYTES]) {
    return ((uint32_t)raw[2] << 24U) | ((uint32_t)raw[3] << 16U) | ((uint32_t)raw[4] << 8U) |
           raw[5];
}

static bool fiat_v2_is_fca(const uint8_t raw[FIAT_V2_WIRE_BYTES]) {
    return (raw[6] & 0xF0U) == FIAT_V2_FCA_TYPE_NIBBLE;
}
static uint32_t fiat_v2_hop(const uint8_t raw[FIAT_V2_WIRE_BYTES]) {
    if(fiat_v2_is_fca(raw)) {
        return ((uint32_t)raw[10] << 24U) | ((uint32_t)raw[11] << 16U) |
               ((uint32_t)raw[12] << 8U) | raw[13];
    }
    return ((uint32_t)raw[9] << 24U) | ((uint32_t)raw[10] << 16U) | ((uint32_t)raw[11] << 8U) |
           raw[12];
}

static uint32_t fiat_v2_counter(const uint8_t raw[FIAT_V2_WIRE_BYTES]) {
    if(fiat_v2_is_fca(raw)) {
        const uint32_t raw_cnt = ((uint32_t)raw[8] << 6U) | (uint32_t)(raw[9] >> 2U);
        return (~raw_cnt) & 0x3FFFU;
    }
    const uint32_t raw_cnt = ((uint32_t)(raw[7] & 0x3FU) << 5U) |
                             (uint32_t)(raw[8] >> FIAT_V2_CNT_SHIFT);
    return (~raw_cnt) & 0x7FFU;
}

static bool fiat_v2_type_valid(uint8_t type_byte) {
    const uint8_t type = type_byte & 0xF0U;

    if(type == 0x70U || type == 0xA0U) {
        return false;
    }
    if(type == 0x10U) {
        return false;
    }

    return true;
}

static bool fiat_v2_frame_valid(const uint8_t raw[FIAT_V2_WIRE_BYTES]) {
    if(raw[0] != FIAT_V2_MARKER0 || raw[1] != FIAT_V2_MARKER1) {
        return false;
    }
    if(!fiat_v2_type_valid(raw[6])) {
        return false;
    }
    if(!fiat_v2_button_valid(raw[7])) {
        return false;
    }
    if(fiat_v2_is_fca(raw) && (raw[7] & 0x3FU) != 0U) {
        return false;
    }

    const uint32_t uid = fiat_v2_uid(raw);
    return uid != 0U && uid != UINT32_MAX;
}

static void fiat_v2_decode_fields(SubGhzProtocolDecoderFiatV2* instance) {
    instance->uid = fiat_v2_uid(instance->raw_data);
    instance->button = instance->raw_data[7];
    instance->hop = fiat_v2_hop(instance->raw_data);
    instance->generic.serial = instance->uid;
    instance->generic.btn = instance->button;
    instance->generic.cnt = fiat_v2_counter(instance->raw_data);
    instance->generic.data = ((uint64_t)instance->generic.serial << 32U) | instance->hop;
    instance->generic.data_count_bit = FIAT_V2_LOGICAL_BITS;
}

static bool
    fiat_v2_commit(SubGhzProtocolDecoderFiatV2* instance, const uint8_t raw[FIAT_V2_WIRE_BYTES]) {
    if(!fiat_v2_frame_valid(raw)) {
        return false;
    }

    if(instance->last_raw_valid && memcmp(instance->last_raw_data, raw, FIAT_V2_WIRE_BYTES) == 0) {
        return true;
    }

    memcpy(instance->raw_data, raw, FIAT_V2_WIRE_BYTES);
    memcpy(instance->last_raw_data, raw, FIAT_V2_WIRE_BYTES);
    instance->last_raw_valid = true;
    fiat_v2_decode_fields(instance);

    FURI_LOG_D(
        TAG,
        "Accepted UID:%08lX Btn:%02X Cnt:%02lX Hop:%08lX",
        (unsigned long)instance->uid,
        instance->button,
        (unsigned long)instance->generic.cnt,
        (unsigned long)instance->hop);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    return true;
}

void* subghz_protocol_decoder_fiat_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFiatV2* instance = calloc(1, sizeof(SubGhzProtocolDecoderFiatV2));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    subghz_protocol_decoder_fiat_v2_reset(instance);
    return instance;
}

void subghz_protocol_decoder_fiat_v2_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_fiat_v2_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV2* instance = context;

    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    memset(instance->last_raw_data, 0, sizeof(instance->last_raw_data));
    instance->last_raw_valid = false;
    instance->generic.data = 0U;
    instance->generic.data_count_bit = 0U;
    instance->generic.serial = 0U;
    instance->generic.btn = 0U;
    instance->generic.cnt = 0U;
    instance->uid = 0U;
    instance->hop = 0U;
    instance->button = 0U;
}

uint8_t subghz_protocol_decoder_fiat_v2_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV2* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = 64U,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, 8U) ^ instance->generic.cnt ^
           instance->button;
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV2* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_hex(
        flipper_format, FIAT_V2_RAW_FIELD, instance->raw_data, FIAT_V2_WIRE_BYTES);

    uint32_t hop = instance->hop;
    uint32_t button = instance->button;
    if(!flipper_format_write_uint32(flipper_format, FIAT_V2_HOP_FIELD, &hop, 1) ||
       !flipper_format_write_uint32(flipper_format, FIAT_V2_BTN_FIELD, &button, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV2* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_fiat_v2_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }
    if(instance->generic.data_count_bit != FIAT_V2_LOGICAL_BITS) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    flipper_format_rewind(flipper_format);
    if(flipper_format_read_hex(
           flipper_format, FIAT_V2_RAW_FIELD, instance->raw_data, FIAT_V2_WIRE_BYTES)) {
        if(!fiat_v2_frame_valid(instance->raw_data)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        fiat_v2_decode_fields(instance);
        return SubGhzProtocolStatusOk;
    }

    return SubGhzProtocolStatusErrorParserOthers;
}

void subghz_protocol_decoder_fiat_v2_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV2* instance = context;

    subghz_block_generic_global.btn_is_available = false;
    subghz_block_generic_global.cnt_is_available = false;
    subghz_block_generic_global.current_btn = instance->button;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(instance->button);
    }
    subghz_custom_btn_set_max(1);

    furi_string_cat_printf(
        output,
        "%s %ubit\r\n"
        "UID:%08lX\r\n"
        "Hop:%08lX Type:%01X\r\n"
        "Btn:%02X [%s] Cnt:%02lX\r\n",
        instance->generic.protocol_name,
        FIAT_V2_LOGICAL_BITS,
        (unsigned long)instance->uid,
        (unsigned long)instance->hop,
        (unsigned)(instance->raw_data[6] >> 4),
        instance->button,
        fiat_v2_button_name(instance->button),
        (unsigned long)instance->generic.cnt);
}

static bool fiat_v2_encoder_build_upload(SubGhzProtocolEncoderFiatV2* instance) {
    LevelDuration* upload = instance->encoder.upload;
    if(!upload) return false;

    size_t index = 0U;
    const size_t cap = FIAT_V2_UPLOAD_CAPACITY;
    index = hitag2_emit(upload, index, cap, false, FIAT_V2_ENC_GAP_US);
    index = hitag2_emit_manchester(
        upload, index, cap, instance->raw_data, FIAT_V2_WIRE_BITS, FIAT_V2_TE_SHORT, true);
    if(!(instance->raw_data[FIAT_V2_WIRE_BYTES - 1] & 1U)) {
        index = hitag2_emit(upload, index, cap, true, FIAT_V2_TE_SHORT);
    }
    index = hitag2_emit(upload, index, cap, false, FIAT_V2_ENC_GAP_US);
    if(index == 0U || index > cap) return false;
    instance->encoder.size_upload = index;
    instance->encoder.front = 0U;
    return true;
}

void* subghz_protocol_encoder_fiat_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFiatV2* instance = calloc(1, sizeof(SubGhzProtocolEncoderFiatV2));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = FIAT_V2_ENC_DEFAULT_REPEAT;
    return instance;
}

void subghz_protocol_encoder_fiat_v2_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_fiat_v2_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV2* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0U;
}

LevelDuration subghz_protocol_encoder_fiat_v2_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV2* instance = context;
    if(instance->encoder.repeat == 0U || !instance->encoder.is_running ||
       instance->encoder.size_upload == 0U) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0U;
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    furi_check(flipper_format);
    SubGhzProtocolEncoderFiatV2* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0U;

    FuriString* proto = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    const bool is_fiat_v2 = flipper_format_read_string(flipper_format, "Protocol", proto) &&
                            furi_string_equal_str(proto, SUBGHZ_PROTOCOL_FIAT_V2_NAME);
    furi_string_free(proto);
    if(!is_fiat_v2) return SubGhzProtocolStatusErrorParserOthers;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_fiat_v2_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) return ret;
    if(instance->generic.data_count_bit != FIAT_V2_LOGICAL_BITS) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(
           flipper_format, FIAT_V2_RAW_FIELD, instance->raw_data, FIAT_V2_WIRE_BYTES) ||
       !fiat_v2_frame_valid(instance->raw_data)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    uint32_t repeat = FIAT_V2_ENC_DEFAULT_REPEAT;
    if(!hitag2_flipper_read_u32(flipper_format, "Repeat", &repeat) || repeat == 0U) {
        repeat = FIAT_V2_ENC_DEFAULT_REPEAT;
    }
    instance->encoder.repeat = repeat;

    if(!instance->encoder.upload || instance->encoder.size_upload < FIAT_V2_UPLOAD_CAPACITY) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(FIAT_V2_UPLOAD_CAPACITY * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = FIAT_V2_UPLOAD_CAPACITY;
    if(!fiat_v2_encoder_build_upload(instance)) {
        return SubGhzProtocolStatusErrorEncoderGetUpload;
    }
    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void fiat_v2_family_raw(void* context, uint8_t raw[14]) {
    SubGhzProtocolDecoderFiatV2* instance = context;
    memcpy(raw, instance->raw_data, FIAT_V2_WIRE_BYTES);
}

bool fiat_v2_family_valid(const uint8_t raw[14]) {
    return fiat_v2_frame_valid(raw);
}

bool fiat_v2_family_frame(void* context, const uint8_t raw[14]) {
    return fiat_v2_commit(context, raw);
}

#undef TAG

#define TAG "RenaultV1Protocol"

#define HITAG2_TE_US                125U
#define HITAG2_HEADER_LOW_US        1500U
#define HITAG2_HEADER_HIGH_US       1000U
#define HITAG2_SHORT_GAP_US         20000U
#define HITAG2_LONG_TO_SHORT_GAP_US 115500U
#define HITAG2_PREAMBLE_PAIRS       255U
#define HITAG2_SHORT_FRAMES         8U
#define HITAG2_UPLOAD_CAPACITY      1792U
#define HITAG2_TYPE_FIELD           "Type"
#define HITAG2_VARIANT_OLD          0U
#define HITAG2_VARIANT_KANGOO       1U
#define HITAG2_DEFAULT_VARIANT      HITAG2_VARIANT_KANGOO
#define HITAG2_OLD_LONG_FRAMES      2U
#define HITAG2_KANGOO_LONG_FRAMES   3U
#define HITAG2_MIN_COUNT_BIT        88U
#define HITAG2_HEADER_BITS          16U
#define HITAG2_KEY_BITS             64U
#define HITAG2_KEY2_BITS            24U
#define HITAG2_SHORT_KEY_BITS       10U
#define HITAG2_KEY_END_BITS         (HITAG2_HEADER_BITS + HITAG2_KEY_BITS)

static const SubGhzBlockConst subghz_protocol_hitag2_const = {
    .te_short = HITAG2_TE_US,
    .te_long = HITAG2_TE_US * 2U,
    .te_delta = 50,
    .min_count_bit_for_found = HITAG2_MIN_COUNT_BIT,
};

struct SubGhzProtocolDecoderHitag2 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockGeneric generic;

    uint16_t header;
    uint8_t recovered;
    uint8_t hitag2_key[6];
    uint32_t hitag2_epoch;
    uint32_t hop;
    uint8_t tail_bits;
    bool hell_key_valid;
    uint64_t last_data;
    uint64_t last_data_2;
    bool last_frame_valid;
    uint8_t tx_variant;
    uint8_t identical_count;
    bool pending_notify;
};

struct SubGhzProtocolEncoderHitag2 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t recovered;
    uint8_t hitag2_key[6];
    uint32_t hitag2_epoch;
    uint8_t tail_bits;
    bool hell_key_valid;
    uint8_t tx_variant;
};

static uint8_t subghz_protocol_decoder_hitag2_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_hitag2_decoder = {
    .alloc = subghz_protocol_decoder_hitag2_alloc,
    .free = subghz_protocol_decoder_hitag2_free,

    .feed = NULL,
    .reset = subghz_protocol_decoder_hitag2_reset,

    .get_hash_data = subghz_protocol_decoder_hitag2_get_hash_data,
    .serialize = subghz_protocol_decoder_hitag2_serialize,
    .deserialize = subghz_protocol_decoder_hitag2_deserialize,
    .get_string = subghz_protocol_decoder_hitag2_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_hitag2_encoder = {
    .alloc = subghz_protocol_encoder_hitag2_alloc,
    .free = subghz_protocol_encoder_hitag2_free,

    .deserialize = subghz_protocol_encoder_hitag2_deserialize,
    .stop = subghz_protocol_encoder_hitag2_stop,
    .yield = subghz_protocol_encoder_hitag2_yield,
};

const SubGhzProtocol subghz_protocol_renault_v1 = {
    .name = SUBGHZ_PROTOCOL_RENAULT_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868 |
            SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_hitag2_decoder,
    .encoder = &subghz_protocol_hitag2_encoder,
};

static const char* hitag2_get_button_name(uint8_t btn) {
    static const char* const names[] = {
        "Sync",
        "Lock",
        "Unlock",
        "??",
        "Trunk",
        "??",
        "??",
        "??",
        "Panic",
    };
    return (btn < COUNT_OF(names)) ? names[btn] : "??";
}

void hitag2_u64_to_bytes_be(uint64_t value, uint8_t* out, size_t nbytes) {
    for(size_t i = 0; i < nbytes; i++) {
        out[i] = (uint8_t)(value >> (8U * (nbytes - 1U - i)));
    }
}

uint64_t hitag2_bytes_to_u64_be(const uint8_t* data, size_t nbytes) {
    uint64_t value = 0;
    for(size_t i = 0; i < nbytes; i++) {
        value = (value << 8U) | data[i];
    }
    return value;
}

static void hitag2_pack_key_bytes(uint64_t key, uint64_t key_2, uint8_t raw[11]) {
    hitag2_u64_to_bytes_be(key, raw, 8);
    raw[8] = (uint8_t)(key_2 >> 16U);
    raw[9] = (uint8_t)(key_2 >> 8U);
    raw[10] = (uint8_t)key_2;
}

uint8_t hitag2_frame_xor(const uint8_t raw[11]) {
    uint8_t value = 0;
    for(size_t i = 0; i < 10; i++) {
        value ^= raw[i];
    }
    return value;
}

static uint16_t hitag2_hell_cnt10(const uint8_t raw[11]) {
    return (uint16_t)(((uint16_t)(raw[4] & 0x0FU) << 6U) | (raw[5] >> 2U));
}

static uint32_t hitag2_hell_hop(const uint8_t raw[11]) {
    return ((uint32_t)(raw[5] & 3U) << 30U) | ((uint32_t)raw[6] << 22U) |
           ((uint32_t)raw[7] << 14U) | ((uint32_t)raw[8] << 6U) | (raw[9] >> 2U);
}

static uint8_t hitag2_hell_tail(const uint8_t raw[11]) {
    return (uint8_t)(raw[9] & 3U);
}

static void hitag2_pack_hell_frame(
    uint32_t uid,
    uint8_t btn,
    uint16_t cnt10,
    uint32_t hop,
    uint8_t tail,
    uint8_t raw[11]) {
    raw[0] = (uint8_t)(uid >> 24U);
    raw[1] = (uint8_t)(uid >> 16U);
    raw[2] = (uint8_t)(uid >> 8U);
    raw[3] = (uint8_t)uid;
    raw[4] = (uint8_t)(((btn & 0x0FU) << 4U) | ((cnt10 >> 6U) & 0x0FU));
    raw[5] = (uint8_t)(((cnt10 & 0x3FU) << 2U) | ((hop >> 30U) & 3U));
    raw[6] = (uint8_t)(hop >> 22U);
    raw[7] = (uint8_t)(hop >> 14U);
    raw[8] = (uint8_t)(hop >> 6U);
    raw[9] = (uint8_t)(((hop << 2U) & 0xFCU) | (tail & 3U));
    raw[10] = hitag2_frame_xor(raw);
}

static void hitag2_apply_raw(uint8_t raw[11], uint64_t* data, uint64_t* data_2) {
    *data = hitag2_bytes_to_u64_be(raw, 8);
    *data_2 = ((uint64_t)raw[8] << 16U) | ((uint64_t)raw[9] << 8U) | raw[10];
}

static uint8_t hitag2_clamp_variant(uint32_t type) {
    return (type == HITAG2_VARIANT_OLD) ? HITAG2_VARIANT_OLD : HITAG2_VARIANT_KANGOO;
}

static uint8_t hitag2_read_variant(FlipperFormat* ff) {
    uint32_t type = HITAG2_DEFAULT_VARIANT;
    if(ff && flipper_format_rewind(ff)) {
        flipper_format_read_uint32(ff, HITAG2_TYPE_FIELD, &type, 1);
    }
    return hitag2_clamp_variant(type);
}

static uint8_t hitag2_variant_long_frames(uint8_t variant) {
    return (variant == HITAG2_VARIANT_KANGOO) ? HITAG2_KANGOO_LONG_FRAMES : HITAG2_OLD_LONG_FRAMES;
}

static void hitag2_decoder_notify(SubGhzProtocolDecoderHitag2* instance) {
    if(!instance->pending_notify) {
        return;
    }
    instance->pending_notify = false;
    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void hitag2_unpack_frame(
    uint64_t data,
    uint64_t data_2,
    uint32_t* serial,
    uint8_t* btn,
    uint16_t* cnt10,
    uint32_t* hop,
    uint8_t* tail) {
    uint8_t raw[11];
    hitag2_pack_key_bytes(data, data_2, raw);
    if(serial) {
        *serial = (uint32_t)(data >> 32U);
    }
    if(btn) {
        *btn = (uint8_t)((raw[4] >> 4U) & 0x0FU);
    }
    if(cnt10) {
        *cnt10 = hitag2_hell_cnt10(raw);
    }
    if(hop) {
        *hop = hitag2_hell_hop(raw);
    }
    if(tail) {
        *tail = hitag2_hell_tail(raw);
    }
}

static void hitag2_apply_check_remote(
    uint64_t data,
    uint64_t data_2,
    uint8_t recovered,
    uint32_t seed,
    uint32_t* serial,
    uint8_t* btn,
    uint32_t* cnt,
    uint32_t* hop,
    uint8_t* tail) {
    uint16_t cnt10 = 0;
    hitag2_unpack_frame(data, data_2, serial, btn, &cnt10, hop, tail);
    if(recovered == HITAG2_RECOVERED_YES) {
        *cnt = ((seed >> 12U) & 0xFF0U) | ((seed << 4U) & 0xF000U) | (seed >> 28U);
    } else {
        *cnt = cnt10;
    }
}

static void subghz_protocol_hitag2_check_remote_controller(SubGhzProtocolDecoderHitag2* instance) {
    hitag2_apply_check_remote(
        instance->generic.data,
        instance->generic.data_2,
        instance->recovered,
        instance->generic.seed,
        &instance->generic.serial,
        &instance->generic.btn,
        &instance->generic.cnt,
        &instance->hop,
        &instance->tail_bits);
}

static void
    hitag2_setup_button_editor(uint8_t btn, uint32_t cnt, uint8_t recovered, bool hell_key) {
    subghz_block_generic_global.current_btn = btn;
    if(subghz_block_generic_global.hitag2_replay_only) {
        subghz_block_generic_global.btn_is_available = false;
        subghz_block_generic_global.cnt_is_available = false;
        if(!subghz_custom_btn_has_original()) {
            subghz_custom_btn_set_original(btn);
        }
        subghz_custom_btn_set_max(1);
        return;
    }
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 4;
    if(hell_key) {
        subghz_block_generic_global.cnt_is_available = true;
        subghz_block_generic_global.cnt_length_bit = 10;
        subghz_block_generic_global.current_cnt = cnt & 0x3FFU;
    } else if(recovered == HITAG2_RECOVERED_YES) {
        subghz_block_generic_global.cnt_is_available = true;
        subghz_block_generic_global.cnt_length_bit = 16;
        subghz_block_generic_global.current_cnt = cnt;
    }
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(btn);
    }
    subghz_custom_btn_set_max(4);
}

static uint8_t hitag2_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_OK:
        return subghz_custom_btn_get_original();
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x8;
    default:
        return subghz_custom_btn_get_original();
    }
}

static inline void hitag2_encoder_apply_button(uint8_t* btn) {
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(*btn);
    }
    *btn = hitag2_get_btn_code();
    subghz_block_generic_global_button_override_get(btn);
}

static inline void hitag2_encoder_apply_counter(uint32_t* cnt) {
    if(!subghz_block_generic_global_counter_override_get(cnt)) {
        *cnt = (*cnt + furi_hal_subghz_get_rolling_counter_mult()) & 0xFFFFU;
    }
}

static bool hitag2_encoder_hell_next_frame(SubGhzProtocolEncoderHitag2* instance) {
    uint8_t raw[11];
    uint32_t uid = 0;
    uint8_t btn = 0;
    uint16_t cnt10 = 0;
    uint32_t hop = 0;
    uint8_t tail = 0;
    hitag2_unpack_frame(
        instance->generic.data, instance->generic.data_2, &uid, &btn, &cnt10, &hop, &tail);

    uint32_t epoch = instance->hitag2_epoch;
    if(!hitag2_hell_find_epoch(instance->hitag2_key, uid, btn, cnt10, hop, &epoch)) {
        return false;
    }

    uint8_t tx_btn = btn;
    hitag2_encoder_apply_button(&tx_btn);
    if(tx_btn == 0U) {
        tx_btn = btn;
    }

    uint32_t full = (epoch << 10U) | (uint32_t)(cnt10 & 0x3FFU);
    uint32_t override = full;
    if(!subghz_block_generic_global_counter_override_get(&override)) {
        full += (uint32_t)furi_hal_subghz_get_rolling_counter_mult();
    } else {
        full = (epoch << 10U) | (override & 0x3FFU);
    }
    epoch = (full >> 10U) & 0x3FFFFUL;
    cnt10 = (uint16_t)(full & 0x3FFU);
    hop = hitag2_hell_authenticator(uid, tx_btn, full, instance->hitag2_key);

    hitag2_pack_hell_frame(uid, tx_btn, cnt10, hop, tail, raw);
    hitag2_apply_raw(raw, &instance->generic.data, &instance->generic.data_2);
    instance->generic.serial = uid;
    instance->generic.btn = tx_btn;
    instance->generic.cnt = cnt10;
    instance->hitag2_epoch = epoch;
    instance->tail_bits = tail;
    instance->hell_key_valid = true;
    return true;
}

void* subghz_protocol_encoder_hitag2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHitag2* instance = calloc(1, sizeof(SubGhzProtocolEncoderHitag2));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_renault_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 1;
    instance->encoder.size_upload = HITAG2_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;

    return instance;
}

void subghz_protocol_encoder_hitag2_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static bool
    hitag2_encoder_add_level(LevelDuration* upload, size_t* index, bool level, uint32_t duration) {
    if(*index > 0 && level_duration_get_level(upload[*index - 1U]) == level) {
        const uint32_t prev = level_duration_get_duration(upload[*index - 1U]);
        upload[*index - 1U] = level_duration_make(level, prev + duration);
        return true;
    }
    if(*index >= HITAG2_UPLOAD_CAPACITY) {
        return false;
    }
    upload[(*index)++] = level_duration_make(level, duration);
    return true;
}

static bool hitag2_encoder_add_bits(
    LevelDuration* upload,
    size_t* index,
    uint64_t value,
    uint8_t bit_count) {
    for(uint8_t i = bit_count; i > 0; i--) {
        const bool one = ((value >> (i - 1U)) & 1ULL) != 0;
        if(one) {
            if(!hitag2_encoder_add_level(upload, index, true, HITAG2_TE_US)) return false;
            if(!hitag2_encoder_add_level(upload, index, false, HITAG2_TE_US)) return false;
        } else {
            if(!hitag2_encoder_add_level(upload, index, false, HITAG2_TE_US)) return false;
            if(!hitag2_encoder_add_level(upload, index, true, HITAG2_TE_US)) return false;
        }
    }
    return true;
}

static bool subghz_protocol_encoder_hitag2_get_upload(SubGhzProtocolEncoderHitag2* instance) {
    furi_check(instance);

    size_t index = 0;
    LevelDuration* upload = instance->encoder.upload;
    const uint64_t key = instance->generic.data;
    const uint64_t key_2 = instance->generic.data_2 & 0xFFFFFFULL;

    for(size_t i = 0; i < HITAG2_PREAMBLE_PAIRS; i++) {
        if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_TE_US)) return false;
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_TE_US)) return false;
    }
    if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_TE_US)) return false;

    const uint8_t long_frames = hitag2_variant_long_frames(instance->tx_variant);
    for(uint8_t frame = 0; frame < long_frames; frame++) {
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_HEADER_LOW_US)) return false;
        if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_HEADER_HIGH_US)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, 1, HITAG2_HEADER_BITS)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, key, HITAG2_KEY_BITS)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, key_2, HITAG2_KEY2_BITS)) return false;
    }

    if(instance->tx_variant != HITAG2_VARIANT_KANGOO) {
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_LONG_TO_SHORT_GAP_US))
            return false;
    }

    const uint64_t short_key = (key >> 18U) & 0x3FFULL;
    for(uint8_t frame = 0; frame < HITAG2_SHORT_FRAMES; frame++) {
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_HEADER_LOW_US)) return false;
        if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_HEADER_HIGH_US)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, 1, HITAG2_HEADER_BITS)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, short_key, HITAG2_SHORT_KEY_BITS))
            return false;
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_SHORT_GAP_US)) return false;
    }

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_hitag2_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        uint8_t key2[8] = {0};
        if(!hitag2_read_hex_be(flipper_format, "Key_2", key2, sizeof(key2))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->generic.data_2 = hitag2_bytes_to_u64_be(key2, sizeof(key2));

        if(!subghz_block_generic_global.hitag2_replay_only) {
            (void)renault_v1_apply_stored_secret(flipper_format);
        }
        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        uint32_t hop = 0;
        uint8_t tail = 0;
        hitag2_apply_check_remote(
            instance->generic.data,
            instance->generic.data_2,
            instance->recovered,
            instance->generic.seed,
            &instance->generic.serial,
            &instance->generic.btn,
            &instance->generic.cnt,
            &hop,
            &tail);
        instance->tail_bits = tail;
        UNUSED(hop);

        instance->hell_key_valid = false;
        memset(instance->hitag2_key, 0, 6U);
        instance->hitag2_epoch = 0;
        if(!subghz_block_generic_global.hitag2_replay_only &&
           hitag2_read_hell_key(flipper_format, instance->hitag2_key, &instance->hitag2_epoch)) {
            (void)hitag2_encoder_hell_next_frame(instance);
        }

        instance->tx_variant = hitag2_read_variant(flipper_format);

        hitag2_flipper_read_u32(flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat);

        hitag2_setup_button_editor(
            instance->generic.btn,
            instance->generic.cnt,
            instance->recovered,
            instance->hell_key_valid);

        if(!subghz_block_generic_global.hitag2_replay_only && !instance->hell_key_valid &&
           instance->recovered == HITAG2_RECOVERED_YES) {
            hitag2_encoder_apply_counter(&instance->generic.cnt);
            hitag2_encoder_apply_button(&instance->generic.btn);

            uint8_t out[11];
            uint8_t iv[4];
            hitag2_encrypt_frame(
                instance->generic.serial,
                instance->generic.cnt,
                instance->generic.btn,
                instance->generic.seed,
                out,
                iv);
            instance->generic.data = hitag2_bytes_to_u64_be(out, 8);
            instance->generic.data_2 = ((uint64_t)out[8] << 16U) | ((uint64_t)out[9] << 8U) |
                                       out[10];
            instance->generic.seed = ((uint32_t)iv[0] << 24U) | ((uint32_t)iv[1] << 16U) |
                                     ((uint32_t)iv[2] << 8U) | iv[3];
        }

        if(!subghz_protocol_encoder_hitag2_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data[8];
        hitag2_u64_to_bytes_be(instance->generic.data, key_data, 8);
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(key_data))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        hitag2_u64_to_bytes_be(instance->generic.data_2, key_data, 8);
        if(!flipper_format_update_hex(flipper_format, "Key_2", key_data, sizeof(key_data))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        if(instance->recovered == HITAG2_RECOVERED_YES) {
            const uint8_t recovered_hex = HITAG2_RECOVERED_YES;
            if(!flipper_format_insert_or_update_hex(
                   flipper_format, "Recovered", &recovered_hex, 1)) {
                ret = SubGhzProtocolStatusErrorParserOthers;
                break;
            }
            if(!hitag2_write_hex_be(flipper_format, "Seed", instance->generic.seed, 4)) {
                ret = SubGhzProtocolStatusErrorParserOthers;
                break;
            }
        }

        {
            uint32_t hop = 0;
            hitag2_unpack_frame(
                instance->generic.data, instance->generic.data_2, NULL, NULL, NULL, &hop, NULL);
            hitag2_write_named_fields(
                flipper_format,
                instance->generic.serial,
                instance->generic.btn,
                instance->generic.cnt,
                hop);
        }
        if(instance->hell_key_valid) {
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(
                flipper_format, HITAG2_HELL_KEY_FIELD, instance->hitag2_key, 6U);
            hitag2_flipper_u32(
                flipper_format, HITAG2_HELL_EPOCH_FIELD, instance->hitag2_epoch & 0x3FFFFUL);
        }
        hitag2_flipper_u32(flipper_format, HITAG2_TYPE_FIELD, instance->tx_variant);

        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_hitag2_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_hitag2_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}

void* subghz_protocol_decoder_hitag2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHitag2* instance = calloc(1, sizeof(SubGhzProtocolDecoderHitag2));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_renault_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_hitag2_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_hitag2_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    hitag2_decoder_notify(instance);
}

static bool hitag2_accept_frame(SubGhzProtocolDecoderHitag2* instance, uint64_t key_2) {
    if(instance->header != 1U) {
        return false;
    }

    uint8_t raw[11];
    hitag2_pack_key_bytes(instance->generic.data, key_2, raw);
    if(hitag2_frame_xor(raw) != raw[10]) {
        return false;
    }

    if(instance->last_frame_valid && instance->last_data == instance->generic.data &&
       instance->last_data_2 == key_2) {
        if(instance->identical_count < 0xFFU) {
            instance->identical_count++;
        }
        instance->tx_variant = (instance->identical_count >= HITAG2_KANGOO_LONG_FRAMES) ?
                                   HITAG2_VARIANT_KANGOO :
                                   HITAG2_VARIANT_OLD;
        return false;
    }

    instance->generic.data_2 = key_2;
    instance->generic.data_count_bit = HITAG2_MIN_COUNT_BIT;
    instance->recovered = 0;
    instance->generic.seed = 0;
    uint16_t cnt10 = 0;
    hitag2_unpack_frame(
        instance->generic.data,
        instance->generic.data_2,
        &instance->generic.serial,
        &instance->generic.btn,
        &cnt10,
        &instance->hop,
        &instance->tail_bits);
    instance->generic.cnt = cnt10;
    instance->last_data = instance->generic.data;
    instance->last_data_2 = key_2;
    instance->last_frame_valid = true;
    instance->identical_count = 1;
    instance->tx_variant = HITAG2_DEFAULT_VARIANT;
    instance->pending_notify = true;
    FURI_LOG_D(
        TAG,
        "Decode Sn:%08lX Btn:%X Cnt:%03X Hop:%08lX",
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        instance->generic.cnt & 0x3FFU,
        (unsigned long)instance->hop);
    hitag2_decoder_notify(instance);
    return true;
}

static uint8_t subghz_protocol_decoder_hitag2_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    uint8_t hash = 0;
    const uint8_t* p = (const uint8_t*)&instance->header;
    hash ^= p[0];
    hash ^= p[1];
    p = (const uint8_t*)&instance->generic.data;
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        hash ^= p[i];
    }
    p = (const uint8_t*)&instance->generic.data_2;
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        hash ^= p[i];
    }
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_hitag2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    subghz_protocol_hitag2_check_remote_controller(instance);

    const uint32_t serial = instance->generic.serial;
    const uint8_t btn = instance->generic.btn;
    const uint32_t cnt = instance->generic.cnt;
    const uint32_t seed = instance->generic.seed;
    const uint64_t data_2 = instance->generic.data_2;
    instance->generic.serial = 0;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;
    instance->generic.seed = 0;
    instance->generic.data_2 = 0;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = cnt;
    instance->generic.seed = seed;
    instance->generic.data_2 = data_2;
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    ret = hitag2_write_extra_fields(
        flipper_format, instance->generic.data_2, instance->recovered, instance->generic.seed);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }
    hitag2_write_named_fields(flipper_format, serial, btn, cnt, instance->hop);
    hitag2_flipper_u32(flipper_format, HITAG2_TYPE_FIELD, instance->tx_variant);
    if(instance->hell_key_valid) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(
            flipper_format, HITAG2_HELL_KEY_FIELD, instance->hitag2_key, 6U);
        hitag2_flipper_u32(
            flipper_format, HITAG2_HELL_EPOCH_FIELD, instance->hitag2_epoch & 0x3FFFFUL);
    }
    const bool had_secret = instance->hell_key_valid ||
                            instance->recovered == HITAG2_RECOVERED_YES;
    if(renault_v1_apply_stored_secret(flipper_format) && !had_secret) {
        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);
        if(hitag2_read_hell_key(flipper_format, instance->hitag2_key, &instance->hitag2_epoch)) {
            instance->hell_key_valid = hitag2_hell_find_epoch(
                instance->hitag2_key,
                serial,
                btn,
                (uint16_t)(cnt & 0x3FFU),
                instance->hop,
                &instance->hitag2_epoch);
        }
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_hitag2_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        uint8_t key2[8] = {0};
        if(!hitag2_read_hex_be(flipper_format, "Key_2", key2, sizeof(key2))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->generic.data_2 = hitag2_bytes_to_u64_be(key2, sizeof(key2));

        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);

        subghz_protocol_hitag2_check_remote_controller(instance);
        (void)renault_v1_apply_stored_secret(flipper_format);
        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);
        subghz_protocol_hitag2_check_remote_controller(instance);
        instance->hell_key_valid = false;
        memset(instance->hitag2_key, 0, 6U);
        instance->hitag2_epoch = 0;
        if(hitag2_read_hell_key(flipper_format, instance->hitag2_key, &instance->hitag2_epoch)) {
            instance->hell_key_valid = hitag2_hell_find_epoch(
                instance->hitag2_key,
                instance->generic.serial,
                instance->generic.btn,
                (uint16_t)(instance->generic.cnt & 0x3FFU),
                instance->hop,
                &instance->hitag2_epoch);
        }
        instance->tx_variant = hitag2_read_variant(flipper_format);
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_decoder_hitag2_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;

    subghz_protocol_hitag2_check_remote_controller(instance);
    hitag2_setup_button_editor(
        instance->generic.btn,
        instance->generic.cnt,
        instance->recovered,
        instance->hell_key_valid);

    furi_string_printf(
        output,
        "%s\r\nK1:%016llX\r\nK2:%06llX Sn:%08lX\r\nBtn:%02X [%s] %db T:%u",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long long)(instance->generic.data_2 & 0xFFFFFFULL),
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        hitag2_get_button_name(instance->generic.btn),
        instance->generic.data_count_bit,
        instance->tx_variant);

    if(instance->hell_key_valid) {
        furi_string_cat_printf(
            output,
            "\r\nCnt:%03lX Hop:%08lX\r\nHell",
            (unsigned long)(instance->generic.cnt & 0x3FFU),
            (unsigned long)instance->hop);
    } else if(instance->recovered == HITAG2_RECOVERED_YES) {
        furi_string_cat_printf(
            output,
            "\r\nIV:%08lX Cnt:%04lX",
            (unsigned long)instance->generic.seed,
            (unsigned long)(instance->generic.cnt & 0xFFFFU));
    } else if(instance->recovered == HITAG2_RECOVERED_BF_MISS) {
        furi_string_cat_printf(output, "\r\nBF not found");
    } else {
        furi_string_cat_printf(
            output,
            "\r\nCnt:%03lX Hop:%08lX\r\nHell (key needed)",
            (unsigned long)(instance->generic.cnt & 0x3FFU),
            (unsigned long)instance->hop);
    }
}

bool renault_v1_family_frame(void* context, const uint8_t raw[13]) {
    SubGhzProtocolDecoderHitag2* instance = context;
    instance->header = ((uint16_t)raw[0] << 8) | raw[1];
    instance->generic.data = hitag2_bytes_to_u64_be(raw + 2, 8);
    return hitag2_accept_frame(instance, hitag2_bytes_to_u64_be(raw + 10, 3));
}
void renault_v1_family_raw(void* context, uint8_t raw[14]) {
    SubGhzProtocolDecoderHitag2* instance = context;
    memset(raw, 0, 14);
    raw[1] = 1;
    hitag2_pack_key_bytes(instance->generic.data, instance->generic.data_2, raw + 2);
}

#undef TAG

typedef enum {
    HitagFiat1,
    HitagRenault,
    HitagFiat2
} HitagFormat;

typedef struct {
    uint8_t state, count;
    uint16_t header, te_high, te_low;
    uint32_t low_sync, high_sync;
    uint64_t bits, key;
    ManchesterState manchester;
} HitagAdaptive;

typedef struct {
    uint8_t cells[28];
    uint16_t count;
    uint8_t state, variant;
    uint32_t lead;
} HitagCells;

typedef struct {
    uint8_t raw[14];
    HitagFormat format;
    uint8_t variant;
} HitagFrame;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzEnvironment* environment;
    HitagAdaptive adaptive[2];
    HitagCells cells[2];
    HitagFrame pending[4];
    uint8_t pending_count;
    SubGhzProtocolDecoderBase* selected;
    HitagFrame last;
    bool last_valid, feeding, reset_pending, notified;
} Hitag2Family;

static const SubGhzProtocol* const formats[] = {
    &subghz_protocol_fiat_v1,
    &subghz_protocol_renault_v1,
    &subghz_protocol_fiat_v2};

bool subghz_protocol_hitag2_member(const SubGhzProtocol* protocol) {
    for(size_t i = 0; i < COUNT_OF(formats); i++)
        if(protocol == formats[i]) return true;
    return false;
}

static size_t frame_size(HitagFormat format) {
    return format == HitagFiat2 ? 14 : 13;
}

static void family_reset(void* context) {
    Hitag2Family* instance = context;
    if(instance->feeding) {
        instance->reset_pending = true;
        return;
    }
    memset(instance->cells, 0, sizeof(instance->cells));
    for(size_t i = 0; i < COUNT_OF(instance->adaptive); i++) {
        instance->adaptive[i].state = 0;
        instance->adaptive[i].manchester = ManchesterStateMid1;
    }
    if(instance->selected) instance->selected->protocol->decoder->reset(instance->selected);
    instance->pending_count = 0;
    instance->reset_pending = false;
}

static void family_selected(SubGhzProtocolDecoderBase* base, void* context) {
    UNUSED(base);
    ((Hitag2Family*)context)->notified = true;
}

static void* family_alloc(SubGhzEnvironment* environment) {
    Hitag2Family* instance = calloc(1, sizeof(Hitag2Family));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_hitag2;
    instance->environment = environment;
    family_reset(instance);
    return instance;
}

static void family_free(void* context) {
    Hitag2Family* instance = context;
    if(instance->selected) instance->selected->protocol->decoder->free(instance->selected);
    free(instance);
}

static bool frame_valid(HitagFormat format, const uint8_t* raw) {
    if(format == HitagFiat1) return fiat_v1_family_valid(raw);
    if(format == HitagFiat2) return fiat_v2_family_valid(raw);
    if(raw[0] != 0 || raw[1] != 1) return false;
    uint8_t check = 0;
    for(size_t i = 2; i < 12; i++)
        check ^= raw[i];
    return check == raw[12];
}

static bool
    queue_frame(Hitag2Family* instance, HitagFormat format, uint8_t variant, const uint8_t* raw) {
    if(!frame_valid(format, raw)) return false;
    size_t size = frame_size(format);
    for(size_t i = 0; i < instance->pending_count; i++) {
        HitagFrame* frame = &instance->pending[i];
        if(frame_size(frame->format) == size && memcmp(frame->raw, raw, size) == 0) return true;
    }
    furi_check(instance->pending_count < COUNT_OF(instance->pending));
    HitagFrame* frame = &instance->pending[instance->pending_count++];
    memset(frame, 0, sizeof(*frame));
    frame->format = format;
    frame->variant = variant;
    memcpy(frame->raw, raw, size);
    return true;
}

static uint32_t adaptive_threshold(uint16_t te) {
    uint32_t triple = (uint32_t)te * 3;
    if(triple < 300) return 150;
    if(triple >= 422) return 210;
    return triple / 2;
}

static uint16_t adaptive_te(uint16_t te, uint32_t duration) {
    uint32_t mixed = (uint32_t)te * 7 + duration;
    if(mixed < 560) return 70;
    if(mixed >= 1488) return 185;
    return mixed / 8;
}

static void adaptive_feed(Hitag2Family* instance, bool wide_sync, bool level, uint32_t duration) {
    HitagAdaptive* path = &instance->adaptive[wide_sync];
    const uint32_t low_min = wide_sync ? 1150 : 1400;
    const uint32_t high_min = wide_sync ? 800 : 920;
    while(true) {
        switch(path->state) {
        case 0:
            if(!level && duration >= low_min && duration <= 2200) {
                path->low_sync = duration;
                path->state = 2;
            }
            return;
        case 2:
            if(!level) {
                path->state = 0;
                continue;
            }
            if(duration < high_min || duration > 1150 || path->low_sync < low_min ||
               path->low_sync > 2200) {
                path->state = 0;
                return;
            }
            path->high_sync = duration;
            path->bits = 0;
            path->count = 0;
            path->manchester = ManchesterStateStart1;
            path->te_high = 120;
            path->te_low = 150;
            path->state = 3;
            return;
        case 3: {
            if(duration <= 49) return;
            if(duration > 520) {
                path->state = 0;
                continue;
            }
            uint16_t* te = level ? &path->te_high : &path->te_low;
            ManchesterEvent event;
            if(duration > adaptive_threshold(*te)) {
                event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
            } else {
                event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                *te = adaptive_te(*te, duration);
            }
            bool bit = false;
            if(!manchester_advance(path->manchester, event, &path->manchester, &bit)) return;
            path->bits = (path->bits << 1) | bit;
            path->count++;
            if(path->count == 16) {
                path->header = (uint16_t)~path->bits;
                path->bits = 0;
            } else if(path->count == 80) {
                path->key = ~path->bits;
                path->bits = 0;
            } else if(path->count == 104) {
                if(path->header == 1 && (!wide_sync || ((path->key >> 32) & 0xF0) != 0x30)) {
                    uint8_t raw[13] = {0, 1};
                    for(size_t i = 0; i < 8; i++)
                        raw[i + 2] = path->key >> ((7 - i) * 8);
                    uint32_t tail = (uint32_t)~path->bits;
                    raw[10] = tail >> 16;
                    raw[11] = tail >> 8;
                    raw[12] = tail;
                    if(!wide_sync || path->high_sync > 949 ||
                       !queue_frame(instance, HitagFiat1, 1, raw))
                        queue_frame(instance, HitagRenault, 0, raw);
                }
                path->bits = 0;
                path->count = 0;
            }
            return;
        }
        default:
            return;
        }
    }
}

static bool near(uint32_t duration, uint32_t nominal, uint32_t delta) {
    return (duration > nominal ? duration - nominal : nominal - duration) < delta;
}

static unsigned cell_units(uint8_t variant, bool fiat2, uint32_t duration) {
    uint32_t te = fiat2 ? 210 : variant ? 100 : 250;
    uint32_t delta = !fiat2 && variant ? 50 : 100;
    if(near(duration, te, delta)) return 1;
    if(near(duration, te * 2, delta)) return 2;
    return 0;
}

static bool cells_decode(Hitag2Family* instance, HitagCells* path, bool fiat2) {
    const unsigned bits = fiat2 ? 112 : 104;
    if(path->count != bits * 2) return false;
    uint8_t raw[14] = {0};
    for(unsigned bit = 0; bit < bits; bit++) {
        unsigned i = bit * 2;
        bool first = (path->cells[i >> 3] >> (i & 7)) & 1;
        i++;
        bool second = (path->cells[i >> 3] >> (i & 7)) & 1;
        if(first == second) return false;
        if(first) raw[bit >> 3] |= 1 << (7 - (bit & 7));
    }
    HitagFormat format = fiat2 ? HitagFiat2 : HitagFiat1;
    if(queue_frame(instance, format, path->variant, raw)) return true;
    for(unsigned i = 0; i < bits / 8; i++)
        raw[i] = (uint8_t)~raw[i];
    return queue_frame(instance, format, path->variant, raw);
}

static void cells_push(Hitag2Family* instance, HitagCells* path, bool fiat2, bool level) {
    unsigned limit = fiat2 ? 224 : 208;
    if(path->count >= limit) {
        if(fiat2) return;
        unsigned bytes = limit / 8;
        for(unsigned i = 0; i + 1 < bytes; i++)
            path->cells[i] = (path->cells[i] >> 1) | (path->cells[i + 1] << 7);
        path->cells[bytes - 1] >>= 1;
    } else {
        path->count++;
    }
    unsigned i = path->count - 1;
    if(level)
        path->cells[i >> 3] |= 1 << (i & 7);
    else
        path->cells[i >> 3] &= ~(1 << (i & 7));
    if(!fiat2) cells_decode(instance, path, false);
}

static void cells_feed(Hitag2Family* instance, bool fiat2, bool level, uint32_t duration) {
    HitagCells* path = &instance->cells[fiat2];
    if(fiat2) {
        unsigned units = cell_units(0, true, duration);
        if(path->state == 0) {
            if(!level && duration >= 900) path->state = 1;
            return;
        }
        if(!units) {
            if(path->state != 1 || level || duration < 900) path->state = 0;
            memset(path->cells, 0, sizeof(path->cells));
            path->count = 0;
            return;
        }
        path->state = 2;
        for(unsigned i = 0; i < units; i++)
            cells_push(instance, path, true, level);
        if(path->count == 224) {
            cells_decode(instance, path, true);
            memset(path, 0, sizeof(*path));
        }
        return;
    }

    bool any_pulse = cell_units(0, false, duration) || cell_units(1, false, duration);
    if(path->state == 0) {
        if(!any_pulse) {
            path->lead = level ? duration : 0;
            return;
        }
        if(near(path->lead, 2033, 100))
            path->variant = 0;
        else if(near(path->lead, 850, 100))
            path->variant = 1;
        else
            return;
        if(!cell_units(path->variant, false, duration)) return;
        path->count = 0;
        memset(path->cells, 0, sizeof(path->cells));
        path->state = 1;
    }
    unsigned units = cell_units(path->variant, false, duration);
    if(units) {
        for(unsigned i = 0; i < units; i++)
            cells_push(instance, path, false, level);
    } else {
        if(!level && duration >= 800) cells_push(instance, path, false, false);
        path->count = 0;
        memset(path->cells, 0, sizeof(path->cells));
        path->state = 0;
        if(!any_pulse) path->lead = level ? duration : 0;
    }
}

static void family_feed(void* context, bool level, uint32_t duration) {
    Hitag2Family* instance = context;
    instance->feeding = true;
    instance->pending_count = 0;
    cells_feed(instance, false, level, duration);
    adaptive_feed(instance, false, level, duration);
    cells_feed(instance, true, level, duration);
    adaptive_feed(instance, true, level, duration);
    for(size_t i = 0; i < instance->pending_count; i++) {
        HitagFrame* frame = &instance->pending[i];
        const SubGhzProtocol* protocol = formats[frame->format];
        if(!instance->selected || instance->selected->protocol != protocol) {
            if(instance->selected) instance->selected->protocol->decoder->free(instance->selected);
            instance->selected = protocol->decoder->alloc(instance->environment);
            furi_check(instance->selected);
            subghz_protocol_decoder_base_set_decoder_callback(
                instance->selected, family_selected, instance);
        }
        instance->notified = false;
        if(frame->format == HitagFiat1)
            fiat_v1_family_frame(instance->selected, frame->raw, frame->variant);
        else if(frame->format == HitagRenault)
            renault_v1_family_frame(instance->selected, frame->raw);
        else
            fiat_v2_family_frame(instance->selected, frame->raw);
        size_t size = frame_size(frame->format);
        bool duplicate = instance->last_valid && frame_size(instance->last.format) == size &&
                         memcmp(instance->last.raw, frame->raw, size) == 0;
        if(instance->notified && !duplicate) {
            instance->last = *frame;
            instance->last_valid = true;
            if(instance->base.callback)
                instance->base.callback(instance->selected, instance->base.context);
        }
    }
    instance->feeding = false;
    if(instance->reset_pending) family_reset(instance);
}

static const SubGhzProtocolDecoder family_decoder =
    {.alloc = family_alloc, .free = family_free, .feed = family_feed, .reset = family_reset};

const SubGhzProtocol subghz_protocol_hitag2 = {
    .name = SUBGHZ_PROTOCOL_HITAG2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868 |
            SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable,
    .decoder = &family_decoder,
};
