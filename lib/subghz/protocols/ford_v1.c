#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/registry.h>
#include "ford_v1.h"
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t
    ford_v1_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void ford_v1_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t ford_v1_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void
    ford_v1_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static SubGhzProtocolStatus
    ford_v1_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void ford_v1_encoder_read_fields(
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

static uint32_t ford_v1_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    ford_v1_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static SubGhzProtocolStatus ford_v1_encoder_read_bit(
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

static void ford_v1_get_string_setup_editor(
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
#pragma GCC diagnostic pop

#define TAG "FordProtocolV1"

static const SubGhzBlockConst subghz_protocol_ford_v1_const = {
    .te_short = 65,
    .te_long = 130,
    .te_delta = 39,
    .min_count_bit_for_found = 136,
};

#define FORD_V1_DELTA_LONG        36U
#define FORD_V1_DELTA_DATASYNC    39U
#define FORD_V1_SILENCE_LONG_MULT 3U

#define FORD_V1_PREAMBLE_MIN 50
#define FORD_V1_DATA_BITS    136
#define FORD_V1_DATA_BYTES   17

typedef struct SubGhzProtocolDecoderFordV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t crc_calc;

    uint64_t data2;

    uint8_t raw_bytes[FORD_V1_DATA_BYTES + 1];
    uint8_t byte_count;

    ManchesterState manchester_state;
    uint16_t preamble_count;

    uint8_t sync_event_idx;
    uint8_t sync_event_count;
    uint8_t sync_events[8];

    uint8_t encryption_supported;
} SubGhzProtocolDecoderFordV1;

typedef enum {
    FordV1DecoderStepReset = 0,
    FordV1DecoderStepPreamble = 1,
    FordV1DecoderStepSync = 2,
    FordV1DecoderStepData = 3,
} FordV1DecoderStep;

static const char* ford_v1_get_button_name(uint8_t btn);
static void ford_v1_decode_with_flag(uint8_t* raw, size_t len, uint8_t flag_byte);
static uint8_t ford_v1_decode_flag_from_endbyte(uint8_t endbyte);
static void ford_v1_encode_inverse_block(uint8_t block[9]);
static void ford_v1_encode_air_9bytes(const uint8_t* plain9, uint8_t* air9_out);
static void ford_v1_fields_from_plain(
    const uint8_t plain9[9],
    uint32_t* serial_out,
    uint8_t* btn_out,
    uint32_t* cnt_out);
static bool ford_v1_plain_fields_valid(const uint8_t plain9[9], bool allow_sync_button);
static bool
    ford_v1_select_plain_from_raw(const uint8_t* raw, uint8_t plain9_out[9], bool* strict_out);
static bool ford_v1_process_data(SubGhzProtocolDecoderFordV1* instance);
static bool ford_v1_try_last_byte_variants(SubGhzProtocolDecoderFordV1* instance);
static bool ford_v1_extract_plain_from_raw(
    const uint8_t* raw17_in,
    uint8_t* plain9_out,
    uint8_t* raw17_canonical_out_opt);
static uint8_t ford_v1_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x8;
    default:
        return subghz_custom_btn_get_original();
    }
}

static void
    ford_v1_plain_apply_fields(uint8_t* plain9, uint32_t serial, uint8_t btn, uint32_t cnt);
static void ford_v1_encoder_rebuild_raw_from_plain(uint8_t* raw17, const uint8_t* plain9);

void subghz_protocol_encoder_ford_v1_free(void* context);
void subghz_protocol_encoder_ford_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_ford_v1_yield(void* context);

const SubGhzProtocolDecoder subghz_protocol_ford_v1_decoder = {
    .alloc = subghz_protocol_decoder_ford_v1_alloc,
    .free = subghz_protocol_decoder_ford_v1_free,
    .feed = subghz_protocol_decoder_ford_v1_feed,
    .reset = subghz_protocol_decoder_ford_v1_reset,
    .get_hash_data = subghz_protocol_decoder_ford_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_v1_serialize,
    .deserialize = subghz_protocol_decoder_ford_v1_deserialize,
    .get_string = subghz_protocol_decoder_ford_v1_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ford_v1_encoder = {
    .alloc = subghz_protocol_encoder_ford_v1_alloc,
    .free = subghz_protocol_encoder_ford_v1_free,
    .deserialize = subghz_protocol_encoder_ford_v1_deserialize,
    .stop = subghz_protocol_encoder_ford_v1_stop,
    .yield = subghz_protocol_encoder_ford_v1_yield,
};

const SubGhzProtocol subghz_protocol_ford_v1 = {
    .name = SUBGHZ_PROTOCOL_FORD_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_ford_v1_decoder,
    .encoder = &subghz_protocol_ford_v1_encoder,
};

#define ford_v1_crc16(data, len) subghz_protocol_blocks_crc16((data), (len), 0x1021, 0x0000)

static const char* ford_v1_get_button_name(uint8_t btn) {
    switch(btn) {
    case 0:
        return "Sync";
    case 1:
        return "Lock";
    case 2:
        return "Unlock";
    case 4:
        return "Trunk";
    case 8:
        return "Panic";
    default:
        return "??";
    }
}

static void ford_v1_decode_with_flag(uint8_t* raw, size_t len, uint8_t flag_byte) {
    if(len < 9) return;

    if(flag_byte) {
        uint8_t xor_byte = raw[7];
        for(int i = 1; i < 7; i++) {
            raw[i] ^= xor_byte;
        }
    } else {
        uint8_t xor_byte = raw[6];
        for(int i = 1; i < 6; i++) {
            raw[i] ^= xor_byte;
        }
        raw[7] ^= xor_byte;
    }

    uint8_t b6 = raw[6];
    uint8_t b7 = raw[7];
    raw[6] = (b6 & 0xAA) | (b7 & 0x55);
    raw[7] = (b7 & 0xAA) | (b6 & 0x55);
}

static uint8_t ford_v1_decode_flag_from_endbyte(uint8_t endbyte) {
    uint8_t parity = 0;
    while(endbyte) {
        parity ^= (endbyte & 1U);
        endbyte >>= 1U;
    }
    return parity & 1U;
}

static void ford_v1_encode_inverse_block(uint8_t block[9]) {
    uint8_t sum = 0;
    for(size_t i = 1; i <= 7; i++) {
        sum = (uint8_t)(sum + block[i]);
    }

    const uint8_t p6 = block[6];
    const uint8_t p7 = block[7];
    const uint8_t post6 = (uint8_t)((p6 & 0xAAU) | (p7 & 0x55U));
    const uint8_t post7 = (uint8_t)((p7 & 0xAAU) | (p6 & 0x55U));
    const uint8_t xorv = (uint8_t)(post6 ^ post7);

    uint8_t xor_byte;
    if((__builtin_popcount((unsigned int)sum) & 1) != 0) {
        block[6] = xorv;
        block[7] = post7;
        xor_byte = post7;
    } else {
        block[6] = post6;
        block[7] = xorv;
        xor_byte = post6;
    }

    for(size_t i = 1; i <= 5; i++) {
        block[i] ^= xor_byte;
    }
}

static void ford_v1_encode_air_9bytes(const uint8_t* plain9, uint8_t* air9_out) {
    uint8_t block[9];
    memcpy(block, plain9, 9);
    ford_v1_encode_inverse_block(block);
    memcpy(air9_out, block, 9);
}

static void ford_v1_fields_from_plain(
    const uint8_t plain9[9],
    uint32_t* serial_out,
    uint8_t* btn_out,
    uint32_t* cnt_out) {
    *serial_out = ((uint32_t)plain9[1] << 24) | ((uint32_t)plain9[2] << 16) |
                  ((uint32_t)plain9[3] << 8) | plain9[0];
    *btn_out = (plain9[5] >> 4) & 0x0F;
    *cnt_out = ((plain9[5] & 0x0F) << 16) | (plain9[6] << 8) | plain9[7];
}

static bool ford_v1_button_valid(uint8_t btn, bool allow_sync_button) {
    return (btn == 1U) || (btn == 2U) || (btn == 4U) || (btn == 8U) ||
           (allow_sync_button && (btn == 0U));
}

static bool ford_v1_plain_fields_valid(const uint8_t plain9[9], bool allow_sync_button) {
    uint32_t serial = 0;
    uint32_t cnt = 0;
    uint8_t btn = 0;
    ford_v1_fields_from_plain(plain9, &serial, &btn, &cnt);

    if(!ford_v1_button_valid(btn, allow_sync_button)) return false;
    if(serial == 0U || serial == 0xFFFFFFFFU) return false;

    return cnt <= 0xFFFFFU;
}

static bool
    ford_v1_select_plain_from_raw(const uint8_t* raw, uint8_t plain9_out[9], bool* strict_out) {
    const uint8_t* const air9 = &raw[6];
    const uint8_t preferred_flag = ford_v1_decode_flag_from_endbyte(air9[8]);
    const uint8_t flags[2] = {preferred_flag, preferred_flag ^ 1U};

    for(size_t i = 0; i < COUNT_OF(flags); i++) {
        uint8_t cand[9];
        memcpy(cand, air9, sizeof(cand));
        ford_v1_decode_with_flag(cand, sizeof(cand), flags[i]);

        if((cand[3] == raw[5]) && (cand[4] == raw[6]) && ford_v1_plain_fields_valid(cand, true)) {
            memcpy(plain9_out, cand, sizeof(cand));
            if(strict_out) *strict_out = true;
            return true;
        }
    }

    for(size_t i = 0; i < COUNT_OF(flags); i++) {
        uint8_t cand[9];
        uint8_t reair[9];
        memcpy(cand, air9, sizeof(cand));
        ford_v1_decode_with_flag(cand, sizeof(cand), flags[i]);
        ford_v1_encode_air_9bytes(cand, reair);

        if((memcmp(reair, air9, sizeof(reair)) == 0) && ford_v1_plain_fields_valid(cand, false)) {
            memcpy(plain9_out, cand, sizeof(cand));
            if(strict_out) *strict_out = false;
            return true;
        }
    }

    return false;
}

static bool ford_v1_process_data(SubGhzProtocolDecoderFordV1* instance) {
    uint8_t* raw = instance->raw_bytes;
    uint8_t orig[FORD_V1_DATA_BYTES];
    memcpy(orig, raw, FORD_V1_DATA_BYTES);

    uint16_t calc_crc = ford_v1_crc16(&raw[3], 12);
    uint16_t recv_crc = ((uint16_t)raw[15] << 8) | raw[16];
    if(recv_crc != calc_crc) {
        memcpy(raw, orig, FORD_V1_DATA_BYTES);
        for(size_t i = 0; i < FORD_V1_DATA_BYTES; i++) {
            raw[i] = ~raw[i];
        }
        calc_crc = ford_v1_crc16(&raw[3], 12);
        recv_crc = ((uint16_t)raw[15] << 8) | raw[16];
    }

    if(recv_crc != calc_crc) {
        return false;
    }
    uint8_t decoded[9];

    if(!ford_v1_select_plain_from_raw(raw, decoded, NULL)) {
        FURI_LOG_W(
            TAG,
            "Descramble unresolved (flag=%u raw[5]=%02X raw[6]=%02X raw[14]=%02X); drop frame",
            ford_v1_decode_flag_from_endbyte(raw[14]),
            raw[5],
            raw[6],
            raw[14]);
        return false;
    }

    uint16_t recalc_crc = ford_v1_crc16(&raw[3], 12);
    instance->crc_calc = recalc_crc;

    uint64_t key1 = 0;
    for(int i = 0; i < 7; i++) {
        key1 = (key1 << 8) | raw[0 + i];
    }

    uint64_t key2 = 0;
    for(int i = 0; i < 8; i++) {
        key2 = (key2 << 8) | raw[7 + i];
    }

    instance->generic.data = key1;
    instance->data2 = key2;
    instance->generic.data_count_bit = FORD_V1_DATA_BITS;

    ford_v1_fields_from_plain(
        decoded, &instance->generic.serial, &instance->generic.btn, &instance->generic.cnt);
    instance->encryption_supported = 1;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }

    return true;
}

static bool ford_v1_try_last_byte_variants(SubGhzProtocolDecoderFordV1* instance) {
    if(instance->byte_count != 16U) {
        subghz_protocol_decoder_ford_v1_reset(instance);
        return false;
    }

    if((uint8_t)(instance->decoder.decode_count_bit + 0x7AU) > 1U) {
        subghz_protocol_decoder_ford_v1_reset(instance);
        return false;
    }

    const uint8_t shift = (uint8_t)(FORD_V1_DATA_BITS - instance->decoder.decode_count_bit);
    const uint8_t variants = (uint8_t)(1U << shift);
    uint8_t saved[16];
    memcpy(saved, instance->raw_bytes, sizeof(saved));
    const uint32_t partial = instance->decoder.decode_data;

    for(uint8_t variant = 0; variant < variants; variant++) {
        memcpy(instance->raw_bytes, saved, sizeof(saved));
        instance->raw_bytes[16] = (uint8_t)(((uint8_t)partial << shift) | variant);

        if(ford_v1_process_data(instance)) {
            subghz_protocol_decoder_ford_v1_reset(instance);
            return true;
        }
    }

    subghz_protocol_decoder_ford_v1_reset(instance);
    return false;
}

static bool ford_v1_extract_plain_from_raw(
    const uint8_t* raw17_in,
    uint8_t* plain9_out,
    uint8_t* raw17_canonical_out_opt) {
    uint8_t raw[FORD_V1_DATA_BYTES];
    memcpy(raw, raw17_in, FORD_V1_DATA_BYTES);

    uint16_t calc_crc = ford_v1_crc16(&raw[3], 12);
    uint16_t recv_crc = ((uint16_t)raw[15] << 8) | raw[16];

    if(recv_crc != calc_crc) {
        memcpy(raw, raw17_in, FORD_V1_DATA_BYTES);
        for(size_t i = 0; i < FORD_V1_DATA_BYTES; i++) {
            raw[i] = (uint8_t)~raw[i];
        }
        calc_crc = ford_v1_crc16(&raw[3], 12);
        recv_crc = ((uint16_t)raw[15] << 8) | raw[16];
    }

    if(recv_crc != calc_crc) {
        return false;
    }

    if(ford_v1_select_plain_from_raw(raw, plain9_out, NULL)) {
        if(raw17_canonical_out_opt) {
            memcpy(raw17_canonical_out_opt, raw, FORD_V1_DATA_BYTES);
        }
        return true;
    }

    return false;
}

void* subghz_protocol_decoder_ford_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFordV1* instance = calloc(1, sizeof(SubGhzProtocolDecoderFordV1));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_ford_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_ford_v1_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_ford_v1_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordV1* instance = context;

    instance->decoder.parser_step = FordV1DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->byte_count = 0;
    memset(instance->raw_bytes, 0, sizeof(instance->raw_bytes));
    instance->preamble_count = 0;
    instance->sync_event_idx = 0;
    instance->sync_event_count = 0;
    memset(instance->sync_events, 0, sizeof(instance->sync_events));

    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_ford_v1_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFordV1* instance = context;

    uint32_t te_short = subghz_protocol_ford_v1_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v1_const.te_long;

    switch(instance->decoder.parser_step) {
    case FordV1DecoderStepReset:
        if(DURATION_DIFF(duration, te_long) < FORD_V1_DELTA_LONG) {
            instance->decoder.parser_step = FordV1DecoderStepPreamble;
            instance->preamble_count = 1;
            instance->decoder.te_last = duration;
        }
        break;

    case FordV1DecoderStepPreamble:
        if(DURATION_DIFF(duration, te_long) < FORD_V1_DELTA_LONG) {
            instance->preamble_count++;
            instance->decoder.te_last = duration;
        } else if(DURATION_DIFF(duration, te_short) < FORD_V1_DELTA_DATASYNC) {
            if(instance->preamble_count >= FORD_V1_PREAMBLE_MIN) {
                instance->sync_event_idx = 0;
                instance->sync_event_count = 1;
                instance->sync_events[0] =
                    (uint8_t)(level ? ManchesterEventShortHigh : ManchesterEventShortLow);
                instance->decoder.parser_step = FordV1DecoderStepSync;
            } else {
                instance->decoder.parser_step = FordV1DecoderStepReset;
            }
        } else {
            if(instance->preamble_count < FORD_V1_PREAMBLE_MIN) {
                instance->decoder.parser_step = FordV1DecoderStepReset;
            }
        }
        break;

    case FordV1DecoderStepSync: {
        uint8_t ev;
        bool is_short = false;

        if(DURATION_DIFF(duration, te_short) < FORD_V1_DELTA_DATASYNC) {
            ev = (uint8_t)(level ? ManchesterEventShortHigh : ManchesterEventShortLow);
            is_short = true;
        } else if(DURATION_DIFF(duration, te_long) < FORD_V1_DELTA_DATASYNC) {
            ev = (uint8_t)(level ? ManchesterEventLongHigh : ManchesterEventLongLow);
        } else {
            instance->decoder.parser_step = FordV1DecoderStepPreamble;
            break;
        }

        instance->sync_event_idx++;
        if(is_short) instance->sync_event_count++;

        if(instance->sync_event_idx < 8) {
            instance->sync_events[instance->sync_event_idx] = ev;
        }

        if(instance->sync_event_count > 2) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->byte_count = 0;
            memset(instance->raw_bytes, 0, sizeof(instance->raw_bytes));
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);

            if(instance->sync_events[0] == (uint8_t)ManchesterEventShortLow) {
                instance->manchester_state = ManchesterStateMid0;
            }

            instance->decoder.parser_step = FordV1DecoderStepData;
            for(uint8_t i = 0; i <= instance->sync_event_idx && i < 8; i++) {
                bool data_bit;
                if(manchester_advance(
                       instance->manchester_state,
                       (ManchesterEvent)instance->sync_events[i],
                       &instance->manchester_state,
                       &data_bit)) {
                    instance->decoder.decode_data = (instance->decoder.decode_data << 1) |
                                                    (data_bit ? 1 : 0);
                    instance->decoder.decode_count_bit++;

                    if((instance->decoder.decode_count_bit & 7) == 0) {
                        uint8_t byte_val = (uint8_t)(instance->decoder.decode_data & 0xFF);
                        if(instance->byte_count < FORD_V1_DATA_BYTES) {
                            instance->raw_bytes[instance->byte_count] = byte_val;
                            instance->byte_count++;
                        }
                        instance->decoder.decode_data = 0;
                    }
                }
            }
            break;
        }

        if(instance->sync_event_idx >= 7) {
            instance->decoder.parser_step = FordV1DecoderStepPreamble;
        }
        break;
    }

    case FordV1DecoderStepData: {
        ManchesterEvent event;

        if(DURATION_DIFF(duration, te_short) < FORD_V1_DELTA_DATASYNC) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(DURATION_DIFF(duration, te_long) < FORD_V1_DELTA_DATASYNC) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            if(duration >= te_long * FORD_V1_SILENCE_LONG_MULT) {
            } else {
            }
            (void)ford_v1_try_last_byte_variants(instance);
            break;
        }

        bool data_bit;
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            instance->decoder.decode_data = (instance->decoder.decode_data << 1) |
                                            (data_bit ? 1 : 0);
            instance->decoder.decode_count_bit++;

            if((instance->decoder.decode_count_bit & 7) == 0) {
                uint8_t byte_val = (uint8_t)(instance->decoder.decode_data & 0xFF);
                if(instance->byte_count < FORD_V1_DATA_BYTES) {
                    instance->raw_bytes[instance->byte_count] = byte_val;
                    instance->byte_count++;
                }
                instance->decoder.decode_data = 0;

                if(instance->byte_count > 16) {
                    ford_v1_process_data(instance);
                    subghz_protocol_decoder_ford_v1_reset(instance);
                }
            }
        }

        instance->decoder.te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_ford_v1_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordV1* instance = context;

    uint32_t crc = instance->crc_calc;
    uint8_t hash = (uint8_t)(crc ^ (crc >> 8));

    for(int i = 0; i < 8; i++) {
        hash ^= instance->raw_bytes[i + 5];
    }

    return hash;
}

static void ford_v1_key3_bytes_from_crc16(uint16_t crc16, uint8_t key3_out[4]) {
    key3_out[0] = 0;
    key3_out[1] = 0;
    key3_out[2] = (uint8_t)((crc16 >> 8) & 0xFFU);
    key3_out[3] = (uint8_t)(crc16 & 0xFFU);
}

static uint16_t ford_v1_crc16_from_key3_bytes(const uint8_t key3[4]) {
    uint32_t crc = ((uint32_t)key3[0] << 24) | ((uint32_t)key3[1] << 16) |
                   ((uint32_t)key3[2] << 8) | (uint32_t)key3[3];
    if((crc & 0xFFFFU) == 0U && (crc >> 16) != 0U) {
        return __builtin_bswap16((uint16_t)(crc >> 16));
    }
    return (uint16_t)(crc & 0xFFFFU);
}

static uint64_t ford_v1_u64_from_be_key8(const uint8_t key8[8]) {
    uint64_t v = 0;
    for(int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)key8[i];
    }
    return v;
}

static uint64_t ford_v1_u64_legacy_halves_bswap(const uint8_t key8[8]) {
    uint64_t stored = ford_v1_u64_from_be_key8(key8);
    return ((uint64_t)__builtin_bswap32((uint32_t)(stored >> 32)) << 32) |
           (uint64_t)__builtin_bswap32((uint32_t)(stored & 0xFFFFFFFFU));
}

static void ford_v1_raw14_from_internal_keys(
    uint64_t key1,
    uint64_t data2,
    uint8_t raw17_out[FORD_V1_DATA_BYTES]) {
    for(int i = 0; i < 7; i++) {
        raw17_out[i] = (uint8_t)((key1 >> (48 - i * 8)) & 0xFFU);
    }
    for(int i = 0; i < 8; i++) {
        raw17_out[7 + i] = (uint8_t)((data2 >> (56 - i * 8)) & 0xFFU);
    }
}

static bool ford_v1_keys_file_to_canonical_raw(
    const uint8_t key1b[8],
    const uint8_t key2b[8],
    uint16_t crc16,
    uint64_t* key1_out,
    uint64_t* key2_out,
    uint8_t raw17[FORD_V1_DATA_BYTES],
    uint8_t plain9[9]) {
    for(unsigned attempt = 0; attempt < 2; attempt++) {
        uint64_t k1 = (attempt == 0) ? ford_v1_u64_from_be_key8(key1b) :
                                       ford_v1_u64_legacy_halves_bswap(key1b);
        uint64_t k2 = (attempt == 0) ? ford_v1_u64_from_be_key8(key2b) :
                                       ford_v1_u64_legacy_halves_bswap(key2b);
        *key1_out = k1;
        *key2_out = k2;
        ford_v1_raw14_from_internal_keys(k1, k2, raw17);
        raw17[15] = (uint8_t)((crc16 >> 8) & 0xFFU);
        raw17[16] = (uint8_t)(crc16 & 0xFFU);
        if(ford_v1_extract_plain_from_raw(raw17, plain9, raw17)) {
            return true;
        }
    }
    *key1_out = ford_v1_u64_from_be_key8(key1b);
    *key2_out = ford_v1_u64_from_be_key8(key2b);
    ford_v1_raw14_from_internal_keys(*key1_out, *key2_out, raw17);
    raw17[15] = (uint8_t)((crc16 >> 8) & 0xFFU);
    raw17[16] = (uint8_t)(crc16 & 0xFFU);
    return false;
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFordV1* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t ser_w = instance->generic.serial;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &ser_w, 1);
        uint32_t btn_w = instance->generic.btn;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->generic.cnt;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);

        uint8_t key1_bytes[8];
        for(int i = 0; i < 8; i++) {
            key1_bytes[i] = (uint8_t)(instance->generic.data >> (56 - i * 8));
        }
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(flipper_format, "Key", key1_bytes, 8);

        uint8_t key2_bytes[8];
        for(int i = 0; i < 8; i++) {
            key2_bytes[i] = (uint8_t)(instance->data2 >> (56 - i * 8));
        }
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(flipper_format, "Key_2", key2_bytes, 8);

        uint8_t key3_bytes[4];
        ford_v1_key3_bytes_from_crc16((uint16_t)(instance->crc_calc & 0xFFFFU), key3_bytes);
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(flipper_format, "Key_3", key3_bytes, 4);
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ford_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFordV1* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_ford_v1_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        uint8_t key1_bytes[8] = {0};
        flipper_format_read_hex(flipper_format, "Key", key1_bytes, 8);

        flipper_format_rewind(flipper_format);
        uint8_t key2_bytes[8] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key_2", key2_bytes, 8)) {
            flipper_format_rewind(flipper_format);
            if(!flipper_format_read_hex(flipper_format, "Key2", key2_bytes, 8)) {
                return SubGhzProtocolStatusErrorParserOthers;
            }
        }

        flipper_format_rewind(flipper_format);
        uint8_t key3_bytes[4] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key_3", key3_bytes, 4)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        instance->crc_calc = ford_v1_crc16_from_key3_bytes(key3_bytes);

        uint8_t raw[FORD_V1_DATA_BYTES];
        uint8_t plain9_tmp[9];
        uint64_t k1 = 0;
        uint64_t k2 = 0;
        uint16_t crc16 = (uint16_t)(instance->crc_calc & 0xFFFFU);
        bool extract_ok = ford_v1_keys_file_to_canonical_raw(
            key1_bytes, key2_bytes, crc16, &k1, &k2, raw, plain9_tmp);
        instance->generic.data = k1;
        instance->data2 = k2;

        instance->encryption_supported = extract_ok ? 1 : 0;

        memcpy(instance->raw_bytes, raw, FORD_V1_DATA_BYTES);

        uint32_t serial = UINT32_MAX;
        uint32_t btn = UINT32_MAX;
        uint32_t cnt = UINT32_MAX;
        ford_v1_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        if((serial == UINT32_MAX || btn == UINT32_MAX || cnt == UINT32_MAX) && extract_ok) {
            uint32_t decoded_serial = 0;
            uint32_t decoded_cnt = 0;
            uint8_t decoded_btn = 0;
            ford_v1_fields_from_plain(plain9_tmp, &decoded_serial, &decoded_btn, &decoded_cnt);

            if(serial == UINT32_MAX) serial = decoded_serial;
            if(btn == UINT32_MAX) btn = decoded_btn;
            if(cnt == UINT32_MAX) cnt = decoded_cnt;
        }

        if(serial == UINT32_MAX || btn == UINT32_MAX || cnt == UINT32_MAX) {
            return SubGhzProtocolStatusErrorParserOthers;
        }

        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)(btn & 0x0FU);
        instance->generic.cnt = cnt & 0xFFFFFU;
    }

    return ret;
}

void subghz_protocol_decoder_ford_v1_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderFordV1* instance = context;
    ford_v1_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 28, 4);

    const uint64_t key1 = instance->generic.data;
    const uint64_t key2 = instance->data2;
    const uint32_t crc = instance->crc_calc;
    const uint16_t crc16 = crc & 0xFFFF;

    if(instance->encryption_supported) {
        const char* btn_name = ford_v1_get_button_name(instance->generic.btn);

        const uint16_t calc_crc = crc16;

        bool crc_ok;
        uint8_t raw[FORD_V1_DATA_BYTES];
        ford_v1_raw14_from_internal_keys(key1, key2, raw);
        raw[15] = (uint8_t)((crc >> 8) & 0xFF);
        raw[16] = (uint8_t)(crc & 0xFF);

        const uint16_t check_crc = ford_v1_crc16(&raw[3], 12);
        crc_ok = (check_crc == calc_crc);

        furi_string_cat_printf(
            output,
            "%s %ubit\r\n",
            instance->generic.protocol_name,
            instance->generic.data_count_bit);
        furi_string_cat_printf(
            output,
            "Key1:%014llX%06llX\r\nKey2:%010llX%04lX\r\n",
            (unsigned long long)key1,
            (unsigned long long)(key2 >> 40),
            (unsigned long long)(key2 & 0xFFFFFFFFFFULL),
            (unsigned long)crc16);
        ((btn_name && btn_name[0] != '\0') ? furi_string_cat_printf(
                                                 output,
                                                 "Sn:%0*lX Btn:%02X [%s]\r\n",
                                                 (int)8,
                                                 (unsigned long)instance->generic.serial,
                                                 instance->generic.btn,
                                                 btn_name) :
                                             furi_string_cat_printf(
                                                 output,
                                                 "Sn:%0*lX Btn:%02X\r\n",
                                                 (int)8,
                                                 (unsigned long)instance->generic.serial,
                                                 instance->generic.btn));
        furi_string_cat_printf(output, "Cnt:%05lX ", (unsigned long)instance->generic.cnt);
        furi_string_cat_printf(
            output, "CRC:%04lX [%s]\r\n", (unsigned long)crc16, crc_ok ? "OK" : "ERR");
    } else {
        uint8_t raw[FORD_V1_DATA_BYTES];
        ford_v1_raw14_from_internal_keys(key1, key2, raw);
        raw[15] = (uint8_t)((crc >> 8) & 0xFF);
        raw[16] = (uint8_t)(crc & 0xFF);

        uint16_t check_crc = ford_v1_crc16(&raw[3], 12);
        bool crc_ok = (check_crc == crc16);

        uint32_t device_id = ((uint32_t)raw[3] << 24) | ((uint32_t)raw[4] << 16) |
                             ((uint32_t)raw[5] << 8) | raw[6];

        furi_string_cat_printf(
            output,
            "%s %ubit\r\n",
            instance->generic.protocol_name,
            instance->generic.data_count_bit);
        furi_string_cat_printf(
            output,
            "Key1:%014llX%06llX\r\nKey2:%010llX%04lX\r\n",
            (unsigned long long)key1,
            (unsigned long long)(key2 >> 40),
            (unsigned long long)(key2 & 0xFFFFFFFFFFULL),
            (unsigned long)crc16);
        furi_string_cat_printf(
            output, "Sn:%0*lX Btn:%02X\r\n", (int)8, (unsigned long)device_id, 0);
        furi_string_cat_printf(
            output,
            "CRC:%04lX [%s]\r\nEncryption not supported !\r\n",
            (unsigned long)crc16,
            crc_ok ? "OK" : "ERR");
    }
}

#define FORD_V1_ENC_BURST_COUNT    6U
#define FORD_V1_ENC_PREAMBLE_PAIRS 400U
#define FORD_V1_ENC_BURST_LD_COUNT \
    ((FORD_V1_ENC_PREAMBLE_PAIRS * 2U) + 2U + (FORD_V1_DATA_BYTES * 16U) + 1U)
#define FORD_V1_ENC_SYNC_SHORT_US 65U
#define FORD_V1_ENC_SYNC_LONG_US  130U
#define FORD_V1_ENC_GAP_REPEAT_US 50000U
#define FORD_V1_ENC_GAP_LAST_US   260U

#if FORD_V1_ENC_BURST_LD_COUNT != 0x433U
#error Ford V1 encoder burst layout constants out of sync
#endif

static const uint8_t ford_v1_encoder_burst_pkt4_vals[6] = {0x08, 0x00, 0x10, 0x08, 0x00, 0x10};

typedef struct SubGhzProtocolEncoderFordV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint64_t data2;
    uint16_t crc_calc;
    uint8_t plain9[9];
    uint8_t raw_tx[FORD_V1_DATA_BYTES];
    uint8_t encryption_supported;
    uint8_t plain_valid;
    uint8_t burst_idx;
} SubGhzProtocolEncoderFordV1;

static void
    ford_v1_plain_apply_fields(uint8_t* plain9, uint32_t serial, uint8_t btn, uint32_t cnt) {
    uint8_t chk = (uint8_t)(plain9[8] - plain9[6] - plain9[7] - plain9[5]);
    plain9[0] = (uint8_t)(serial & 0xFFU);
    plain9[1] = (uint8_t)((serial >> 24) & 0xFFU);
    plain9[2] = (uint8_t)((serial >> 16) & 0xFFU);
    plain9[3] = (uint8_t)((serial >> 8) & 0xFFU);
    plain9[5] = (uint8_t)(((btn & 0x0FU) << 4) | ((cnt >> 16) & 0x0FU));
    plain9[6] = (uint8_t)((cnt >> 8) & 0xFFU);
    plain9[7] = (uint8_t)(cnt & 0xFFU);
    plain9[8] = (uint8_t)(chk + plain9[7] + plain9[6] + plain9[5]);
}

static void ford_v1_encoder_rebuild_raw_from_plain(uint8_t* raw17, const uint8_t* plain9) {
    uint8_t air9[9];
    ford_v1_encode_air_9bytes(plain9, air9);
    memcpy(&raw17[6], air9, 9);
    uint16_t c = ford_v1_crc16(&raw17[3], 12);
    raw17[15] = (uint8_t)(c >> 8);
    raw17[16] = (uint8_t)(c & 0xFF);
}

static void ford_v1_encoder_keys_from_raw(SubGhzProtocolEncoderFordV1* instance) {
    uint8_t* raw = instance->raw_tx;
    uint64_t key1 = 0;
    for(int i = 0; i < 7; i++) {
        key1 = (key1 << 8) | raw[i];
    }
    uint64_t key2 = 0;
    for(int i = 0; i < 8; i++) {
        key2 = (key2 << 8) | raw[7 + i];
    }
    instance->generic.data = key1;
    instance->data2 = key2;
    instance->crc_calc = (uint16_t)(((uint16_t)raw[15] << 8) | raw[16]);
}

static void ford_v1_encoder_apply_counter_cap(uint32_t* cnt_p, uint32_t cap) {
    int32_t ee = furi_hal_subghz_get_rolling_counter_mult();
    if(ee == -0x7FFFFFFF) {
        uint32_t u3 = *cnt_p;
        uint32_t r2 = u3 + 1u;
        if(r2 > cap) {
            *cnt_p = 0;
            return;
        }
        if(u3 == 0u) {
            *cnt_p = r2;
            return;
        }
        uint32_t capm1 = cap - 1u;
        if(u3 == capm1) {
            *cnt_p = capm1;
            return;
        }
        *cnt_p = r2;
        return;
    }
    if(subghz_block_generic_global_counter_override_get(cnt_p)) {
        return;
    }
    uint32_t sum = ee + *cnt_p;
    if(sum <= cap) {
        *cnt_p = sum;
        return;
    }
    *cnt_p = 0;
}

static void ford_v1_encoder_patch_key1_low_bits(SubGhzProtocolEncoderFordV1* instance) {
    uint64_t k = instance->generic.data;
    uint32_t lo = (uint32_t)(k & 0xFFFFFFFFULL);
    lo = (lo & 0xFF00FFFFU) | 0x80000U;
    instance->generic.data = (k & 0xFFFFFFFF00000000ULL) | (uint64_t)lo;
}

static void ford_v1_encoder_build_burst(SubGhzProtocolEncoderFordV1* instance, uint8_t burst_idx) {
    LevelDuration* const upload = instance->encoder.upload;
    uint8_t pkt[FORD_V1_DATA_BYTES];
    memcpy(pkt, instance->raw_tx, FORD_V1_DATA_BYTES);

    pkt[4] = ford_v1_encoder_burst_pkt4_vals[burst_idx];
    uint16_t crcw = ford_v1_crc16(&pkt[3], 12);
    pkt[15] = (uint8_t)(crcw >> 8);
    pkt[16] = (uint8_t)(crcw & 0xFFU);

    size_t index = 0;
    for(size_t i = 0; i < FORD_V1_ENC_PREAMBLE_PAIRS; i++) {
        upload[index++] = level_duration_make(true, FORD_V1_ENC_SYNC_LONG_US);
        upload[index++] = level_duration_make(false, FORD_V1_ENC_SYNC_LONG_US);
    }
    upload[index++] = level_duration_make(true, FORD_V1_ENC_SYNC_LONG_US);
    upload[index++] = level_duration_make(false, FORD_V1_ENC_SYNC_SHORT_US);

    for(size_t by = 0; by < FORD_V1_DATA_BYTES; by++) {
        uint8_t b = pkt[by];
        for(int bit_i = 7; bit_i >= 0; bit_i--) {
            bool bit = ((b >> bit_i) & 1U) != 0U;
            upload[index++] = level_duration_make(bit, FORD_V1_ENC_SYNC_SHORT_US);
            upload[index++] = level_duration_make(!bit, FORD_V1_ENC_SYNC_SHORT_US);
        }
    }

    upload[index++] = level_duration_make(
        false,
        (burst_idx + 1U == FORD_V1_ENC_BURST_COUNT) ? FORD_V1_ENC_GAP_LAST_US :
                                                      FORD_V1_ENC_GAP_REPEAT_US);
    instance->encoder.size_upload = FORD_V1_ENC_BURST_LD_COUNT;
    instance->encoder.front = 0;
}

void* subghz_protocol_encoder_ford_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFordV1* instance = calloc(1, sizeof(SubGhzProtocolEncoderFordV1));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_ford_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    instance->burst_idx = 0;
    if(!instance->encoder.upload || instance->encoder.size_upload < FORD_V1_ENC_BURST_LD_COUNT) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(FORD_V1_ENC_BURST_LD_COUNT * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = FORD_V1_ENC_BURST_LD_COUNT;
    return instance;
}

void subghz_protocol_encoder_ford_v1_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_ford_v1_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_ford_v1_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordV1* instance = context;
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
SubGhzProtocolStatus
    subghz_protocol_encoder_ford_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFordV1* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = 3;
    instance->plain_valid = 0;
    instance->burst_idx = 0;

    do {
        flipper_format_rewind(flipper_format);
        if(ford_v1_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        uint8_t key1_bytes[8] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key", key1_bytes, 8)) {
            break;
        }
        flipper_format_rewind(flipper_format);
        uint8_t key2_bytes[8] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key_2", key2_bytes, 8)) {
            flipper_format_rewind(flipper_format);
            if(!flipper_format_read_hex(flipper_format, "Key2", key2_bytes, 8)) {
                break;
            }
        }
        flipper_format_rewind(flipper_format);
        uint8_t key3_bytes[4] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key_3", key3_bytes, 4)) {
            break;
        }

        uint16_t crc16 = ford_v1_crc16_from_key3_bytes(key3_bytes);
        instance->crc_calc = crc16;

        bool keys_ok = ford_v1_keys_file_to_canonical_raw(
            key1_bytes,
            key2_bytes,
            crc16,
            &instance->generic.data,
            &instance->data2,
            instance->raw_tx,
            instance->plain9);

        instance->plain_valid = keys_ok ? 1U : 0U;
        instance->encryption_supported = instance->plain_valid;
        if(keys_ok) {
            ford_v1_encoder_keys_from_raw(instance);
            instance->crc_calc =
                (uint16_t)(((uint16_t)instance->raw_tx[15] << 8) | instance->raw_tx[16]);
        }

        if(!instance->encryption_supported || !instance->plain_valid) {
            break;
        }

        flipper_format_rewind(flipper_format);
        SubGhzProtocolStatus g = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_ford_v1_const.min_count_bit_for_found);
        if(g != SubGhzProtocolStatusOk) {
            ret = g;
            break;
        }

        ford_v1_encoder_keys_from_raw(instance);

        {
            uint32_t serial = instance->generic.serial;
            uint32_t btn = instance->generic.btn;
            uint32_t cnt = instance->generic.cnt;
            ford_v1_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
            instance->generic.serial = serial;
            instance->generic.btn = (uint8_t)(btn & 0x0FU);
            instance->generic.cnt = cnt & 0xFFFFFU;
        }

        {
            uint8_t btn_rf = (uint8_t)(instance->generic.btn & 0x0FU);
            if(!subghz_custom_btn_has_original()) {
                subghz_custom_btn_set_original(btn_rf);
            }
            btn_rf = ford_v1_get_btn_code();
            subghz_block_generic_global_button_override_get(&btn_rf);
            instance->generic.btn = btn_rf & 0x0FU;

            ford_v1_encoder_apply_counter_cap(&instance->generic.cnt, 0xFFFFFU);

            uint8_t work[9];
            memcpy(work, instance->plain9, 9);
            ford_v1_plain_apply_fields(
                work,
                instance->generic.serial,
                instance->generic.btn,
                instance->generic.cnt & 0xFFFFFU);
            memcpy(instance->plain9, work, 9);
            ford_v1_encoder_rebuild_raw_from_plain(instance->raw_tx, work);
            ford_v1_encoder_keys_from_raw(instance);
        }

        ford_v1_encoder_patch_key1_low_bits(instance);

        instance->encoder.repeat = ford_v1_encoder_read_repeat(flipper_format, 3);

        ford_v1_encoder_build_burst(instance, instance->burst_idx);

        {
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_uint32(
                flipper_format, "Serial", &instance->generic.serial, 1);
            uint32_t btn_store = instance->generic.btn;
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_store, 1);
            uint32_t cnt_store = instance->generic.cnt;
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_store, 1);

            uint8_t key1_bytes[8];
            for(int i = 0; i < 8; i++) {
                key1_bytes[i] = (uint8_t)(instance->generic.data >> (56 - i * 8));
            }
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(flipper_format, "Key", key1_bytes, 8);

            uint8_t key2_bytes[8];
            for(int i = 0; i < 8; i++) {
                key2_bytes[i] = (uint8_t)(instance->data2 >> (56 - i * 8));
            }
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(flipper_format, "Key_2", key2_bytes, 8);

            uint8_t key3_bytes[4];
            ford_v1_key3_bytes_from_crc16((uint16_t)(instance->crc_calc & 0xFFFFU), key3_bytes);
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(flipper_format, "Key_3", key3_bytes, 4);
            subghz_block_generic_global.current_cnt = instance->generic.cnt;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}
