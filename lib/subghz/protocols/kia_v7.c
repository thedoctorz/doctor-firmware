#include "kia_v7.h"
#include "aes_common.h"
#include <lib/subghz/registry.h>
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
    kia_v7_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void kia_v7_encoder_read_fields(
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

static uint32_t kia_v7_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus kia_v7_encoder_read_bit(
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

static SubGhzProtocolStatus kia_v7_serialize_fields(
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

static void kia_v7_get_string_setup_editor(
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
    kia_v7_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static inline ManchesterEvent
    kia_v7_manchester_event(uint32_t duration, bool level, const SubGhzBlockConst* t) {
    if(DURATION_DIFF(duration, t->te_short) < t->te_delta) {
        return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    }
    if(DURATION_DIFF(duration, t->te_long) < t->te_delta) {
        return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }
    return ManchesterEventReset;
}

static uint8_t kia_v7_reverse_bits8(uint8_t value) {
    reverse_bits_in_bytes(&value, 1);
    return value;
}

static void kia_v7_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t kia_v7_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static SubGhzProtocolStatus
    kia_v7_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

#include <string.h>

#define KIA_V7_PREAMBLE_PAIRS      0x13FU
#define KIA_V7_TAIL_PREAMBLE_PAIRS 0x0FU
#define KIA_V7_PREAMBLE_MIN_PAIRS  16
#define KIA_V7_HEADER              0x4C
#define KIA_V7_TAIL_GAP_US         0x7D0
#define KIA_V7_KEY_BITS            64U
#define KIA_V7_DEFAULT_TX_REPEAT   3U
#define KIA_V7_FRAME_SLOTS(pairs)  (((pairs) * 2U) + 1U + (KIA_V7_KEY_BITS * 2U) + 2U)
#define KIA_V7_UPLOAD_CAPACITY \
    (KIA_V7_FRAME_SLOTS(KIA_V7_PREAMBLE_PAIRS) + KIA_V7_FRAME_SLOTS(KIA_V7_TAIL_PREAMBLE_PAIRS))
static const SubGhzBlockConst subghz_protocol_kia_v7_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = KIA_V7_KEY_BITS,
};

typedef enum {
    KiaV7DecoderStepReset = 0,
    KiaV7DecoderStepPreamble = 1,
    KiaV7DecoderStepSyncLow = 2,
    KiaV7DecoderStepData = 3,
} KiaV7DecoderStep;

struct SubGhzProtocolDecoderKiaV7 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint16_t preamble_count;

    uint8_t decoded_button;
    uint8_t fixed_high_byte;
    uint8_t crc_calculated;
    uint8_t crc_raw;
    bool crc_valid;
};

static uint8_t kia_v7_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x01;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x02;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x03;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x08;
    default:
        return subghz_custom_btn_get_original();
    }
}

struct SubGhzProtocolEncoderKiaV7 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t tx_bit_count;
    uint8_t decoded_button;
    uint8_t fixed_high_byte;
    uint8_t crc_calculated;
    uint8_t crc_raw;
    bool crc_valid;
};

#define kia_v7_crc8(data, len) subghz_protocol_blocks_crc8((data), (len), 0x7F, 0x4C)

static const char* kia_v7_get_button_name(uint8_t button) {
    switch(button) {
    case 0x01:
        return "Lock";
    case 0x02:
        return "Unlock";
    case 0x03:
    case 0x08:
        return "Trunk";
    default:
        return "??";
    }
}

static SubGhzProtocolStatus kia_v7_write_display_for_button(
    FlipperFormat* flipper_format,
    const char* protocol_name,
    uint8_t button) {
    return kia_v7_write_display(flipper_format, protocol_name, kia_v7_get_button_name(button));
}

static void kia_v7_decode_key_common(
    SubGhzBlockGeneric* generic,
    uint8_t* decoded_button,
    uint8_t* fixed_high_byte,
    uint8_t* crc_calculated,
    uint8_t* crc_raw,
    bool* crc_valid) {
    uint8_t bytes[8];
    kia_v7_u64_to_bytes_be(generic->data, bytes);

    const uint32_t serial = (((uint32_t)bytes[3]) << 20U) | (((uint32_t)bytes[4]) << 12U) |
                            (((uint32_t)bytes[5]) << 4U) | (((uint32_t)bytes[6]) >> 4U);
    const uint16_t counter = ((uint16_t)bytes[1] << 8U) | (uint16_t)bytes[2];
    const uint8_t button = bytes[6] & 0x0FU;
    const uint8_t crc_calc = kia_v7_crc8(bytes, 7);
    const uint8_t crc_pkt = bytes[7];

    generic->serial = serial & 0x0FFFFFFFU;
    generic->btn = button;
    generic->cnt = counter;
    generic->data_count_bit = KIA_V7_KEY_BITS;

    if(decoded_button) {
        *decoded_button = button;
    }
    if(fixed_high_byte) {
        *fixed_high_byte = bytes[0];
    }
    if(crc_calculated) {
        *crc_calculated = crc_calc;
    }
    if(crc_raw) {
        *crc_raw = crc_pkt;
    }
    if(crc_valid) {
        *crc_valid = (crc_calc == crc_pkt);
    }
}

static void kia_v7_decode_key_decoder(SubGhzProtocolDecoderKiaV7* instance) {
    kia_v7_decode_key_common(
        &instance->generic,
        &instance->decoded_button,
        &instance->fixed_high_byte,
        &instance->crc_calculated,
        &instance->crc_raw,
        &instance->crc_valid);
}

static uint64_t kia_v7_encode_key(
    uint8_t fixed_high_byte,
    uint32_t serial,
    uint8_t button,
    uint16_t counter,
    uint8_t* crc_out) {
    uint8_t bytes[8];

    serial &= 0x0FFFFFFFU;
    button &= 0x0FU;

    bytes[0] = fixed_high_byte;
    bytes[1] = (counter >> 8U) & 0xFFU;
    bytes[2] = counter & 0xFFU;
    bytes[3] = (serial >> 20U) & 0xFFU;
    bytes[4] = (serial >> 12U) & 0xFFU;
    bytes[5] = (serial >> 4U) & 0xFFU;
    bytes[6] = ((serial & 0x0FU) << 4U) | button;
    bytes[7] = kia_v7_crc8(bytes, 7);

    if(crc_out) {
        *crc_out = bytes[7];
    }

    return kia_v7_bytes_to_u64_be(bytes);
}

static void kia_v7_decode_key_encoder(SubGhzProtocolEncoderKiaV7* instance) {
    kia_v7_decode_key_common(
        &instance->generic,
        &instance->decoded_button,
        &instance->fixed_high_byte,
        &instance->crc_calculated,
        &instance->crc_raw,
        &instance->crc_valid);
}

static bool kia_v7_encoder_append_frame(
    LevelDuration* upload,
    size_t* index,
    size_t max_size,
    uint64_t data,
    uint8_t bit_count,
    size_t preamble_pairs) {
    const LevelDuration high_short =
        level_duration_make(true, subghz_protocol_kia_v7_const.te_short);
    const LevelDuration low_short =
        level_duration_make(false, subghz_protocol_kia_v7_const.te_short);
    const LevelDuration low_tail = level_duration_make(false, KIA_V7_TAIL_GAP_US);

    for(size_t i = 0; i < preamble_pairs; i++) {
        if((*index + 2U) > max_size) {
            return false;
        }
        upload[(*index)++] = high_short;
        upload[(*index)++] = low_short;
    }

    if((*index + 1U) > max_size) {
        return false;
    }
    upload[(*index)++] = high_short;

    for(int32_t bit = (int32_t)bit_count - 1; bit >= 0; bit--) {
        if((*index + 2U) > max_size) {
            return false;
        }
        const bool value = ((data >> bit) & 1ULL) != 0ULL;
        upload[(*index)++] = value ? high_short : low_short;
        upload[(*index)++] = value ? low_short : high_short;
    }

    if((*index + 2U) > max_size) {
        return false;
    }
    upload[(*index)++] = high_short;
    upload[(*index)++] = low_tail;
    return true;
}

static bool kia_v7_encoder_get_upload(SubGhzProtocolEncoderKiaV7* instance) {
    furi_check(instance);

    const uint8_t bit_count = (instance->tx_bit_count > 0U && instance->tx_bit_count <= 64U) ?
                                  instance->tx_bit_count :
                                  64U;
    size_t index = 0;

    if(!kia_v7_encoder_append_frame(
           instance->encoder.upload,
           &index,
           KIA_V7_UPLOAD_CAPACITY,
           instance->generic.data,
           bit_count,
           KIA_V7_PREAMBLE_PAIRS)) {
        return false;
    }
    if(!kia_v7_encoder_append_frame(
           instance->encoder.upload,
           &index,
           KIA_V7_UPLOAD_CAPACITY,
           instance->generic.data,
           bit_count,
           KIA_V7_TAIL_PREAMBLE_PAIRS)) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void subghz_protocol_decoder_kia_v7_free(void* context) {
    furi_assert(context);
    free(context);
}

uint8_t subghz_protocol_decoder_kia_v7_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV7* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_encoder_kia_v7_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV7* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v7_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV7* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v7_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV7* instance = context;
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
const SubGhzProtocolDecoder subghz_protocol_kia_v7_decoder = {
    .alloc = subghz_protocol_decoder_kia_v7_alloc,
    .free = subghz_protocol_decoder_kia_v7_free,
    .feed = subghz_protocol_decoder_kia_v7_feed,
    .reset = subghz_protocol_decoder_kia_v7_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v7_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v7_serialize,
    .deserialize = subghz_protocol_decoder_kia_v7_deserialize,
    .get_string = subghz_protocol_decoder_kia_v7_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v7_encoder = {
    .alloc = subghz_protocol_encoder_kia_v7_alloc,
    .free = subghz_protocol_encoder_kia_v7_free,
    .deserialize = subghz_protocol_encoder_kia_v7_deserialize,
    .stop = subghz_protocol_encoder_kia_v7_stop,
    .yield = subghz_protocol_encoder_kia_v7_yield,
};

const SubGhzProtocol subghz_protocol_kia_v7 = {
    .name = SUBGHZ_PROTOCOL_KIA_V7_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_v7_decoder,
    .encoder = &subghz_protocol_kia_v7_encoder,
};

void* subghz_protocol_encoder_kia_v7_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderKiaV7* instance = calloc(1, sizeof(SubGhzProtocolEncoderKiaV7));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_kia_v7;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 1;
    instance->encoder.size_upload = KIA_V7_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v7_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);

    SubGhzProtocolEncoderKiaV7* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = KIA_V7_DEFAULT_TX_REPEAT;

    do {
        flipper_format_rewind(flipper_format);
        if(kia_v7_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, KIA_V7_KEY_BITS);
        if(load_st != SubGhzProtocolStatusOk) {
            break;
        }

        instance->tx_bit_count =
            (instance->generic.data_count_bit > 0U && instance->generic.data_count_bit <= 64U) ?
                (uint8_t)instance->generic.data_count_bit :
                64U;

        kia_v7_decode_key_encoder(instance);

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        kia_v7_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        if(!subghz_custom_btn_has_original()) {
            subghz_custom_btn_set_original((uint8_t)btn);
        }
        btn = kia_v7_get_btn_code();
        subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        if(!subghz_block_generic_global_counter_override_get(&cnt)) {
            cnt += furi_hal_subghz_get_rolling_counter_mult();
        }

        instance->generic.serial = serial & 0x0FFFFFFFU;
        instance->generic.btn = (uint8_t)btn & 0x0FU;
        instance->generic.cnt = cnt & 0xFFFFU;

        instance->generic.data = kia_v7_encode_key(
            instance->fixed_high_byte,
            instance->generic.serial,
            instance->generic.btn,
            (uint16_t)instance->generic.cnt,
            &instance->crc_calculated);
        instance->generic.data_count_bit = KIA_V7_KEY_BITS;

        instance->encoder.repeat =
            kia_v7_encoder_read_repeat(flipper_format, KIA_V7_DEFAULT_TX_REPEAT);

        if(!kia_v7_encoder_get_upload(instance)) {
            break;
        }

        if(instance->encoder.size_upload == 0) {
            break;
        }

        flipper_format_rewind(flipper_format);
        uint8_t key_data[sizeof(uint64_t)];
        kia_v7_u64_to_bytes_be(instance->generic.data, key_data);
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(key_data))) {
            break;
        }

        uint32_t serial_w = instance->generic.serial;
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->generic.btn;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->generic.cnt;
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        subghz_block_generic_global.current_cnt = instance->generic.cnt;

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void* subghz_protocol_decoder_kia_v7_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderKiaV7* instance = calloc(1, sizeof(SubGhzProtocolDecoderKiaV7));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_kia_v7;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_kia_v7_reset(void* context) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV7* instance = context;
    instance->decoder.parser_step = KiaV7DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_kia_v7_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV7* instance = context;
    ManchesterEvent event = ManchesterEventReset;
    bool data = false;

    switch(instance->decoder.parser_step) {
    case KiaV7DecoderStepReset:
        if(level && (DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_short) <
                     subghz_protocol_kia_v7_const.te_delta)) {
            instance->decoder.parser_step = KiaV7DecoderStepPreamble;
            instance->decoder.te_last = duration;
            instance->preamble_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case KiaV7DecoderStepPreamble:
        if(level) {
            if((DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_long) <
                subghz_protocol_kia_v7_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v7_const.te_short) <
                subghz_protocol_kia_v7_const.te_delta)) {
                if(instance->preamble_count > (KIA_V7_PREAMBLE_MIN_PAIRS - 1U)) {
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 0;
                    instance->preamble_count = 0;

                    subghz_protocol_blocks_add_bit(&instance->decoder, 1U);
                    subghz_protocol_blocks_add_bit(&instance->decoder, 0U);
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1U);
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1U);

                    instance->decoder.te_last = duration;
                    instance->decoder.parser_step = KiaV7DecoderStepSyncLow;
                } else {
                    instance->decoder.parser_step = KiaV7DecoderStepReset;
                }
            } else if((DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_short) <
                       subghz_protocol_kia_v7_const.te_delta)) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV7DecoderStepReset;
            }
        } else {
            if((DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_short) <
                subghz_protocol_kia_v7_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v7_const.te_short) <
                subghz_protocol_kia_v7_const.te_delta)) {
                instance->preamble_count++;
            } else {
                instance->decoder.parser_step = KiaV7DecoderStepReset;
            }
        }
        break;

    case KiaV7DecoderStepSyncLow:
        if(!level &&
           (DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_short) <
            subghz_protocol_kia_v7_const.te_delta) &&
           (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v7_const.te_long) <
            subghz_protocol_kia_v7_const.te_delta)) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = KiaV7DecoderStepData;
        }
        break;

    case KiaV7DecoderStepData: {
        if((DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_short) <
            subghz_protocol_kia_v7_const.te_delta)) {
            event = (ManchesterEvent)((uint8_t)(level & 1U) << 1U);
        } else if((DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_long) <
                   subghz_protocol_kia_v7_const.te_delta)) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            event = ManchesterEventReset;
        }

        if((DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_short) <
            subghz_protocol_kia_v7_const.te_delta) ||
           (DURATION_DIFF(duration, subghz_protocol_kia_v7_const.te_long) <
            subghz_protocol_kia_v7_const.te_delta)) {
            if(manchester_advance(
                   instance->manchester_state, event, &instance->manchester_state, &data)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, data);
            }
        }

        if(instance->decoder.decode_count_bit == KIA_V7_KEY_BITS) {
            const uint64_t candidate = ~instance->decoder.decode_data;
            const uint8_t hdr = (uint8_t)((candidate >> 56U) & 0xFFU);

            if(hdr == KIA_V7_HEADER) {
                instance->generic.data = candidate;
                instance->generic.data_count_bit = KIA_V7_KEY_BITS;
                kia_v7_decode_key_decoder(instance);

                if(instance->crc_valid) {
                    if(instance->base.callback) {
                        instance->base.callback(&instance->base, instance->base.context);
                    }
                } else {
                    instance->generic.data = 0;
                    instance->generic.data_count_bit = 0;
                }

                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = KiaV7DecoderStepReset;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
            } else {
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = KiaV7DecoderStepReset;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
            }
        }

        break;
    }
    }
}

void subghz_protocol_decoder_kia_v7_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV7* instance = context;
    kia_v7_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 16, 4);

    kia_v7_decode_key_decoder(instance);

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(output, "Key:%016llX\r\n", (unsigned long long)instance->generic.data);
    {
        const char* btn_name = kia_v7_get_button_name(instance->decoded_button);
        if(btn_name && btn_name[0] != '\0') {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X [%s]\r\n",
                7,
                (unsigned long)(instance->generic.serial & 0x0FFFFFFFU),
                instance->decoded_button & 0x0FU,
                btn_name);
        } else {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\n",
                7,
                (unsigned long)(instance->generic.serial & 0x0FFFFFFFU),
                instance->decoded_button & 0x0FU);
        }
    }
    furi_string_cat_printf(output, "Cnt:%04lX ", instance->generic.cnt & 0xFFFFU);
    furi_string_cat_printf(
        output, "CRC:%02X [%s]", instance->crc_calculated, instance->crc_valid ? "OK" : "ERR");
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v7_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV7* instance = context;
    kia_v7_decode_key_decoder(instance);

    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) {
        return status;
    }

    status = kia_v7_serialize_fields(
        flipper_format,
        true,
        true,
        true,
        false,
        instance->generic.serial & 0x0FFFFFFFU,
        instance->decoded_button & 0x0FU,
        (uint32_t)(instance->generic.cnt & 0xFFFFU),
        0);
    if(status != SubGhzProtocolStatusOk) return status;

    uint32_t repeat_u32 = KIA_V7_DEFAULT_TX_REPEAT;
    if(!flipper_format_write_uint32(flipper_format, "Repeat", &repeat_u32, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    return kia_v7_write_display_for_button(
        flipper_format, instance->generic.protocol_name, instance->decoded_button);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v7_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV7* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, KIA_V7_KEY_BITS);

    if(status != SubGhzProtocolStatusOk) {
        return status;
    }

    if(!flipper_format_rewind(flipper_format)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    kia_v7_decode_key_decoder(instance);

    uint32_t ser_u32 = 0;
    uint32_t btn_u32 = 0;
    uint32_t cnt_u32 = 0;
    bool got_serial = false;
    bool got_btn = false;
    bool got_cnt = false;

    flipper_format_rewind(flipper_format);
    got_serial = flipper_format_read_uint32(flipper_format, "Serial", &ser_u32, 1);
    flipper_format_rewind(flipper_format);
    got_btn = flipper_format_read_uint32(flipper_format, "Btn", &btn_u32, 1);
    flipper_format_rewind(flipper_format);
    got_cnt = flipper_format_read_uint32(flipper_format, "Cnt", &cnt_u32, 1);

    if(got_serial || got_btn || got_cnt) {
        if(got_serial) {
            instance->generic.serial = ser_u32 & 0x0FFFFFFFU;
        }
        if(got_btn) {
            instance->generic.btn = (uint8_t)(btn_u32 & 0x0FU);
        }
        if(got_cnt) {
            instance->generic.cnt = (uint16_t)(cnt_u32 & 0xFFFFU);
        }
        instance->generic.data = kia_v7_encode_key(
            instance->fixed_high_byte,
            instance->generic.serial,
            instance->generic.btn & 0x0FU,
            (uint16_t)(instance->generic.cnt & 0xFFFFU),
            &instance->crc_calculated);
        kia_v7_decode_key_decoder(instance);
    }

    return SubGhzProtocolStatusOk;
}
