#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/registry.h>
#include "fiat_v1.h"
#include "hitag2_key_db.h"
#include <furi_hal.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t
    fiat_v1_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void fiat_v1_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t fiat_v1_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void
    fiat_v1_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static SubGhzProtocolStatus
    fiat_v1_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void fiat_v1_encoder_read_fields(
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

static uint32_t fiat_v1_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    fiat_v1_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
    if(!ff || !protocol_name || !suffix) return SubGhzProtocolStatusError;
    FuriString* display = furi_string_alloc();
    if(!display) return SubGhzProtocolStatusError;
    furi_string_printf(display, "%s - %s", protocol_name, suffix);
    flipper_format_rewind(ff);
    SubGhzProtocolStatus status =
        flipper_format_insert_or_update_string_cstr(ff, "Disp", furi_string_get_cstr(display)) ?
            SubGhzProtocolStatusOk :
            SubGhzProtocolStatusErrorParserOthers;
    furi_string_free(display);
    return status;
}

static SubGhzProtocolStatus fiat_v1_encoder_read_bit(
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
        for(size_t i = 0; i < allowed_bits_count; i++)
            if((uint32_t)allowed_bits[i] == bit) {
                ok = true;
                break;
            }
        if(!ok) return SubGhzProtocolStatusError;
    }
    *out_bit = bit;
    return SubGhzProtocolStatusOk;
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
#pragma GCC diagnostic pop

#define TAG "FiatProtocolV1"

#define FIAT_V1_TE_SHORT           250U
#define FIAT_V1_TE_LONG            500U
#define FIAT_V1_TE_DELTA           100U
#define FIAT_V1_TE_B_SHORT         100U
#define FIAT_V1_TE_B_LONG          200U
#define FIAT_V1_TE_B_DELTA         50U
#define FIAT_V1_WIRE_BITS          104U
#define FIAT_V1_WIRE_BYTES         13U
#define FIAT_V1_WIRE_CELLS         (FIAT_V1_WIRE_BITS * 2U)
#define FIAT_V1_LOGICAL_BITS       102U
#define FIAT_V1_VARIANT_COUNT      2U
#define FIAT_V1_BOUNDARY_MIN_US    800U
#define FIAT_V1_DEFAULT_TAIL_BITS  2U
#define FIAT_V1_RAW_FIELD          "Raw"
#define FIAT_V1_HOP_FIELD          "Hop"
#define FIAT_V1_TAIL_BITS_FIELD    "Tail Bits"
#define FIAT_V1_XOR_FIELD          "XOR"
#define FIAT_V1_HITAG2_KEY_FIELD   "Hitag2 Key"
#define FIAT_V1_HITAG2_EPOCH_FIELD "Hitag2 Epoch"
#define FIAT_V1_KNOWN_KEY_COUNT    9U

#define FIAT_V1_TE_VARIANT_A       0U
#define FIAT_V1_TE_VARIANT_B       1U
#define FIAT_V1_DEFAULT_TE_VARIANT FIAT_V1_TE_VARIANT_B
#define FIAT_V1_ENC_LEAD_A_US      2033U
#define FIAT_V1_ENC_GAP_A_US       3252U
#define FIAT_V1_ENC_LEAD_B_US      850U
#define FIAT_V1_ENC_GAP_B_US       1230U
#define FIAT_V1_LEAD_TOLERANCE_US  100U
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

typedef enum {
    FiatV1DecoderStepReset = 0,
    FiatV1DecoderStepData = 1,
} FiatV1DecoderStep;

struct SubGhzProtocolDecoderFiatV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint8_t cells[FIAT_V1_VARIANT_COUNT][FIAT_V1_WIRE_CELLS];
    uint16_t cell_count[FIAT_V1_VARIANT_COUNT];

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
    uint32_t last_lead_us;
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

static bool
    fiat_v1_feed_data_pulse(SubGhzProtocolDecoderFiatV1* instance, bool level, uint32_t duration);
static bool fiat_v1_frame_valid(const uint8_t raw[FIAT_V1_WIRE_BYTES]);
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
    .feed = subghz_protocol_decoder_fiat_v1_feed,
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
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_fiat_v1_decoder,
    .encoder = &subghz_protocol_fiat_v1_encoder,
};

static bool fiat_v1_duration_is_short(uint8_t variant, uint32_t duration) {
    return DURATION_DIFF(duration, fiat_v1_variant_const(variant)->te_short) <
           (int32_t)fiat_v1_variant_const(variant)->te_delta;
}

static bool fiat_v1_duration_is_long(uint8_t variant, uint32_t duration) {
    return DURATION_DIFF(duration, fiat_v1_variant_const(variant)->te_long) <
           (int32_t)fiat_v1_variant_const(variant)->te_delta;
}

static bool fiat_v1_duration_is_pulse(uint32_t duration) {
    for(uint8_t variant = 0U; variant < FIAT_V1_VARIANT_COUNT; variant++) {
        if(fiat_v1_duration_is_short(variant, duration) ||
           fiat_v1_duration_is_long(variant, duration)) {
            return true;
        }
    }
    return false;
}

static bool fiat_v1_duration_is_lead(uint8_t variant, uint32_t duration) {
    return DURATION_DIFF(duration, fiat_v1_enc_lead_us(variant)) <
           (int32_t)FIAT_V1_LEAD_TOLERANCE_US;
}

static bool fiat_v1_lead_armed(const SubGhzProtocolDecoderFiatV1* instance, uint8_t variant) {
    return instance->last_lead_us != 0U &&
           fiat_v1_duration_is_lead(variant, instance->last_lead_us);
}

static void
    fiat_v1_note_boundary(SubGhzProtocolDecoderFiatV1* instance, bool level, uint32_t duration) {
    if(fiat_v1_duration_is_pulse(duration)) {
        return;
    }
    instance->last_lead_us = level ? duration : 0U;
}

static bool fiat_v1_button_valid(uint8_t button) {
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

static uint32_t fiat_v1_uid(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    return ((uint32_t)raw[2] << 24U) | ((uint32_t)raw[3] << 16U) | ((uint32_t)raw[4] << 8U) |
           raw[5];
}

static uint32_t fiat_v1_counter(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    return ((uint32_t)(raw[6] & 0x0FU) << 6U) | (raw[7] >> 2U);
}

static uint32_t fiat_v1_hop(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
    return ((uint32_t)(raw[7] & 0x03U) << 30U) | ((uint32_t)raw[8] << 22U) |
           ((uint32_t)raw[9] << 14U) | ((uint32_t)raw[10] << 6U) | (raw[11] >> 2U);
}

static bool fiat_v1_frame_valid(const uint8_t raw[FIAT_V1_WIRE_BYTES]) {
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

static void fiat_v1_clear_cells(SubGhzProtocolDecoderFiatV1* instance, uint8_t variant) {
    instance->cell_count[variant] = 0U;
    memset(instance->cells[variant], 0, FIAT_V1_WIRE_CELLS);
}

static void fiat_v1_clear_all_cells(SubGhzProtocolDecoderFiatV1* instance) {
    for(uint8_t variant = 0U; variant < FIAT_V1_VARIANT_COUNT; variant++) {
        fiat_v1_clear_cells(instance, variant);
    }
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
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = instance->generic.data_count_bit;
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

static bool
    fiat_v1_try_decode_window(SubGhzProtocolDecoderFiatV1* instance, uint8_t variant, bool invert) {
    if(instance->cell_count[variant] != FIAT_V1_WIRE_CELLS) {
        return false;
    }

    const uint8_t* cells = instance->cells[variant];
    uint8_t raw[FIAT_V1_WIRE_BYTES] = {0};
    for(uint8_t bit_index = 0U; bit_index < FIAT_V1_WIRE_BITS; bit_index++) {
        const uint8_t first = cells[bit_index * 2U];
        const uint8_t second = cells[bit_index * 2U + 1U];
        if(first == second) {
            return false;
        }

        bool bit = first != 0U;
        if(invert) {
            bit = !bit;
        }
        if(bit) {
            raw[bit_index >> 3U] |= (uint8_t)(1U << (7U - (bit_index & 7U)));
        }
    }

    return fiat_v1_commit(instance, raw, variant);
}

static void fiat_v1_try_decode(SubGhzProtocolDecoderFiatV1* instance, uint8_t variant) {
    if(fiat_v1_try_decode_window(instance, variant, false)) {
        return;
    }
    (void)fiat_v1_try_decode_window(instance, variant, true);
}

static void fiat_v1_push_cell(SubGhzProtocolDecoderFiatV1* instance, uint8_t variant, bool level) {
    uint8_t* cells = instance->cells[variant];
    if(instance->cell_count[variant] < FIAT_V1_WIRE_CELLS) {
        cells[instance->cell_count[variant]++] = level ? 1U : 0U;
    } else {
        memmove(cells, &cells[1], FIAT_V1_WIRE_CELLS - 1U);
        cells[FIAT_V1_WIRE_CELLS - 1U] = level ? 1U : 0U;
    }
    fiat_v1_try_decode(instance, variant);
}

static bool
    fiat_v1_feed_data_pulse(SubGhzProtocolDecoderFiatV1* instance, bool level, uint32_t duration) {
    bool matched = false;

    for(uint8_t variant = 0U; variant < FIAT_V1_VARIANT_COUNT; variant++) {
        if(!fiat_v1_lead_armed(instance, variant)) {
            continue;
        }
        if(fiat_v1_duration_is_short(variant, duration)) {
            fiat_v1_push_cell(instance, variant, level);
            matched = true;
        } else if(fiat_v1_duration_is_long(variant, duration)) {
            fiat_v1_push_cell(instance, variant, level);
            fiat_v1_push_cell(instance, variant, level);
            matched = true;
        } else {
            if(!level && duration >= FIAT_V1_BOUNDARY_MIN_US) {
                fiat_v1_push_cell(instance, variant, false);
            }
            fiat_v1_clear_cells(instance, variant);
        }
    }

    return matched;
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

static uint8_t fiat_v1_truth(uint32_t table, uint8_t index) {
    return (uint8_t)((table >> index) & 1U);
}

static uint8_t fiat_v1_filter_index(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint8_t)((a << 3U) | (b << 2U) | (c << 1U) | d);
}

static uint8_t fiat_v1_byte_bit(uint8_t byte, uint8_t bit) {
    return (uint8_t)((byte >> bit) & 1U);
}

static uint8_t fiat_v1_bcm_hitag2_filter(const uint8_t state[6]) {
    uint8_t group = 0U;
    group |= fiat_v1_truth(
        0x2c79U,
        fiat_v1_filter_index(
            fiat_v1_byte_bit(state[0], 1U),
            fiat_v1_byte_bit(state[0], 2U),
            fiat_v1_byte_bit(state[0], 4U),
            fiat_v1_byte_bit(state[0], 5U)));
    group |= (uint8_t)(fiat_v1_truth(
                           0x6671U,
                           fiat_v1_filter_index(
                               fiat_v1_byte_bit(state[1], 0U),
                               fiat_v1_byte_bit(state[1], 1U),
                               fiat_v1_byte_bit(state[1], 3U),
                               fiat_v1_byte_bit(state[1], 7U)))
                       << 1U);
    group |= (uint8_t)(fiat_v1_truth(
                           0x6671U,
                           fiat_v1_filter_index(
                               fiat_v1_byte_bit(state[3], 5U),
                               fiat_v1_byte_bit(state[2], 0U),
                               fiat_v1_byte_bit(state[2], 2U),
                               fiat_v1_byte_bit(state[2], 6U)))
                       << 2U);
    group |= (uint8_t)(fiat_v1_truth(
                           0x6671U,
                           fiat_v1_filter_index(
                               fiat_v1_byte_bit(state[4], 6U),
                               fiat_v1_byte_bit(state[3], 0U),
                               fiat_v1_byte_bit(state[3], 2U),
                               fiat_v1_byte_bit(state[3], 3U)))
                       << 3U);
    group |= (uint8_t)(fiat_v1_truth(
                           0x2c79U,
                           fiat_v1_filter_index(
                               fiat_v1_byte_bit(state[5], 1U),
                               fiat_v1_byte_bit(state[5], 3U),
                               fiat_v1_byte_bit(state[5], 4U),
                               fiat_v1_byte_bit(state[4], 5U)))
                       << 4U);
    return fiat_v1_truth(0x7907287bUL, group);
}

static uint8_t fiat_v1_parity8(uint8_t value) {
    value ^= (uint8_t)(value >> 4U);
    value ^= (uint8_t)(value >> 2U);
    value ^= (uint8_t)(value >> 1U);
    return value & 1U;
}

static uint8_t fiat_v1_bcm_hitag2_feedback(const uint8_t state[6]) {
    static const uint8_t masks[6] = {0xb3U, 0x80U, 0x83U, 0x22U, 0x00U, 0x73U};
    uint8_t feedback = 0U;
    for(uint8_t i = 0U; i < 6U; i++) {
        feedback ^= fiat_v1_parity8((uint8_t)(state[i] & masks[i]));
    }
    return feedback & 1U;
}

static void fiat_v1_bcm_hitag2_shift(uint8_t state[6], uint8_t input) {
    for(uint8_t i = 0U; i < 5U; i++) {
        state[i] = (uint8_t)((state[i] << 1U) | (state[i + 1U] >> 7U));
    }
    state[5] = (uint8_t)((state[5] << 1U) | (input & 1U));
}

static uint8_t fiat_v1_input_bit_u32_be(uint32_t value, uint8_t index) {
    return (uint8_t)((value >> (31U - index)) & 1U);
}

static uint8_t fiat_v1_input_bit_bytes_be(const uint8_t* bytes, uint8_t index) {
    return (uint8_t)((bytes[index >> 3U] >> (7U - (index & 7U))) & 1U);
}

static uint32_t fiat_v1_bcm_generate_authenticator(
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    const uint8_t key[6],
    uint32_t epoch) {
    uint8_t state[6] = {
        (uint8_t)(uid >> 24U),
        (uint8_t)(uid >> 16U),
        (uint8_t)(uid >> 8U),
        (uint8_t)uid,
        key[4],
        key[5],
    };

    const uint32_t iv = ((epoch & 0x3FFFFUL) << 14U) | (((uint32_t)control & 0x03FFUL) << 4U) |
                        ((uint32_t)button & 0x0FUL);

    for(uint8_t i = 0U; i < 32U; i++) {
        const uint8_t input = fiat_v1_input_bit_u32_be(iv, i) ^
                              fiat_v1_input_bit_bytes_be(key, i) ^
                              fiat_v1_bcm_hitag2_filter(state);
        fiat_v1_bcm_hitag2_shift(state, input);
    }

    uint32_t authenticator = 0U;
    for(uint8_t i = 0U; i < 32U; i++) {
        authenticator = (authenticator << 1U) | fiat_v1_bcm_hitag2_filter(state);
        fiat_v1_bcm_hitag2_shift(state, fiat_v1_bcm_hitag2_feedback(state));
    }
    return authenticator;
}

static const uint8_t fiat_v1_known_keys[FIAT_V1_KNOWN_KEY_COUNT][6] = {
    {0xB7U, 0x92U, 0x80U, 0xAEU, 0xCCU, 0x37U},
    {0xD4U, 0x24U, 0x28U, 0xF7U, 0xD9U, 0x66U},
    {0x4DU, 0x34U, 0x3FU, 0xD4U, 0xE7U, 0xB6U},
    {0x6DU, 0x6BU, 0xF2U, 0x1DU, 0x3AU, 0x1AU},
    {0xA3U, 0xF3U, 0xACU, 0xF7U, 0xB9U, 0x10U},
    {0x4DU, 0x49U, 0x4BU, 0x52U, 0x4FU, 0x4EU},
    {0xCDU, 0x49U, 0x4BU, 0x52U, 0x4FU, 0x4EU},
    {0x33U, 0xFAU, 0x2FU, 0xCDU, 0xC3U, 0x3BU},
    {0xF6U, 0x1AU, 0xEFU, 0x9CU, 0xD0U, 0x1BU},
};

static bool fiat_v1_key_matches(
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t hop,
    const uint8_t key[6],
    uint32_t epoch) {
    return fiat_v1_bcm_generate_authenticator(uid, button, control, key, epoch) == hop;
}

static bool fiat_v1_parse_frame_from_flipper(
    FlipperFormat* ff,
    uint32_t* uid,
    uint8_t* button,
    uint16_t* control,
    uint32_t* hop) {
    if(!ff || !uid || !button || !control || !hop) return false;

    uint8_t raw[FIAT_V1_WIRE_BYTES] = {0};
    flipper_format_rewind(ff);
    if(flipper_format_read_hex(ff, FIAT_V1_RAW_FIELD, raw, sizeof(raw)) &&
       fiat_v1_frame_valid(raw)) {
        *uid = fiat_v1_uid(raw);
        *button = raw[6] >> 4U;
        *control = (uint16_t)fiat_v1_counter(raw);
        *hop = fiat_v1_hop(raw);
        return true;
    }

    uint32_t serial = 0U;
    uint32_t btn = 0U;
    uint32_t cnt = 0U;
    uint32_t hop_val = 0U;
    flipper_format_rewind(ff);
    if(flipper_format_read_uint32(ff, "Serial", &serial, 1U)) {
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Btn", &btn, 1U)) {
            flipper_format_rewind(ff);
            if(flipper_format_read_uint32(ff, "Cnt", &cnt, 1U)) {
                flipper_format_rewind(ff);
                if(flipper_format_read_uint32(ff, FIAT_V1_HOP_FIELD, &hop_val, 1U)) {
                    *uid = serial;
                    *button = (uint8_t)btn;
                    *control = (uint16_t)(cnt & 0x03FFU);
                    *hop = hop_val;
                    return serial != 0U && serial != UINT32_MAX &&
                           fiat_v1_button_valid((uint8_t)btn);
                }
            }
        }
    }

    SubGhzBlockGeneric generic = {0};
    flipper_format_rewind(ff);
    if(subghz_block_generic_deserialize_check_count_bit(
           &generic, ff, subghz_protocol_fiat_v1_const.min_count_bit_for_found) !=
       SubGhzProtocolStatusOk) {
        return false;
    }
    *uid = (uint32_t)(generic.data >> 32U);
    *hop = (uint32_t)generic.data;
    *button = generic.btn;
    *control = (uint16_t)(generic.cnt & 0x03FFU);
    return *uid != 0U && *uid != UINT32_MAX && fiat_v1_button_valid(*button);
}

static bool fiat_v1_key_matches_any_button(
    uint32_t uid,
    uint16_t control,
    uint32_t hop,
    const uint8_t key[6],
    uint32_t epoch) {
    static const uint8_t buttons[] = {0x1U, 0x2U, 0x4U, 0x8U};
    for(size_t i = 0; i < COUNT_OF(buttons); i++) {
        if(fiat_v1_key_matches(uid, buttons[i], control, hop, key, epoch)) {
            return true;
        }
    }
    return false;
}

static bool fiat_v1_key_is_builtin(const uint8_t key[6]) {
    for(uint8_t i = 0U; i < FIAT_V1_KNOWN_KEY_COUNT; i++) {
        if(memcmp(key, fiat_v1_known_keys[i], 6U) == 0) return true;
    }
    return false;
}

static void fiat_v1_hitag2_key_disk_to_cipher(const uint8_t disk[6], uint8_t cipher[6]) {
    const uint8_t tmp[6] = {disk[2], disk[3], disk[4], disk[5], disk[0], disk[1]};
    memcpy(cipher, tmp, sizeof(tmp));
}

static void fiat_v1_hitag2_key_cipher_to_disk(const uint8_t cipher[6], uint8_t disk[6]) {
    const uint8_t tmp[6] = {cipher[4], cipher[5], cipher[0], cipher[1], cipher[2], cipher[3]};
    memcpy(disk, tmp, sizeof(tmp));
}

static void fiat_v1_store_hitag2_key(FlipperFormat* ff, const uint8_t key[6], uint32_t epoch) {
    if(!ff || !key) return;
    uint8_t disk_key[6];
    fiat_v1_hitag2_key_cipher_to_disk(key, disk_key);
    flipper_format_rewind(ff);
    flipper_format_insert_or_update_hex(ff, FIAT_V1_HITAG2_KEY_FIELD, disk_key, 6U);
    fiat_v1_flipper_update_or_insert_u32(ff, FIAT_V1_HITAG2_EPOCH_FIELD, epoch & 0x3FFFFUL);
}

static bool fiat_v1_resolve_hitag2_key(
    FlipperFormat* ff,
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t hop,
    uint8_t key_out[6],
    uint32_t* epoch_out) {
    if(!key_out) return false;

    uint8_t key[6] = {0};
    uint32_t epoch = 0U;
    if(ff) {
        flipper_format_rewind(ff);
        uint8_t disk_key[6] = {0};
        if(flipper_format_read_hex(ff, FIAT_V1_HITAG2_KEY_FIELD, disk_key, sizeof(disk_key))) {
            fiat_v1_hitag2_key_disk_to_cipher(disk_key, key);
            flipper_format_rewind(ff);
            if(flipper_format_read_uint32(ff, FIAT_V1_HITAG2_EPOCH_FIELD, &epoch, 1U)) {
                epoch &= 0x3FFFFUL;
            }
            if(fiat_v1_key_matches(uid, button, control, hop, key, epoch) ||
               fiat_v1_key_matches_any_button(uid, control, hop, key, epoch)) {
                memcpy(key_out, key, sizeof(key));
                if(epoch_out) *epoch_out = epoch;
                if(!fiat_v1_key_is_builtin(key)) {
                    Hitag2KeyDbRecord existing = {0};
                    if(!hitag2_key_db_find(uid, &existing)) {
                        hitag2_key_db_save_key(uid, key, epoch);
                    }
                }
                return true;
            }
        }
    }

    if(ff && uid != 0U && uid != UINT32_MAX) {
        Hitag2KeyDbRecord record = {0};
        if(hitag2_key_db_find(uid, &record) && record.has_key) {
            const uint32_t db_epoch = record.epoch & 0x3FFFFUL;
            if(fiat_v1_key_matches(uid, button, control, hop, record.key, db_epoch) ||
               fiat_v1_key_matches_any_button(uid, control, hop, record.key, db_epoch)) {
                memcpy(key_out, record.key, sizeof(record.key));
                if(epoch_out) *epoch_out = db_epoch;
                fiat_v1_store_hitag2_key(ff, record.key, db_epoch);
                return true;
            }
        }
    }

    for(uint8_t i = 0U; i < FIAT_V1_KNOWN_KEY_COUNT; i++) {
        if(fiat_v1_key_matches(uid, button, control, hop, fiat_v1_known_keys[i], 0U) ||
           fiat_v1_key_matches_any_button(uid, control, hop, fiat_v1_known_keys[i], 0U)) {
            memcpy(key_out, fiat_v1_known_keys[i], sizeof(key));
            if(epoch_out) *epoch_out = 0U;
            return true;
        }
    }
    return false;
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
    index = fiat_v1_emit_merge(upload, index, cap, true, fiat_v1_enc_lead_us(variant));

    for(uint8_t bit_index = 0U; bit_index < FIAT_V1_WIRE_BITS; bit_index++) {
        const bool bit = ((instance->raw_data[bit_index >> 3U] >> (7U - (bit_index & 7U))) & 1U) !=
                         0U;
        index = fiat_v1_emit_merge(upload, index, cap, bit, te);
        index = fiat_v1_emit_merge(upload, index, cap, !bit, te);
    }

    index = fiat_v1_emit_merge(upload, index, cap, false, fiat_v1_enc_gap_us(variant));
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
        /* Keep hop paired with file Cnt; Btn may already be remapped by the UI. */
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

    flipper_format_rewind(flipper_format);
    uint32_t tail_bits = instance->tail_bits;
    if(flipper_format_read_uint32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, &tail_bits, 1U)) {
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
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Serial", serial_w);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Btn", btn_w);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Cnt", cnt_w);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Type", type_w);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_HOP_FIELD, hop_w);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_XOR_FIELD, xor_w);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, tail_w);
    flipper_format_insert_or_update_hex(
        flipper_format, FIAT_V1_RAW_FIELD, instance->raw_data, FIAT_V1_WIRE_BYTES);
    uint8_t hitag2_disk_key[6];
    fiat_v1_hitag2_key_cipher_to_disk(instance->hitag2_key, hitag2_disk_key);
    flipper_format_insert_or_update_hex(
        flipper_format, FIAT_V1_HITAG2_KEY_FIELD, hitag2_disk_key, 6U);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_HITAG2_EPOCH_FIELD, epoch_w);
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
    instance->decoder.parser_step = FiatV1DecoderStepReset;
    instance->decoder.decode_data = 0U;
    instance->decoder.decode_count_bit = 0U;
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
    instance->last_lead_us = 0U;
    memset(instance->hitag2_key, 0, sizeof(instance->hitag2_key));
    fiat_v1_clear_all_cells(instance);
}

void subghz_protocol_decoder_fiat_v1_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV1* instance = context;

    switch(instance->decoder.parser_step) {
    case FiatV1DecoderStepReset:
        if(fiat_v1_duration_is_pulse(duration)) {
            bool armed = false;
            for(uint8_t variant = 0U; variant < FIAT_V1_VARIANT_COUNT; variant++) {
                if(!fiat_v1_lead_armed(instance, variant)) {
                    continue;
                }
                if(fiat_v1_duration_is_short(variant, duration) ||
                   fiat_v1_duration_is_long(variant, duration)) {
                    armed = true;
                    break;
                }
            }
            if(!armed) {
                break;
            }
            fiat_v1_clear_all_cells(instance);
            instance->decoder.parser_step = FiatV1DecoderStepData;
            if(!fiat_v1_feed_data_pulse(instance, level, duration)) {
                instance->decoder.parser_step = FiatV1DecoderStepReset;
            }
        } else {
            fiat_v1_note_boundary(instance, level, duration);
        }
        break;

    case FiatV1DecoderStepData:
        if(!fiat_v1_feed_data_pulse(instance, level, duration)) {
            instance->decoder.parser_step = FiatV1DecoderStepReset;
            fiat_v1_note_boundary(instance, level, duration);
        }
        break;
    }
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
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Serial", instance->generic.serial);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Btn", instance->generic.btn);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Cnt", instance->generic.cnt);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, "Type", te_variant);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_HOP_FIELD, hop);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_XOR_FIELD, frame_xor);
    fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_TAIL_BITS_FIELD, tail_bits);

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
        fiat_v1_flipper_update_or_insert_u32(flipper_format, FIAT_V1_HITAG2_EPOCH_FIELD, epoch);
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
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Type", &value, 1)) {
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

bool fiat_v1_flipper_needs_hitag2_key(FlipperFormat* flipper_format) {
    if(!flipper_format) return false;
    FuriString* proto = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_string(flipper_format, "Protocol", proto) ||
       !furi_string_equal_str(proto, SUBGHZ_PROTOCOL_FIAT_V1_NAME)) {
        furi_string_free(proto);
        return false;
    }
    furi_string_free(proto);

    uint32_t uid = 0U;
    uint8_t button = 0U;
    uint16_t control = 0U;
    uint32_t hop = 0U;
    uint8_t key[6] = {0};
    uint32_t epoch = 0U;
    if(!fiat_v1_parse_frame_from_flipper(flipper_format, &uid, &button, &control, &hop)) {
        return true;
    }
    return !fiat_v1_resolve_hitag2_key(flipper_format, uid, button, control, hop, key, &epoch);
}

bool fiat_v1_flipper_parse_hitag2_key_text(const char* text, uint8_t key[6]) {
    if(!text || !key || strlen(text) != 12U) return false;
    for(size_t i = 0; i < 6U; i++) {
        char byte_str[3] = {text[i * 2], text[i * 2 + 1], '\0'};
        char* end = NULL;
        unsigned long val = strtoul(byte_str, &end, 16);
        if(end != byte_str + 2 || val > 0xFFUL) return false;
        key[i] = (uint8_t)val;
    }
    return true;
}

bool fiat_v1_flipper_accept_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) return false;

    FuriString* proto = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    const bool is_fiat = flipper_format_read_string(flipper_format, "Protocol", proto) &&
                         furi_string_equal_str(proto, SUBGHZ_PROTOCOL_FIAT_V1_NAME);
    furi_string_free(proto);
    if(!is_fiat) return false;

    uint32_t uid = 0U;
    uint8_t button = 0U;
    uint16_t control = 0U;
    uint32_t hop = 0U;
    if(!fiat_v1_parse_frame_from_flipper(flipper_format, &uid, &button, &control, &hop)) {
        return false;
    }
    uint8_t cipher_key[6];
    fiat_v1_hitag2_key_disk_to_cipher(key, cipher_key);
    if(!fiat_v1_key_matches(uid, button, control, hop, cipher_key, 0U) &&
       !fiat_v1_key_matches_any_button(uid, control, hop, cipher_key, 0U)) {
        return false;
    }

    fiat_v1_store_hitag2_key(flipper_format, cipher_key, 0U);
    hitag2_key_db_save_key(uid, cipher_key, 0U);
    return true;
}

bool fiat_v1_flipper_write_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) return false;
    uint8_t disk_key[6];
    fiat_v1_hitag2_key_cipher_to_disk(key, disk_key);
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(
           flipper_format, FIAT_V1_HITAG2_KEY_FIELD, disk_key, 6U)) {
        return false;
    }
    uint32_t epoch = 0U;
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_uint32(flipper_format, FIAT_V1_HITAG2_EPOCH_FIELD, &epoch, 1U)) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(
            flipper_format, FIAT_V1_HITAG2_EPOCH_FIELD, &epoch, 1U);
    }
    return true;
}
