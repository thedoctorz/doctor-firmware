#include "kia_v6.h"
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
    kia_v6_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void kia_v6_encoder_read_fields(
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

static uint32_t kia_v6_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus kia_v6_encoder_read_bit(
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

static SubGhzProtocolStatus kia_v6_serialize_fields(
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

static void kia_v6_get_string_setup_editor(
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
    kia_v6_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static inline ManchesterEvent
    kia_v6_manchester_event(uint32_t duration, bool level, const SubGhzBlockConst* t) {
    if(DURATION_DIFF(duration, t->te_short) < t->te_delta) {
        return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    }
    if(DURATION_DIFF(duration, t->te_long) < t->te_delta) {
        return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }
    return ManchesterEventReset;
}

static uint8_t kia_v6_reverse_bits8(uint8_t value) {
    reverse_bits_in_bytes(&value, 1);
    return value;
}

static void kia_v6_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t kia_v6_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static SubGhzProtocolStatus
    kia_v6_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static const char* kia_v6_get_button_name(uint8_t btn) {
    switch(btn & 0x0FU) {
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

#include "keys.h"
#include <furi.h>
#include <string.h>

#define TAG "KiaV6"

#define KIA_V6_XOR_MASK_LOW  0x84AF25FB
#define KIA_V6_XOR_MASK_HIGH 0x638766AB

static const SubGhzBlockConst subghz_protocol_kia_v6_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 100,
    .min_count_bit_for_found = 144,
};

struct SubGhzProtocolDecoderKiaV6 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    ManchesterState manchester_state;

    uint32_t data_part1_low;
    uint32_t data_part1_high;

    uint32_t stored_part1_low;
    uint32_t stored_part1_high;
    uint32_t stored_part2_low;
    uint32_t stored_part2_high;
    uint16_t data_part3;

    uint8_t bit_count;
    uint8_t fx_field;
    uint8_t crc1_field;
    uint8_t crc2_field;
    bool keys_loaded;
};

struct SubGhzProtocolEncoderKiaV6 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint32_t stored_part1_low;
    uint32_t stored_part1_high;
    uint32_t stored_part2_low;
    uint32_t stored_part2_high;
    uint16_t data_part3;
    uint8_t fx_field;
};

typedef enum {
    KiaV6DecoderStepReset = 0,
    KiaV6DecoderStepWaitFirstHigh,
    KiaV6DecoderStepCountPreamble,
    KiaV6DecoderStepWaitLongHigh,
    KiaV6DecoderStepData,
} KiaV6DecoderStep;

void subghz_protocol_decoder_kia_v6_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_encoder_kia_v6_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v6_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v6_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV6* instance = context;
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
const SubGhzProtocolDecoder subghz_protocol_kia_v6_decoder = {
    .alloc = subghz_protocol_decoder_kia_v6_alloc,
    .free = subghz_protocol_decoder_kia_v6_free,
    .feed = subghz_protocol_decoder_kia_v6_feed,
    .reset = subghz_protocol_decoder_kia_v6_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v6_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v6_serialize,
    .deserialize = subghz_protocol_decoder_kia_v6_deserialize,
    .get_string = subghz_protocol_decoder_kia_v6_get_string,
};

static uint8_t kia_default_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x3;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x8;
    default:
        return subghz_custom_btn_get_original();
    }
}

const SubGhzProtocolEncoder subghz_protocol_kia_v6_encoder = {
    .alloc = subghz_protocol_encoder_kia_v6_alloc,
    .free = subghz_protocol_encoder_kia_v6_free,
    .deserialize = subghz_protocol_encoder_kia_v6_deserialize,
    .stop = subghz_protocol_encoder_kia_v6_stop,
    .yield = subghz_protocol_encoder_kia_v6_yield,
};

const SubGhzProtocol subghz_protocol_kia_v6 = {
    .name = SUBGHZ_PROTOCOL_KIA_V6_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_v6_decoder,
    .encoder = &subghz_protocol_kia_v6_encoder,
};

#define kia_v6_crc8(data, len) subghz_protocol_blocks_crc8((data), (len), 0x07, 0xFF)

static void get_kia_v6_aes_key(uint8_t* aes_key) {
    uint64_t keystore_a = get_kia_v6_keystore_a();
    uint32_t keystore_a_hi = (keystore_a >> 32) & 0xFFFFFFFF;
    uint32_t keystore_a_lo = keystore_a & 0xFFFFFFFF;

    uint32_t uVar15_a = keystore_a_lo ^ KIA_V6_XOR_MASK_LOW; // low part
    uint32_t uVar5_a = KIA_V6_XOR_MASK_HIGH ^ keystore_a_hi; // high part

    uint64_t val64_a = ((uint64_t)uVar5_a << 32) | uVar15_a;
    for(int i = 0; i < 8; i++) {
        aes_key[i] = (val64_a >> (56 - i * 8)) & 0xFF;
    }

    uint64_t keystore_b = get_kia_v6_keystore_b();
    uint32_t keystore_b_hi = (keystore_b >> 32) & 0xFFFFFFFF;
    uint32_t keystore_b_lo = keystore_b & 0xFFFFFFFF;

    uint32_t uVar15_b = keystore_b_lo ^ KIA_V6_XOR_MASK_LOW;
    uint32_t uVar5_b = KIA_V6_XOR_MASK_HIGH ^ keystore_b_hi;

    uint64_t val64_b = ((uint64_t)uVar5_b << 32) | uVar15_b;
    for(int i = 0; i < 8; i++) {
        aes_key[i + 8] = (val64_b >> (56 - i * 8)) & 0xFF;
    }
}

static void kia_v6_encrypt_payload(
    uint8_t fx_field,
    uint32_t serial,
    uint8_t btn,
    uint32_t cnt,
    uint32_t* out_part1_low,
    uint32_t* out_part1_high,
    uint32_t* out_part2_low,
    uint32_t* out_part2_high,
    uint16_t* out_part3) {
    uint8_t plain[16];
    memset(plain, 0, 16);
    plain[0] = fx_field;
    plain[4] = (serial >> 16) & 0xFF;
    plain[5] = (serial >> 8) & 0xFF;
    plain[6] = serial & 0xFF;
    plain[7] = btn & 0x0F;
    plain[8] = (cnt >> 24) & 0xFF;
    plain[9] = (cnt >> 16) & 0xFF;
    plain[10] = (cnt >> 8) & 0xFF;
    plain[11] = cnt & 0xFF;
    plain[12] = aes_sbox_byte(cnt & 0xFF);
    plain[15] = kia_v6_crc8(plain, 15);

    uint8_t aes_key[16];
    get_kia_v6_aes_key(aes_key);
    uint8_t expanded_key[176];
    aes_key_expansion(aes_key, expanded_key);
    aes128_encrypt(expanded_key, plain);

    uint8_t fx_hi = 0x20 | (fx_field >> 4);
    uint8_t fx_lo = fx_field & 0x0F;
    *out_part1_high = ((uint32_t)fx_hi << 24) | ((uint32_t)fx_lo << 16) |
                      ((uint32_t)plain[0] << 8) | plain[1];
    *out_part1_low = ((uint32_t)plain[2] << 24) | ((uint32_t)plain[3] << 16) |
                     ((uint32_t)plain[4] << 8) | plain[5];
    *out_part2_high = ((uint32_t)plain[6] << 24) | ((uint32_t)plain[7] << 16) |
                      ((uint32_t)plain[8] << 8) | plain[9];
    *out_part2_low = ((uint32_t)plain[10] << 24) | ((uint32_t)plain[11] << 16) |
                     ((uint32_t)plain[12] << 8) | plain[13];
    *out_part3 = ((uint16_t)plain[14] << 8) | plain[15];
}
static bool kia_v6_decrypt(SubGhzProtocolDecoderKiaV6* instance) {
    uint8_t encrypted_data[16];

    encrypted_data[0] = (instance->stored_part1_high >> 8) & 0xFF;
    encrypted_data[1] = instance->stored_part1_high & 0xFF;

    encrypted_data[2] = (instance->stored_part1_low >> 24) & 0xFF;
    encrypted_data[3] = (instance->stored_part1_low >> 16) & 0xFF;
    encrypted_data[4] = (instance->stored_part1_low >> 8) & 0xFF;
    encrypted_data[5] = instance->stored_part1_low & 0xFF;

    encrypted_data[6] = (instance->stored_part2_high >> 24) & 0xFF;
    encrypted_data[7] = (instance->stored_part2_high >> 16) & 0xFF;
    encrypted_data[8] = (instance->stored_part2_high >> 8) & 0xFF;
    encrypted_data[9] = instance->stored_part2_high & 0xFF;

    encrypted_data[10] = (instance->stored_part2_low >> 24) & 0xFF;
    encrypted_data[11] = (instance->stored_part2_low >> 16) & 0xFF;
    encrypted_data[12] = (instance->stored_part2_low >> 8) & 0xFF;
    encrypted_data[13] = instance->stored_part2_low & 0xFF;

    encrypted_data[14] = (instance->data_part3 >> 8) & 0xFF;
    encrypted_data[15] = instance->data_part3 & 0xFF;

    uint8_t fx_byte0 = (instance->stored_part1_high >> 24) & 0xFF;
    uint8_t fx_byte1 = (instance->stored_part1_high >> 16) & 0xFF;
    instance->fx_field = ((fx_byte0 & 0xF) << 4) | (fx_byte1 & 0xF);

    uint8_t aes_key[16];
    get_kia_v6_aes_key(aes_key);
    uint8_t expanded_key[176];
    aes_key_expansion(aes_key, expanded_key);
    aes128_decrypt(expanded_key, encrypted_data);

    uint8_t* decrypted = encrypted_data;

    uint8_t calculated_crc = kia_v6_crc8(decrypted, 15);
    uint8_t stored_crc = decrypted[15];

    // Serial: bytes 4-6 as 24-bit big-endian
    instance->generic.serial = ((uint32_t)decrypted[4] << 16) | ((uint32_t)decrypted[5] << 8) |
                               decrypted[6];

    instance->generic.btn = decrypted[7];

    instance->generic.cnt = ((uint32_t)decrypted[8] << 24) | ((uint32_t)decrypted[9] << 16) |
                            ((uint32_t)decrypted[10] << 8) | decrypted[11];

    instance->crc1_field = decrypted[12];

    instance->crc2_field = decrypted[15];

    return (calculated_crc ^ stored_crc) < 2;
}

void* subghz_protocol_decoder_kia_v6_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderKiaV6* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV6));
    furi_check(instance);
    memset(instance, 0, sizeof(SubGhzProtocolDecoderKiaV6));

    if(environment) {
        kia_keys_load(environment);
    }

    instance->base.protocol = &subghz_protocol_kia_v6;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v6_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV6* instance = context;
    instance->decoder.parser_step = KiaV6DecoderStepReset;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

void subghz_protocol_decoder_kia_v6_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    uint32_t uVar4, uVar5;
    ManchesterEvent event;
    bool data_bit;
    uint8_t bit_count_inc;
    uint32_t step_value;

    switch(instance->decoder.parser_step) {
    case KiaV6DecoderStepReset: // case 0
        if(level == 0) {
            return;
        }
        if(DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short) <
           subghz_protocol_kia_v6_const.te_delta) {
            instance->decoder.parser_step = KiaV6DecoderStepWaitFirstHigh;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        return;

    case KiaV6DecoderStepWaitFirstHigh: // case 1
        if(level != 0) {
            return;
        }
        uint32_t diff_short = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short);
        uint32_t diff_long = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_long);

        uint32_t diff = (diff_long < diff_short) ? diff_long : diff_short;

        if(diff_long < subghz_protocol_kia_v6_const.te_delta && diff_long < diff_short) {
            if(instance->header_count >= 0x259) { // 601 decimal
                instance->header_count = 0;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = KiaV6DecoderStepWaitLongHigh;
                return;
            }
        }

        if(diff >= subghz_protocol_kia_v6_const.te_delta) {
            step_value = KiaV6DecoderStepReset;
            goto LAB_reset;
        }

        if(DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v6_const.te_short) <
           subghz_protocol_kia_v6_const.te_delta) {
            instance->decoder.te_last = duration;
            instance->header_count++;
            return;
        } else {
            step_value = KiaV6DecoderStepReset;
            goto LAB_reset;
        }

    case KiaV6DecoderStepWaitLongHigh: // case 2
        if(level == 0) {
            step_value = KiaV6DecoderStepReset;
            goto LAB_reset;
        }
        uint32_t diff_long_check = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_long);
        uint32_t diff_short_check = DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short);

        if(diff_long_check >= subghz_protocol_kia_v6_const.te_delta) {
            if(diff_short_check >= subghz_protocol_kia_v6_const.te_delta) {
                step_value = KiaV6DecoderStepReset;
                goto LAB_reset;
            }
        }

        if(DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v6_const.te_long) >=
           subghz_protocol_kia_v6_const.te_delta) {
            step_value = KiaV6DecoderStepReset;
            goto LAB_reset;
        }
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;

        subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        subghz_protocol_blocks_add_bit(&instance->decoder, 1);
        subghz_protocol_blocks_add_bit(&instance->decoder, 0);
        subghz_protocol_blocks_add_bit(&instance->decoder, 1);

        instance->data_part1_low = (uint32_t)(instance->decoder.decode_data & 0xFFFFFFFF);
        instance->data_part1_high = (uint32_t)((instance->decoder.decode_data >> 32) & 0xFFFFFFFF);
        instance->bit_count = instance->decoder.decode_count_bit;

        instance->decoder.parser_step = KiaV6DecoderStepData;
        return;

    case KiaV6DecoderStepData: // case 3
        if(DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_short) <
           subghz_protocol_kia_v6_const.te_delta) {
            event = (level & 0x7F) << 1;
            goto manchester_process;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_kia_v6_const.te_long) <
            subghz_protocol_kia_v6_const.te_delta) {
            event = level ? 6 : 4;
            goto manchester_process;
        }
        step_value = KiaV6DecoderStepReset;
        goto LAB_reset;

    manchester_process:
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            uVar4 = instance->data_part1_low;
            uVar5 = (uVar4 << 1) | (data_bit ? 1 : 0);

            uint32_t carry = (uVar4 >> 31) & 1;
            uVar4 = (instance->data_part1_high << 1) | carry;

            instance->data_part1_low = uVar5;
            instance->data_part1_high = uVar4;

            instance->decoder.decode_data = ((uint64_t)uVar4 << 32) | uVar5;

            bit_count_inc = instance->bit_count + 1;
            instance->bit_count = bit_count_inc;

            if(bit_count_inc == 0x40) {
                instance->stored_part1_low = ~uVar5;
                instance->stored_part1_high = ~uVar4;
                instance->data_part1_low = 0;
                instance->data_part1_high = 0;
            } else if(bit_count_inc == 0x80) {
                instance->stored_part2_low = ~uVar5;
                instance->stored_part2_high = ~uVar4;
                instance->data_part1_low = 0;
                instance->data_part1_high = 0;
            }
        }

        instance->decoder.te_last = duration;

        if(instance->bit_count != subghz_protocol_kia_v6_const.min_count_bit_for_found) {
            return;
        }
        instance->generic.data_count_bit = subghz_protocol_kia_v6_const.min_count_bit_for_found;
        instance->data_part3 = ~((uint16_t)instance->data_part1_low);

        kia_v6_decrypt(instance);

        if(instance->base.callback) {
            instance->base.callback(&instance->base, instance->base.context);
        }

        instance->data_part1_low = 0;
        instance->data_part1_high = 0;
        instance->bit_count = 0;
        step_value = KiaV6DecoderStepReset;
        goto LAB_reset;

    default:
        return;
    }

LAB_reset:
    instance->decoder.parser_step = step_value;
    return;
}

uint8_t subghz_protocol_decoder_kia_v6_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    uint8_t hash = 0;
    uint8_t* data = (uint8_t*)&instance->data_part1_low;
    size_t data_size = (instance->bit_count + 7) / 8;
    size_t max_size = (subghz_protocol_kia_v6_const.min_count_bit_for_found + 7) / 8;
    if(data_size > max_size) {
        data_size = max_size;
    }

    for(size_t i = 0; i < data_size; i++) {
        hash ^= data[i];
    }

    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v6_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        if(!subghz_block_generic_serialize_preset(flipper_format, preset)) break;

        if(!flipper_format_write_string_cstr(
               flipper_format, "Protocol", instance->generic.protocol_name))
            break;

        uint32_t bits = subghz_protocol_kia_v6_const.min_count_bit_for_found;
        if(!flipper_format_write_uint32(flipper_format, "Bit", &bits, 1)) break;

        uint64_t key_data = ((uint64_t)instance->stored_part1_high << 32) |
                            instance->stored_part1_low;
        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%016llX", key_data);
        if(!flipper_format_write_string_cstr(flipper_format, "Key", key_str)) break;

        if(kia_v6_serialize_fields(
               flipper_format,
               true,
               true,
               true,
               false,
               instance->generic.serial,
               instance->generic.btn,
               instance->generic.cnt,
               0) != SubGhzProtocolStatusOk)
            break;

        uint32_t key2_low = instance->stored_part2_low;
        if(!flipper_format_write_uint32(flipper_format, "Key_2", &key2_low, 1)) break;

        uint32_t key2_high = instance->stored_part2_high;
        if(!flipper_format_write_uint32(flipper_format, "Key_3", &key2_high, 1)) break;

        uint32_t key3 = instance->data_part3;
        if(!flipper_format_write_uint32(flipper_format, "Key_4", &key3, 1)) break;

        uint32_t fx = instance->fx_field;
        if(!flipper_format_write_uint32(flipper_format, "Fx", &fx, 1)) break;

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV6* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize_check_count_bit(
               &instance->generic,
               flipper_format,
               subghz_protocol_kia_v6_const.min_count_bit_for_found) != SubGhzProtocolStatusOk) {
            break;
        }

        uint64_t key = instance->generic.data;

        instance->stored_part1_low = (uint32_t)(key & 0xFFFFFFFF);
        instance->stored_part1_high = (uint32_t)((key >> 32) & 0xFFFFFFFF);

        uint32_t temp;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Key_2", &temp, 1)) {
            instance->stored_part2_low = temp;
        }
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Key_3", &temp, 1)) {
            instance->stored_part2_high = temp;
        }
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Key_4", &temp, 1)) {
            instance->data_part3 = (uint16_t)temp;
        }

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        flipper_format_rewind(flipper_format);
        const bool got_serial = flipper_format_read_uint32(flipper_format, "Serial", &serial, 1);
        flipper_format_rewind(flipper_format);
        const bool got_btn = flipper_format_read_uint32(flipper_format, "Btn", &btn, 1);
        flipper_format_rewind(flipper_format);
        const bool got_cnt = flipper_format_read_uint32(flipper_format, "Cnt", &cnt, 1);

        if(!got_serial || !got_btn || !got_cnt) {
            SubGhzProtocolDecoderKiaV6 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.stored_part1_low = instance->stored_part1_low;
            tmp.stored_part1_high = instance->stored_part1_high;
            tmp.stored_part2_low = instance->stored_part2_low;
            tmp.stored_part2_high = instance->stored_part2_high;
            tmp.data_part3 = instance->data_part3;
            if(!kia_v6_decrypt(&tmp)) {
                break;
            }
            if(!got_serial) serial = tmp.generic.serial;
            if(!got_btn) btn = tmp.generic.btn;
            if(!got_cnt) cnt = tmp.generic.cnt;
            instance->fx_field = tmp.fx_field;
        }

        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)(btn & 0x0FU);
        instance->generic.cnt = cnt;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Fx", &temp, 1)) {
            instance->fx_field = (uint8_t)temp;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_decoder_kia_v6_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV6* instance = context;
    kia_v6_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 24, 5);

    kia_v6_decrypt(instance);

    const uint32_t key1_hi = instance->stored_part1_high;
    const uint32_t key1_lo = instance->stored_part1_low;
    const uint32_t key2_hi = instance->stored_part2_high;
    const uint32_t key2_lo = instance->stored_part2_low;

    const uint32_t key2_uVar4 = key2_hi << 16;
    const uint32_t key2_uVar2 = key2_lo >> 16;
    const uint32_t key2_uVar1 = key2_hi >> 16;
    const uint32_t key2_combined = key2_uVar4 | key2_uVar2;

    const uint32_t key2_uVar3 = key2_lo << 16;
    const uint32_t key2_second = (instance->data_part3 & 0xFFFF) | key2_uVar3;

    const uint32_t serial_6 = instance->generic.serial & 0xFFFFFF;

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key1:%08lX%08lX%04lX\r\nKey2:%08lX%08lX Fx:%02X\r\n",
        (unsigned long)key1_hi,
        (unsigned long)key1_lo,
        (unsigned long)key2_uVar1,
        (unsigned long)key2_combined,
        (unsigned long)key2_second,
        instance->fx_field);
    {
        const char* btn_name = kia_v6_get_button_name(instance->generic.btn);
        if(btn_name && btn_name[0] != '\0') {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X [%s]\r\n",
                (int)6,
                (unsigned long)serial_6,
                instance->generic.btn & 0x0FU,
                btn_name);
        } else {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\n",
                (int)6,
                (unsigned long)serial_6,
                instance->generic.btn & 0x0FU);
        }
    }
    furi_string_cat_printf(
        output,
        "Cnt:%08lX CRC1:%02X CRC2:%02X",
        (unsigned long)instance->generic.cnt,
        instance->crc1_field,
        instance->crc2_field);
}

#define KIA_V6_PREAMBLE_PAIRS_1 640
#define KIA_V6_PREAMBLE_PAIRS_2 38
#define KIA_V6_TE_SHORT         200
#define KIA_V6_TE_LONG          400
#define KIA_V6_UPLOAD_SIZE      1928
static inline void
    kia_v6_encode_manchester_bit(LevelDuration* upload, size_t* index, bool bit, uint32_t te) {
    if(bit) {
        upload[(*index)++] = level_duration_make(false, te);
        upload[(*index)++] = level_duration_make(true, te);
    } else {
        upload[(*index)++] = level_duration_make(true, te);
        upload[(*index)++] = level_duration_make(false, te);
    }
}

static void kia_v6_encode_message(
    LevelDuration* upload,
    size_t* index,
    int preamble_pairs,
    uint32_t p1_lo,
    uint32_t p1_hi,
    uint32_t p2_lo,
    uint32_t p2_hi,
    uint16_t p3) {
    const uint32_t te_short = KIA_V6_TE_SHORT;
    const uint32_t te_long = KIA_V6_TE_LONG;

    for(int i = 0; i < preamble_pairs; i++) {
        upload[(*index)++] = level_duration_make(true, te_short);
        upload[(*index)++] = level_duration_make(false, te_short);
    }

    upload[(*index)++] = level_duration_make(false, te_short);
    upload[(*index)++] = level_duration_make(true, te_long);
    upload[(*index)++] = level_duration_make(false, te_short);

    for(int b = 60; b >= 0; b--) {
        uint32_t word = (b >= 32) ? p1_hi : p1_lo;
        int shift = (b >= 32) ? (b - 32) : b;
        kia_v6_encode_manchester_bit(upload, index, ((~word) >> shift) & 1, te_short);
    }

    for(int b = 63; b >= 0; b--) {
        uint32_t word = (b >= 32) ? p2_hi : p2_lo;
        int shift = (b >= 32) ? (b - 32) : b;
        kia_v6_encode_manchester_bit(upload, index, ((~word) >> shift) & 1, te_short);
    }

    for(int b = 15; b >= 0; b--) {
        kia_v6_encode_manchester_bit(upload, index, ((~p3) >> b) & 1, te_short);
    }
}

static void subghz_protocol_encoder_kia_v6_build_upload(SubGhzProtocolEncoderKiaV6* instance) {
    furi_check(instance);

    kia_v6_encrypt_payload(
        instance->fx_field,
        instance->generic.serial,
        instance->generic.btn & 0x0F,
        instance->generic.cnt,
        &instance->stored_part1_low,
        &instance->stored_part1_high,
        &instance->stored_part2_low,
        &instance->stored_part2_high,
        &instance->data_part3);

    uint32_t p1_lo = instance->stored_part1_low;
    uint32_t p1_hi = instance->stored_part1_high;
    uint32_t p2_lo = instance->stored_part2_low;
    uint32_t p2_hi = instance->stored_part2_high;
    uint16_t p3 = instance->data_part3;

    size_t index = 0;

    kia_v6_encode_message(
        instance->encoder.upload, &index, KIA_V6_PREAMBLE_PAIRS_1, p1_lo, p1_hi, p2_lo, p2_hi, p3);

    instance->encoder.upload[index++] = level_duration_make(false, KIA_V6_TE_LONG);

    kia_v6_encode_message(
        instance->encoder.upload, &index, KIA_V6_PREAMBLE_PAIRS_2, p1_lo, p1_hi, p2_lo, p2_hi, p3);

    instance->encoder.upload[index++] = level_duration_make(false, KIA_V6_TE_LONG);

    instance->encoder.size_upload = index;

    instance->encoder.front = 0;
}

void* subghz_protocol_encoder_kia_v6_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolEncoderKiaV6* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV6));
    if(!instance) return NULL;
    memset(instance, 0, sizeof(SubGhzProtocolEncoderKiaV6));

    if(environment) {
        kia_keys_load(environment);
    }

    instance->base.protocol = &subghz_protocol_kia_v6;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.size_upload = KIA_V6_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = 3;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV6* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->generic.data_count_bit = subghz_protocol_kia_v6_const.min_count_bit_for_found;

    if(kia_v6_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize_check_count_bit(
               &instance->generic,
               flipper_format,
               subghz_protocol_kia_v6_const.min_count_bit_for_found) != SubGhzProtocolStatusOk) {
            break;
        }

        const uint64_t key = instance->generic.data;
        instance->stored_part1_low = (uint32_t)(key & 0xFFFFFFFFU);
        instance->stored_part1_high = (uint32_t)((key >> 32) & 0xFFFFFFFFU);

        uint32_t temp = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Key_2", &temp, 1)) {
            instance->stored_part2_low = temp;
        }
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Key_3", &temp, 1)) {
            instance->stored_part2_high = temp;
        }
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Key_4", &temp, 1)) {
            instance->data_part3 = (uint16_t)temp;
        }

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        flipper_format_rewind(flipper_format);
        const bool got_serial = flipper_format_read_uint32(flipper_format, "Serial", &serial, 1);
        flipper_format_rewind(flipper_format);
        const bool got_btn = flipper_format_read_uint32(flipper_format, "Btn", &btn, 1);
        flipper_format_rewind(flipper_format);
        const bool got_cnt = flipper_format_read_uint32(flipper_format, "Cnt", &cnt, 1);

        if(!got_serial || !got_btn || !got_cnt) {
            SubGhzProtocolDecoderKiaV6 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.stored_part1_low = instance->stored_part1_low;
            tmp.stored_part1_high = instance->stored_part1_high;
            tmp.stored_part2_low = instance->stored_part2_low;
            tmp.stored_part2_high = instance->stored_part2_high;
            tmp.data_part3 = instance->data_part3;
            if(kia_v6_decrypt(&tmp)) {
                if(!got_serial) serial = tmp.generic.serial;
                if(!got_btn) btn = tmp.generic.btn;
                if(!got_cnt) cnt = tmp.generic.cnt;
                instance->fx_field = tmp.fx_field;
            }
        }

        if(!subghz_custom_btn_has_original()) {
            subghz_custom_btn_set_original((uint8_t)btn);
        }
        btn = kia_default_get_btn_code();
        subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        if(!subghz_block_generic_global_counter_override_get(&cnt)) {
            cnt += furi_hal_subghz_get_rolling_counter_mult();
        }

        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)btn;
        instance->generic.cnt = cnt;

        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Fx", &temp, 1)) {
            instance->fx_field = (uint8_t)temp;
        }

        instance->encoder.repeat =
            (int32_t)(kia_v6_encoder_read_repeat(flipper_format, 3) & 0x7FFFFFFF);

        subghz_protocol_encoder_kia_v6_build_upload(instance);

        flipper_format_rewind(flipper_format);
        const uint64_t key1 = ((uint64_t)instance->stored_part1_high << 32) |
                              instance->stored_part1_low;
        char key_out[20];
        snprintf(key_out, sizeof(key_out), "%016llX", (unsigned long long)key1);
        flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key_out);
        uint32_t key2_lo = instance->stored_part2_low;
        flipper_format_insert_or_update_uint32(flipper_format, "Key_2", &key2_lo, 1);
        uint32_t key3_hi = instance->stored_part2_high;
        flipper_format_insert_or_update_uint32(flipper_format, "Key_3", &key3_hi, 1);
        uint32_t key4 = instance->data_part3;
        flipper_format_insert_or_update_uint32(flipper_format, "Key_4", &key4, 1);
        uint32_t serial_w = instance->generic.serial;
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->generic.btn;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->generic.cnt;
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        uint32_t fx_w = instance->fx_field;
        flipper_format_insert_or_update_uint32(flipper_format, "Fx", &fx_w, 1);
        subghz_block_generic_global.current_cnt = instance->generic.cnt;

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}
