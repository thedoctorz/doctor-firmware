#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/registry.h>
#include "psa.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t psa_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void psa_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t psa_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void psa_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static bool psa_read_key2_u64(FlipperFormat* flipper_format, uint64_t* out_key) {
    furi_check(out_key);
    uint8_t key_data[sizeof(uint64_t)] = {0};
    flipper_format_rewind(flipper_format);
    bool have_key2 = flipper_format_read_hex(flipper_format, "Key_2", key_data, sizeof(uint64_t));
    if(!have_key2) {
        flipper_format_rewind(flipper_format);
        have_key2 = flipper_format_read_hex(flipper_format, "Key2", key_data, sizeof(uint64_t));
    }
    if(have_key2) {
        *out_key = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            *out_key = (*out_key << 8) | key_data[i];
        }
        return true;
    }
    FuriString* value = furi_string_alloc();
    bool ok = false;
    flipper_format_rewind(flipper_format);
    have_key2 = flipper_format_read_string(flipper_format, "Key_2", value);
    if(!have_key2) {
        flipper_format_rewind(flipper_format);
        have_key2 = flipper_format_read_string(flipper_format, "Key2", value);
    }
    if(have_key2) {
        const char* str = furi_string_get_cstr(value);
        uint64_t key = 0;
        uint8_t hex_pos = 0;
        for(size_t i = 0; str[i] != '\0' && hex_pos < 16; i++) {
            if(str[i] == ' ') continue;
            uint8_t nibble = 0;
            if(str[i] >= '0' && str[i] <= '9') {
                nibble = (uint8_t)(str[i] - '0');
            } else if(str[i] >= 'A' && str[i] <= 'F') {
                nibble = (uint8_t)(str[i] - 'A' + 10);
            } else if(str[i] >= 'a' && str[i] <= 'f') {
                nibble = (uint8_t)(str[i] - 'a' + 10);
            } else {
                break;
            }
            key = (key << 4) | nibble;
            hex_pos++;
        }
        if(hex_pos == 16) {
            *out_key = key;
            ok = true;
        }
    }
    furi_string_free(value);
    return ok;
}

static SubGhzProtocolStatus
    psa_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void psa_encoder_read_fields(
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

static uint32_t psa_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    psa_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static SubGhzProtocolStatus psa_encoder_read_bit(
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

static void psa_get_string_setup_editor(
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

#define TAG "PSAProtocol"

static const SubGhzBlockConst subghz_protocol_psa_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 128,
};

#define PSA_TE_SHORT_125        0x7d
#define PSA_TE_LONG_250         0xfa
#define PSA_TE_END_1000         1000
#define PSA_TE_END_500          500
#define PSA_TOLERANCE_99        99
#define PSA_TOLERANCE_100       100
#define PSA_TOLERANCE_49        0x31
#define PSA_TOLERANCE_50        0x32
#define PSA_PATTERN_THRESHOLD_1 0x46
#define PSA_PATTERN_THRESHOLD_2 0x45
#define PSA_MAX_BITS            0x79
#define PSA_KEY1_BITS           0x40
#define PSA_KEY2_BITS           0x50
#define PSA_BUFFER_SIZE         48
#define PSA_TE_LONG_300         0x12c
#define PSA_UPLOAD_CAPACITY     325U
#define TEA_DELTA               0x9E3779B9U
#define TEA_ROUNDS              32

typedef struct {
    uint32_t s0[TEA_ROUNDS];
    uint32_t s1[TEA_ROUNDS];
} PsaTeaSchedule;

static void psa_tea_build_schedule(const uint32_t* key, PsaTeaSchedule* out) {
    for(int i = 0; i < TEA_ROUNDS; i++) {
        uint32_t sum0 = (uint32_t)((uint64_t)i * TEA_DELTA);
        uint32_t sum1 = (uint32_t)((uint64_t)(i + 1) * TEA_DELTA);
        out->s0[i] = key[sum0 & 3] + sum0;
        out->s1[i] = key[(sum1 >> 11) & 3] + sum1;
    }
}

static inline void
    psa_tea_encrypt_with_schedule(uint32_t* v0, uint32_t* v1, const PsaTeaSchedule* sched) {
    for(int i = 0; i < TEA_ROUNDS; i++) {
        *v0 += (sched->s0[i] ^ (((*v1 >> 5) ^ (*v1 << 4)) + *v1));
        *v1 += (sched->s1[i] ^ (((*v0 >> 5) ^ (*v0 << 4)) + *v0));
    }
}

#define PSA_BF1_CONST_U4 0x0E0F5C41U
#define PSA_BF1_CONST_U5 0x0F5C4123U

static const uint32_t PSA_BF1_KEY_SCHEDULE[4] = {
    0x4A434915U,
    0xD6743C2BU,
    0x1F29D308U,
    0xE6B79A64U,
};

static const uint32_t PSA_BF2_KEY_SCHEDULE[4] = {
    0x4039C240U,
    0xEDA92CABU,
    0x4306C02AU,
    0x02192A04U,
};

#define PSA_BF1_START 0x23000000U
#define PSA_BF1_END   0x24000000U
#define PSA_BF2_START 0xF3000000U
#define PSA_BF2_END   0xF4000000U

typedef enum {
    PSADecoderState0 = 0,
    PSADecoderState1 = 1,
    PSADecoderState2 = 2,
    PSADecoderState3 = 3,
    PSADecoderState4 = 4,
} PSADecoderState;

struct SubGhzProtocolDecoderPSA {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;

    uint32_t state;
    uint32_t prev_duration;

    uint32_t decode_data_low;
    uint32_t decode_data_high;
    uint8_t decode_count_bit;

    uint32_t seed;
    uint32_t key1_low;
    uint32_t key1_high;
    uint16_t validation_field;
    uint32_t key2_low;
    uint32_t key2_high;

    uint32_t status_flag;
    uint16_t decrypted;
    uint8_t mode_serialize;

    uint16_t pattern_counter;
    ManchesterState manchester_state;

    uint8_t decrypted_button;
    uint32_t decrypted_serial;
    uint32_t decrypted_counter;
    uint16_t decrypted_crc;
    uint32_t decrypted_seed;
    uint8_t decrypted_type;
};

static uint8_t psa_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x8;
    default:
        return subghz_custom_btn_get_original();
    }
}

struct SubGhzProtocolEncoderPSA {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t key1_low;
    uint32_t key1_high;
    uint16_t validation_field;
    uint32_t key2_low;
    uint32_t counter;
    uint8_t button;
    uint8_t type;
    uint8_t seed;
    uint8_t mode;
    uint32_t serial;
    uint16_t crc;
    bool is_running;
};

static bool psa_load_decrypted_from_flipper_format(
    SubGhzProtocolDecoderPSA* instance,
    FlipperFormat* flipper_format);
static void psa_decrypt_router(SubGhzProtocolDecoderPSA* instance, bool allow_bruteforce);

const SubGhzProtocolDecoder subghz_protocol_psa_decoder = {
    .alloc = subghz_protocol_decoder_psa_alloc,
    .free = subghz_protocol_decoder_psa_free,
    .feed = subghz_protocol_decoder_psa_feed,
    .reset = subghz_protocol_decoder_psa_reset,
    .get_hash_data = subghz_protocol_decoder_psa_get_hash_data,
    .serialize = subghz_protocol_decoder_psa_serialize,
    .deserialize = subghz_protocol_decoder_psa_deserialize,
    .get_string = subghz_protocol_decoder_psa_get_string,
};
const SubGhzProtocolEncoder subghz_protocol_psa_encoder = {
    .alloc = subghz_protocol_encoder_psa_alloc,
    .free = subghz_protocol_encoder_psa_free,
    .deserialize = subghz_protocol_encoder_psa_deserialize,
    .stop = subghz_protocol_encoder_psa_stop,
    .yield = subghz_protocol_encoder_psa_yield,
};

const SubGhzProtocol subghz_protocol_psa = {
    .name = SUBGHZ_PROTOCOL_PSA_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_psa_decoder,
    .encoder = &subghz_protocol_psa_encoder,
};

static void psa_setup_byte_buffer(
    uint8_t* buffer,
    uint32_t key1_low,
    uint32_t key1_high,
    uint32_t key2_low);
static void psa_calculate_checksum(uint8_t* buffer);
static uint8_t psa_calculate_tea_crc(uint32_t v0, uint32_t v1);
static void psa_tea_encrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key);
static void psa_unpack_tea_result_to_buffer(uint8_t* buffer, uint32_t v0, uint32_t v1);

static void psa_second_stage_xor_encrypt(uint8_t* buffer) {
    uint8_t E6 = buffer[8];
    uint8_t E7 = buffer[9];

    uint8_t P[6];
    P[0] = buffer[2];
    P[1] = buffer[3];
    P[2] = buffer[4];
    P[3] = buffer[5];
    P[4] = buffer[6];
    P[5] = buffer[7];

    uint8_t E5 = P[5] ^ E7 ^ E6;
    uint8_t E0 = P[2] ^ E5;
    uint8_t E2 = P[4] ^ E0;
    uint8_t E4 = P[3] ^ E2;
    uint8_t E3 = P[0] ^ E5;
    uint8_t E1 = P[1] ^ E3;

    buffer[2] = E0;
    buffer[3] = E1;
    buffer[4] = E2;
    buffer[5] = E3;
    buffer[6] = E4;
    buffer[7] = E5;
}

static void psa_build_buffer_mode23(
    SubGhzProtocolEncoderPSA* instance,
    uint8_t* buffer,
    uint8_t* preserve_buffer01) {
    memset(buffer, 0, PSA_BUFFER_SIZE);

    buffer[2] = (uint8_t)((instance->serial >> 16) & 0xFF);
    buffer[3] = (uint8_t)((instance->serial >> 8) & 0xFF);
    buffer[4] = (uint8_t)(instance->serial & 0xFF);
    buffer[5] = (uint8_t)((instance->counter >> 8) & 0xFF);
    buffer[6] = (uint8_t)(instance->counter & 0xFF);
    buffer[7] = (uint8_t)(instance->crc & 0xFF);
    buffer[8] = (uint8_t)(instance->button & 0xF);

    uint8_t original_buffer9 = 0;
    bool has_original_key2 = (instance->key2_low != 0);
    if(has_original_key2) {
        original_buffer9 = (uint8_t)(instance->key2_low & 0xFF);
        buffer[9] = original_buffer9;
    } else {
    }
    uint8_t initial_plaintext[6];
    initial_plaintext[0] = buffer[2];
    initial_plaintext[1] = buffer[3];
    initial_plaintext[2] = buffer[4];
    initial_plaintext[3] = buffer[5];
    initial_plaintext[4] = buffer[6];
    initial_plaintext[5] = buffer[7];
    uint8_t initial_button = buffer[8] & 0xF;

    bool found = false;
    uint8_t buffer9_to_use = has_original_key2 ? original_buffer9 : 0;
    uint8_t buffer9_end = has_original_key2 ? original_buffer9 + 1 : 255;

    for(uint8_t buffer9_try = buffer9_to_use; buffer9_try < buffer9_end && !found; buffer9_try++) {
        for(uint8_t buffer8_high_try = 0; buffer8_high_try < 16 && !found; buffer8_high_try++) {
            buffer[2] = initial_plaintext[0];
            buffer[3] = initial_plaintext[1];
            buffer[4] = initial_plaintext[2];
            buffer[5] = initial_plaintext[3];
            buffer[6] = initial_plaintext[4];
            buffer[7] = initial_plaintext[5];
            buffer[8] = initial_button | (buffer8_high_try << 4);
            buffer[9] = buffer9_try;

            psa_second_stage_xor_encrypt(buffer);

            psa_calculate_checksum(buffer);
            uint8_t checksum_after = buffer[11];
            uint8_t key2_high_after = checksum_after & 0xF0;

            uint8_t validation = (checksum_after ^ buffer[8]) & 0xF0;
            if(validation == 0) {
                buffer[8] = (buffer[8] & 0x0F) | key2_high_after;
                buffer[13] = buffer[9] ^ buffer[8];
                found = true;
                break;
            }
        }
    }

    if(!found) {
        FURI_LOG_W(TAG, "Brute force failed, using default approach");
        buffer[2] = initial_plaintext[0];
        buffer[3] = initial_plaintext[1];
        buffer[4] = initial_plaintext[2];
        buffer[5] = initial_plaintext[3];
        buffer[6] = initial_plaintext[4];
        buffer[7] = initial_plaintext[5];
        buffer[8] = initial_button;
        buffer[9] = has_original_key2 ? original_buffer9 : 0x23;

        psa_second_stage_xor_encrypt(buffer);
        psa_calculate_checksum(buffer);
        uint8_t checksum_after = buffer[11];
        uint8_t key2_high_after = checksum_after & 0xF0;
        buffer[8] = (buffer[8] & 0x0F) | key2_high_after;
        buffer[13] = buffer[9] ^ buffer[8];
    }

    if(preserve_buffer01 != NULL) {
        buffer[0] = preserve_buffer01[0];
        buffer[1] = preserve_buffer01[1];
    } else {
        buffer[0] = buffer[2] ^ buffer[6];
        buffer[1] = buffer[3] ^ buffer[7];
    }
}

static void psa_build_buffer_mode36(
    SubGhzProtocolEncoderPSA* instance,
    uint8_t* buffer,
    uint8_t* preserve_buffer01) {
    memset(buffer, 0, PSA_BUFFER_SIZE);

    uint32_t v0 = ((instance->serial & 0xFFFFFF) << 8) | ((instance->button & 0xF) << 4) |
                  ((instance->counter >> 24) & 0xF);
    uint32_t v1 = ((instance->counter & 0xFFFFFF) << 8) | (instance->crc & 0xFF);
    uint8_t crc = psa_calculate_tea_crc(v0, v1);
    v1 = (v1 & 0xFFFFFF00) | crc;
    uint32_t bf_counter = PSA_BF1_START | (instance->serial & 0xFFFFFF);
    uint32_t working_key[4];

    uint32_t wk2 = PSA_BF1_CONST_U4;
    uint32_t wk3 = bf_counter;
    psa_tea_encrypt(&wk2, &wk3, PSA_BF1_KEY_SCHEDULE);

    uint32_t wk0 = (bf_counter << 8) | 0x0E;
    uint32_t wk1 = PSA_BF1_CONST_U5;
    psa_tea_encrypt(&wk0, &wk1, PSA_BF1_KEY_SCHEDULE);

    working_key[0] = wk0;
    working_key[1] = wk1;
    working_key[2] = wk2;
    working_key[3] = wk3;

    psa_tea_encrypt(&v0, &v1, working_key);

    psa_unpack_tea_result_to_buffer(buffer, v0, v1);

    if(preserve_buffer01 != NULL) {
        buffer[0] = preserve_buffer01[0];
        buffer[1] = preserve_buffer01[1];
    } else {
        buffer[0] = buffer[2] ^ buffer[6];
        buffer[1] = buffer[3] ^ buffer[7];
    }
}

static void psa_encoder_build_upload(SubGhzProtocolEncoderPSA* instance) {
    furi_check(instance);

    uint8_t buffer[PSA_BUFFER_SIZE] = {0};

    uint8_t preserve_buffer01[2] = {0};
    uint8_t* preserve_ptr = NULL;

    if(instance->key1_low != 0 || instance->key1_high != 0) {
        uint8_t orig_buffer[PSA_BUFFER_SIZE] = {0};
        psa_setup_byte_buffer(
            orig_buffer, instance->key1_low, instance->key1_high, instance->key2_low);
        preserve_buffer01[0] = orig_buffer[0];
        preserve_buffer01[1] = orig_buffer[1];
        preserve_ptr = preserve_buffer01;
    } else {
    }

    if(instance->mode == 0x23) {
        psa_build_buffer_mode23(instance, buffer, preserve_ptr);
    } else if(instance->mode == 0x36) {
        psa_build_buffer_mode36(instance, buffer, preserve_ptr);
    } else {
        FURI_LOG_E(TAG, "Unknown mode: 0x%02X", instance->mode);
        return;
    }

    uint32_t key1_high = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                         ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
    uint32_t key1_low = ((uint32_t)buffer[4] << 24) | ((uint32_t)buffer[5] << 16) |
                        ((uint32_t)buffer[6] << 8) | (uint32_t)buffer[7];
    uint16_t validation_field = ((uint16_t)buffer[8] << 8) | (uint16_t)buffer[9];

    size_t index = 0;
    uint32_t te = PSA_TE_LONG_250;

    for(int i = 0; i < 80; i++) {
        if(index >= instance->encoder.size_upload - 2) break;
        instance->encoder.upload[index++] = level_duration_make(true, te);
        instance->encoder.upload[index++] = level_duration_make(false, te);
    }

    uint32_t te_long_transition = subghz_protocol_psa_const.te_long;
    if(index < instance->encoder.size_upload - 3) {
        instance->encoder.upload[index++] = level_duration_make(false, te);
        instance->encoder.upload[index++] = level_duration_make(true, te_long_transition);
        instance->encoder.upload[index++] = level_duration_make(false, te);
    }

    uint64_t key1_data = ((uint64_t)key1_high << 32) | key1_low;
    for(int bit = 63; bit >= 0; bit--) {
        if(index >= instance->encoder.size_upload - 2) break;
        bool bit_value = (key1_data >> bit) & 1;
        if(bit_value) {
            instance->encoder.upload[index++] = level_duration_make(true, te);
            instance->encoder.upload[index++] = level_duration_make(false, te);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, te);
            instance->encoder.upload[index++] = level_duration_make(true, te);
        }
    }

    for(int bit = 15; bit >= 0; bit--) {
        if(index >= instance->encoder.size_upload - 2) break;
        bool bit_value = (validation_field >> bit) & 1;
        if(bit_value) {
            instance->encoder.upload[index++] = level_duration_make(true, te);
            instance->encoder.upload[index++] = level_duration_make(false, te);
        } else {
            instance->encoder.upload[index++] = level_duration_make(false, te);
            instance->encoder.upload[index++] = level_duration_make(true, te);
        }
    }

    uint32_t end_duration = PSA_TE_END_1000;
    if(index < instance->encoder.size_upload - 1) {
        instance->encoder.upload[index++] = level_duration_make(true, end_duration);
        instance->encoder.upload[index++] = level_duration_make(false, end_duration);
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
    instance->encoder.repeat = 3;

    instance->key1_high = key1_high;
    instance->key1_low = key1_low;
    instance->validation_field = validation_field;
    instance->key2_low = (instance->key2_low & 0xFFFF0000UL) | validation_field;
}

void* subghz_protocol_encoder_psa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPSA* instance = malloc(sizeof(SubGhzProtocolEncoderPSA));

    if(instance) {
        memset(instance, 0, sizeof(SubGhzProtocolEncoderPSA));
        instance->base.protocol = &subghz_protocol_psa;
        instance->generic.protocol_name = instance->base.protocol->name;

        if(!instance->encoder.upload || instance->encoder.size_upload < PSA_UPLOAD_CAPACITY) {
            free(instance->encoder.upload);
            instance->encoder.upload = malloc(PSA_UPLOAD_CAPACITY * sizeof(LevelDuration));
            furi_check(instance->encoder.upload);
        }
        instance->encoder.size_upload = PSA_UPLOAD_CAPACITY;
        instance->encoder.repeat = 3;
        instance->encoder.front = 0;
        instance->encoder.is_running = false;
        instance->is_running = false;
    }

    return instance;
}

void subghz_protocol_encoder_psa_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;

    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_psa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;

    uint64_t key1 = 0;
    if(!subghz_block_generic_read_key_u64(flipper_format, &key1)) {
        return SubGhzProtocolStatusError;
    }
    instance->key1_low = (uint32_t)(key1 & 0xFFFFFFFFU);
    instance->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFFU);

    uint64_t key2 = 0;
    if(!psa_read_key2_u64(flipper_format, &key2)) {
        return SubGhzProtocolStatusError;
    }
    instance->key2_low = (uint32_t)(key2 & 0xFFFFFFFFU);
    instance->validation_field = (uint16_t)(key2 & 0xFFFFU);

    SubGhzProtocolDecoderPSA decoder = {0};
    decoder.key1_low = instance->key1_low;
    decoder.key1_high = instance->key1_high;
    decoder.key2_low = instance->key2_low;
    decoder.key2_high = (uint32_t)((key2 >> 32) & 0xFFFFFFFFU);

    uint32_t validation = 0;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "ValidationField", &validation, 1)) {
        decoder.validation_field = (uint16_t)(validation & 0xFFFFU);
        instance->validation_field = decoder.validation_field;
    }

    if(!psa_load_decrypted_from_flipper_format(&decoder, flipper_format)) {
        psa_decrypt_router(&decoder, false);
    }

    if(decoder.decrypted != 0x50 || decoder.decrypted_type == 0) {
        return SubGhzProtocolStatusError;
    }

    uint32_t serial = decoder.decrypted_serial;
    uint32_t btn = decoder.decrypted_button;
    uint32_t cnt = decoder.decrypted_counter;
    uint32_t type_val = decoder.decrypted_type;

    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original((uint8_t)btn);
    }
    btn = psa_get_btn_code();
    subghz_block_generic_global_button_override_get((uint8_t*)&btn);
    if(!subghz_block_generic_global_counter_override_get(&cnt)) {
        cnt += furi_hal_subghz_get_rolling_counter_mult();
    }

    instance->serial = serial;
    instance->counter = cnt;
    instance->button = (uint8_t)(btn & 0xFFU);
    instance->type = (uint8_t)(type_val & 0xFFU);
    instance->crc = (uint16_t)(decoder.decrypted_crc & 0xFFFFU);
    instance->seed = (uint8_t)(decoder.decrypted_seed & 0xFFU);

    instance->mode = (instance->type == 0x36) ? 0x36 : 0x23;

    psa_encoder_build_upload(instance);

    flipper_format_rewind(flipper_format);
    char key1_str[20];
    const uint64_t tx_key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    snprintf(key1_str, sizeof(key1_str), "%016llX", (unsigned long long)tx_key1);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key1_str);
    char key2_str[20];
    snprintf(key2_str, sizeof(key2_str), "%016llX", (unsigned long long)instance->key2_low);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Key_2", key2_str);
    uint32_t serial_w = instance->serial;
    flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
    uint32_t btn_w = instance->button;
    flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
    uint32_t cnt_w = instance->counter;
    flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    uint32_t type_w = instance->type;
    flipper_format_insert_or_update_uint32(flipper_format, "Type", &type_w, 1);
    uint32_t crc_w = instance->crc;
    flipper_format_insert_or_update_uint32(flipper_format, "CRC", &crc_w, 1);
    uint32_t seed_w = instance->seed;
    flipper_format_insert_or_update_uint32(flipper_format, "Seed", &seed_w, 1);
    uint32_t val_w = instance->validation_field;
    flipper_format_insert_or_update_uint32(flipper_format, "ValidationField", &val_w, 1);
    subghz_block_generic_global.current_cnt = instance->counter;

    instance->is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_psa_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;
    instance->is_running = false;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_psa_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;

    if(!instance->is_running || instance->encoder.size_upload == 0) {
        instance->is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    instance->encoder.front++;

    if(instance->encoder.front >= instance->encoder.size_upload) {
        instance->encoder.front = 0;
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        if(instance->encoder.repeat <= 0) {
            instance->is_running = false;
        }
    }

    return ret;
}
static uint32_t psa_abs_diff(uint32_t a, uint32_t b) {
    if(a < b) {
        return b - a;
    } else {
        return a - b;
    }
}

static bool psa_is_te_125_short(uint32_t duration) {
    return psa_abs_diff(duration, PSA_TE_SHORT_125) <= PSA_TOLERANCE_49;
}

static bool psa_is_te_125_long(uint32_t duration) {
    return psa_abs_diff(duration, PSA_TE_LONG_250) <= PSA_TOLERANCE_49;
}

static void psa_setup_byte_buffer(
    uint8_t* buffer,
    uint32_t key1_low,
    uint32_t key1_high,
    uint32_t key2_low) {
    for(int i = 0; i < 8; i++) {
        int shift = i * 8;
        uint8_t byte_val;
        if(shift < 32) {
            byte_val = (uint8_t)((key1_low >> shift) & 0xFF);
        } else {
            byte_val = (uint8_t)((key1_high >> (shift - 32)) & 0xFF);
        }
        buffer[7 - i] = byte_val;
    }
    buffer[9] = (uint8_t)(key2_low & 0xFF);
    buffer[8] = (uint8_t)((key2_low >> 8) & 0xFF);
}

static bool psa_direct_xor_allowed_by_key2(uint8_t key2_high_byte) {
    uint8_t lo = key2_high_byte & 0xf;
    if(lo < 3) return true;
    if(lo < 7 && (key2_high_byte & 0xc) != 0) return true;
    return false;
}

static void psa_calculate_checksum(uint8_t* buffer) {
    uint32_t checksum = 0;
    for(int i = 2; i < 8; i++) {
        checksum += (buffer[i] & 0xF) + ((buffer[i] >> 4) & 0xF);
    }
    buffer[11] = (uint8_t)((checksum * 0x10) & 0xFF);
}

static void psa_copy_reverse(uint8_t* temp, uint8_t* source) {
    temp[0] = source[5];
    temp[1] = source[4];
    temp[2] = source[3];
    temp[3] = source[2];
    temp[4] = source[9];
    temp[5] = source[8];
    temp[6] = source[7];
    temp[7] = source[6];
}

static void psa_second_stage_xor_decrypt(uint8_t* buffer) {
    uint8_t temp[8];
    psa_copy_reverse(temp, buffer);
    buffer[2] = temp[0] ^ temp[6];
    buffer[3] = temp[2] ^ temp[0];
    buffer[4] = temp[6] ^ temp[3];
    buffer[5] = temp[7] ^ temp[1];
    buffer[6] = temp[3] ^ temp[1];
    buffer[7] = temp[6] ^ temp[4] ^ temp[5];
}

static void psa_tea_encrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key) {
    uint32_t sum = 0;
    for(int i = 0; i < TEA_ROUNDS; i++) {
        uint32_t k_idx1 = sum & 3;
        uint32_t temp = key[k_idx1] + sum;
        sum = sum + TEA_DELTA;
        *v0 = *v0 + (temp ^ (((*v1 >> 5) ^ (*v1 << 4)) + *v1));
        uint32_t k_idx2 = (sum >> 11) & 3;
        temp = key[k_idx2] + sum;
        *v1 = *v1 + (temp ^ (((*v0 >> 5) ^ (*v0 << 4)) + *v0));
    }
}
static inline void psa_tea_decrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key) {
    uint32_t sum = TEA_DELTA * TEA_ROUNDS;
    for(int i = 0; i < TEA_ROUNDS; i++) {
        uint32_t k_idx2 = (sum >> 11) & 3;
        uint32_t temp = key[k_idx2] + sum;
        sum = sum - TEA_DELTA;
        *v1 = *v1 - (temp ^ (((*v0 >> 5) ^ (*v0 << 4)) + *v0));
        uint32_t k_idx1 = sum & 3;
        temp = key[k_idx1] + sum;
        *v0 = *v0 - (temp ^ (((*v1 >> 5) ^ (*v1 << 4)) + *v1));
    }
}

static void psa_prepare_tea_data(uint8_t* buffer, uint32_t* w0, uint32_t* w1) {
    *w0 = ((uint32_t)buffer[3] << 16) | ((uint32_t)buffer[2] << 24) | ((uint32_t)buffer[4] << 8) |
          (uint32_t)buffer[5];
    *w1 = ((uint32_t)buffer[7] << 16) | ((uint32_t)buffer[6] << 24) | ((uint32_t)buffer[8] << 8) |
          (uint32_t)buffer[9];
}

static uint8_t psa_calculate_tea_crc(uint32_t v0, uint32_t v1) {
    uint32_t crc = ((v0 >> 24) & 0xFF) + ((v0 >> 16) & 0xFF) + ((v0 >> 8) & 0xFF) + (v0 & 0xFF);
    crc += ((v1 >> 24) & 0xFF) + ((v1 >> 16) & 0xFF) + ((v1 >> 8) & 0xFF);
    return (uint8_t)(crc & 0xFF);
}

static uint16_t psa_calculate_crc16_bf2(uint8_t* buffer, int length) {
    uint16_t crc = 0;
    for(int i = 0; i < length; i++) {
        crc = crc ^ ((uint16_t)buffer[i] << 8);
        for(int j = 0; j < 8; j++) {
            if(crc & 0x8000) {
                crc = (crc << 1) ^ 0x8005;
            } else {
                crc = crc << 1;
            }
            crc = crc & 0xFFFF;
        }
    }
    return crc & 0xFFFF;
}

static void psa_unpack_tea_result_to_buffer(uint8_t* buffer, uint32_t v0, uint32_t v1) {
    buffer[2] = (uint8_t)((v0 >> 24) & 0xFF);
    buffer[3] = (uint8_t)((v0 >> 16) & 0xFF);
    buffer[4] = (uint8_t)((v0 >> 8) & 0xFF);
    buffer[5] = (uint8_t)(v0 & 0xFF);
    buffer[6] = (uint8_t)((v1 >> 24) & 0xFF);
    buffer[7] = (uint8_t)((v1 >> 16) & 0xFF);
    buffer[8] = (uint8_t)((v1 >> 8) & 0xFF);
    buffer[9] = (uint8_t)(v1 & 0xFF);
}

static void psa_extract_fields_mode23(uint8_t* buffer, SubGhzProtocolDecoderPSA* instance) {
    instance->decrypted_button = buffer[8] & 0xF;
    instance->decrypted_serial = ((uint32_t)buffer[3] << 8) | ((uint32_t)buffer[2] << 16) |
                                 (uint32_t)buffer[4];
    instance->decrypted_counter = (uint32_t)buffer[6] | ((uint32_t)buffer[5] << 8);
    instance->decrypted_crc = (uint16_t)buffer[7];
    instance->decrypted_type = 0x23;
    instance->decrypted_seed = instance->decrypted_serial;
}

static void psa_extract_fields_mode36(uint8_t* buffer, SubGhzProtocolDecoderPSA* instance) {
    instance->decrypted_button = (buffer[5] >> 4) & 0xF;
    instance->decrypted_serial = ((uint32_t)buffer[3] << 8) | ((uint32_t)buffer[2] << 16) |
                                 (uint32_t)buffer[4];
    instance->decrypted_counter = ((uint32_t)buffer[7] << 8) | ((uint32_t)buffer[6] << 16) |
                                  (uint32_t)buffer[8] | (((uint32_t)buffer[5] & 0xF) << 24);
    instance->decrypted_crc = (uint16_t)buffer[9];
    instance->decrypted_type = 0x36;
    instance->decrypted_seed = instance->decrypted_serial;
}

static bool psa_brute_force_decrypt_bf1(
    SubGhzProtocolDecoderPSA* instance,
    uint8_t* buffer,
    uint32_t w0,
    uint32_t w1) {
    PsaTeaSchedule bf1_sched;
    psa_tea_build_schedule(PSA_BF1_KEY_SCHEDULE, &bf1_sched);

    for(uint32_t counter = PSA_BF1_START; counter < PSA_BF1_END; counter++) {
        uint32_t wk2 = PSA_BF1_CONST_U4;
        uint32_t wk3 = counter;
        psa_tea_encrypt_with_schedule(&wk2, &wk3, &bf1_sched);

        uint32_t wk0 = (counter << 8) | 0x0E;
        uint32_t wk1 = PSA_BF1_CONST_U5;
        psa_tea_encrypt_with_schedule(&wk0, &wk1, &bf1_sched);

        uint32_t working_key[4] = {wk0, wk1, wk2, wk3};

        uint32_t dec_v0 = w0;
        uint32_t dec_v1 = w1;
        psa_tea_decrypt(&dec_v0, &dec_v1, working_key);

        if((counter & 0xFFFFFF) == (dec_v0 >> 8)) {
            uint8_t crc = psa_calculate_tea_crc(dec_v0, dec_v1);
            if(crc == (dec_v1 & 0xFF)) {
                psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);
                psa_extract_fields_mode36(buffer, instance);
                return true;
            }
        }
    }
    return false;
}

static bool psa_brute_force_decrypt_bf2(
    SubGhzProtocolDecoderPSA* instance,
    uint8_t* buffer,
    uint32_t w0,
    uint32_t w1) {
    for(uint32_t counter = PSA_BF2_START; counter < PSA_BF2_END; counter++) {
        uint32_t working_key[4] = {
            PSA_BF2_KEY_SCHEDULE[0] ^ counter,
            PSA_BF2_KEY_SCHEDULE[1] ^ counter,
            PSA_BF2_KEY_SCHEDULE[2] ^ counter,
            PSA_BF2_KEY_SCHEDULE[3] ^ counter,
        };

        uint32_t dec_v0 = w0;
        uint32_t dec_v1 = w1;
        psa_tea_decrypt(&dec_v0, &dec_v1, working_key);

        if((counter & 0xFFFFFF) == (dec_v0 >> 8)) {
            psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);

            uint8_t crc_buffer[6] = {
                (uint8_t)((dec_v0 >> 24) & 0xFF),
                (uint8_t)((dec_v0 >> 8) & 0xFF),
                (uint8_t)((dec_v0 >> 16) & 0xFF),
                (uint8_t)(dec_v0 & 0xFF),
                (uint8_t)((dec_v1 >> 24) & 0xFF),
                (uint8_t)((dec_v1 >> 16) & 0xFF),
            };
            uint16_t crc16 = psa_calculate_crc16_bf2(crc_buffer, 6);

            uint16_t expected_crc = (uint16_t)(dec_v1 & 0xFFFF);

            if(crc16 == expected_crc) {
                psa_extract_fields_mode36(buffer, instance);
                return true;
            }
        }
    }
    return false;
}

static void psa_fill_bf_state_from_buffer(PsaBfState* state, uint8_t* buffer) {
    state->decrypted_button = (buffer[5] >> 4) & 0xF;
    state->decrypted_serial = ((uint32_t)buffer[3] << 8) | ((uint32_t)buffer[2] << 16) |
                              (uint32_t)buffer[4];
    state->decrypted_counter = ((uint32_t)buffer[7] << 8) | ((uint32_t)buffer[6] << 16) |
                               (uint32_t)buffer[8] | (((uint32_t)buffer[5] & 0xF) << 24);
    state->decrypted_crc = (uint16_t)buffer[9];
    state->decrypted_seed = state->decrypted_serial;
    state->decrypted_type = 0x36;
}

#define PSA_BF_PROGRESS_INTERVAL 4096U

void psa_brute_force_run(PsaBfState* state) {
    uint8_t buffer[48] = {0};
    psa_setup_byte_buffer(buffer, state->key1_low, state->key1_high, state->key2_low);
    uint32_t w0, w1;
    psa_prepare_tea_data(buffer, &w0, &w1);

    state->progress_current = 0;
    state->progress_total = (PSA_BF1_END - PSA_BF1_START) + (PSA_BF2_END - PSA_BF2_START);
    state->status = PSA_BF_STATUS_RUNNING;

    PsaTeaSchedule bf1_sched;
    psa_tea_build_schedule(PSA_BF1_KEY_SCHEDULE, &bf1_sched);

    for(uint32_t counter = PSA_BF1_START; counter < PSA_BF1_END; counter++) {
        if(state->cancel) {
            state->status = PSA_BF_STATUS_CANCELLED;
            return;
        }
        if((counter & (PSA_BF_PROGRESS_INTERVAL - 1)) == 0) {
            state->progress_current = counter - PSA_BF1_START;
        }

        uint32_t wk2 = PSA_BF1_CONST_U4;
        uint32_t wk3 = counter;
        psa_tea_encrypt_with_schedule(&wk2, &wk3, &bf1_sched);
        uint32_t wk0 = (counter << 8) | 0x0E;
        uint32_t wk1 = PSA_BF1_CONST_U5;
        psa_tea_encrypt_with_schedule(&wk0, &wk1, &bf1_sched);
        uint32_t working_key[4] = {wk0, wk1, wk2, wk3};

        uint32_t dec_v0 = w0;
        uint32_t dec_v1 = w1;
        psa_tea_decrypt(&dec_v0, &dec_v1, working_key);

        if((counter & 0xFFFFFF) == (dec_v0 >> 8)) {
            uint8_t crc = psa_calculate_tea_crc(dec_v0, dec_v1);
            if(crc == (dec_v1 & 0xFF)) {
                psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);
                psa_fill_bf_state_from_buffer(state, buffer);
                state->progress_current = counter - PSA_BF1_START;
                state->status = PSA_BF_STATUS_FOUND;
                return;
            }
        }
    }

    state->progress_current = PSA_BF1_END - PSA_BF1_START;

    for(uint32_t counter = PSA_BF2_START; counter < PSA_BF2_END; counter++) {
        if(state->cancel) {
            state->status = PSA_BF_STATUS_CANCELLED;
            return;
        }
        if((counter & (PSA_BF_PROGRESS_INTERVAL - 1)) == 0) {
            state->progress_current = (PSA_BF1_END - PSA_BF1_START) + (counter - PSA_BF2_START);
        }

        uint32_t working_key[4] = {
            PSA_BF2_KEY_SCHEDULE[0] ^ counter,
            PSA_BF2_KEY_SCHEDULE[1] ^ counter,
            PSA_BF2_KEY_SCHEDULE[2] ^ counter,
            PSA_BF2_KEY_SCHEDULE[3] ^ counter,
        };
        uint32_t dec_v0 = w0;
        uint32_t dec_v1 = w1;
        psa_tea_decrypt(&dec_v0, &dec_v1, working_key);

        if((counter & 0xFFFFFF) == (dec_v0 >> 8)) {
            psa_unpack_tea_result_to_buffer(buffer, dec_v0, dec_v1);
            uint8_t crc_buffer[6] = {
                (uint8_t)((dec_v0 >> 24) & 0xFF),
                (uint8_t)((dec_v0 >> 8) & 0xFF),
                (uint8_t)((dec_v0 >> 16) & 0xFF),
                (uint8_t)(dec_v0 & 0xFF),
                (uint8_t)((dec_v1 >> 24) & 0xFF),
                (uint8_t)((dec_v1 >> 16) & 0xFF),
            };
            uint16_t crc16 = psa_calculate_crc16_bf2(crc_buffer, 6);
            uint16_t expected_crc = (uint16_t)(dec_v1 & 0xFFFF);
            if(crc16 == expected_crc) {
                psa_fill_bf_state_from_buffer(state, buffer);
                state->progress_current =
                    (PSA_BF1_END - PSA_BF1_START) + (counter - PSA_BF2_START);
                state->status = PSA_BF_STATUS_FOUND;
                return;
            }
        }
    }

    state->status = PSA_BF_STATUS_NOT_FOUND;
}

int32_t psa_brute_force_thread_entry(void* arg) {
    PsaBfState* state = arg;
    psa_brute_force_run(state);

    if(state->on_done) {
        state->on_done(state->on_done_ctx);
    }
    return 0;
}

bool psa_bf_state_from_flipper_format(PsaBfState* state, FlipperFormat* ff) {
    furi_check(state);
    furi_check(ff);
    bool ok = false;
    do {
        uint64_t key1 = 0;
        if(!subghz_block_generic_read_key_u64(ff, &key1)) break;
        state->key1_low = (uint32_t)(key1 & 0xFFFFFFFF);
        state->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFF);

        uint64_t key2 = 0;
        if(!psa_read_key2_u64(ff, &key2)) break;
        state->key2_low = (uint16_t)(key2 & 0xFFFF);

        state->cancel = 0;
        state->progress_current = 0;
        state->progress_total = 0;
        state->status = PSA_BF_STATUS_IDLE;
        state->on_done = NULL;
        state->on_done_ctx = NULL;
        ok = true;
    } while(false);
    return ok;
}

bool psa_bf_needs_bruteforce(FlipperFormat* ff, bool require_psa_protocol) {
    if(!ff) return false;
    FuriString* s = furi_string_alloc();
    flipper_format_rewind(ff);
    if(require_psa_protocol) {
        if(!flipper_format_read_string(ff, "Protocol", s) ||
           !furi_string_equal_str(s, SUBGHZ_PROTOCOL_PSA_NAME)) {
            furi_string_free(s);
            return false;
        }
        flipper_format_rewind(ff);
    }
    bool has_key = flipper_format_read_string(ff, "Key", s);
    if(!has_key) {
        furi_string_free(s);
        return false;
    }
    uint32_t serial = 0;
    flipper_format_rewind(ff);
    bool has_serial = flipper_format_read_uint32(ff, "Serial", &serial, 1);
    furi_string_free(s);
    return !has_serial;
}

bool psa_bf_patch_flipper_format_on_success(FlipperFormat* ff, const PsaBfState* state) {
    if(!ff || !state) return false;
    flipper_format_rewind(ff);
    if(!flipper_format_insert_or_update_uint32(ff, "Serial", &state->decrypted_serial, 1)) {
        return false;
    }
    uint32_t btn = state->decrypted_button;
    flipper_format_insert_or_update_uint32(ff, "Btn", &btn, 1);
    flipper_format_insert_or_update_uint32(ff, "Cnt", &state->decrypted_counter, 1);
    uint32_t type = state->decrypted_type;
    flipper_format_insert_or_update_uint32(ff, "Type", &type, 1);
    uint32_t crc_val = state->decrypted_crc;
    flipper_format_insert_or_update_uint32(ff, "CRC", &crc_val, 1);
    flipper_format_insert_or_update_uint32(ff, "Seed", &state->decrypted_seed, 1);
    return true;
}

static bool psa_direct_xor_decrypt(SubGhzProtocolDecoderPSA* instance, uint8_t* buffer) {
    psa_calculate_checksum(buffer);
    uint8_t checksum = buffer[11];
    uint8_t key2_high = buffer[8];

    uint8_t validation_result = (checksum ^ key2_high) & 0xF0;
    if(validation_result == 0) {
        buffer[13] = buffer[9] ^ buffer[8];
        psa_second_stage_xor_decrypt(buffer);
        psa_extract_fields_mode23(buffer, instance);
        return true;
    }
    return false;
}

static void psa_handle_decoded_frame(SubGhzProtocolDecoderPSA* instance, uint8_t direct_xor_mode);

static bool psa_try_complete_at_key2(SubGhzProtocolDecoderPSA* instance, uint8_t direct_xor_mode) {
    if(instance->decode_count_bit != PSA_KEY2_BITS) {
        return false;
    }
    if(((instance->key1_high >> 16) & 0xF) != 0xA) {
        instance->decode_data_low = 0;
        instance->decode_data_high = 0;
        instance->decode_count_bit = 0;
        instance->state = PSADecoderState0;
        return true;
    }
    psa_handle_decoded_frame(instance, direct_xor_mode);
    instance->state = PSADecoderState0;
    return true;
}

static void psa_handle_decoded_frame(SubGhzProtocolDecoderPSA* instance, uint8_t direct_xor_mode) {
    instance->key2_low = instance->decode_data_low;
    instance->key2_high = instance->decode_data_high;
    instance->validation_field = (uint16_t)(instance->decode_data_low & 0xFFFF);
    instance->decrypted_type = 0;
    instance->decrypted_button = 0;
    instance->decrypted_serial = 0;
    instance->decrypted_counter = 0;
    instance->decrypted_crc = 0;
    instance->decrypted_seed = 0;
    instance->decrypted = 0x00;
    instance->mode_serialize = psa_direct_xor_allowed_by_key2((uint8_t)(instance->key2_low >> 8)) ?
                                   direct_xor_mode :
                                   0x36;
    instance->status_flag = 0x80;
    uint8_t buffer[PSA_BUFFER_SIZE] = {0};
    psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);
    if(instance->mode_serialize != 0x36 && psa_direct_xor_decrypt(instance, buffer)) {
        instance->mode_serialize = 0x23;
        instance->decrypted = 0x50;
    } else {
        instance->decrypted = 0x00;
        instance->mode_serialize = 0x36;
    }

    bool publish = (instance->decrypted == 0x50);
    if(!publish) {
        const uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
        const uint8_t marker_byte = (uint8_t)((instance->key1_high >> 16) & 0xFFU);
        const uint8_t key2_high_byte = (uint8_t)(instance->key2_low >> 8);
        if(!psa_direct_xor_allowed_by_key2(key2_high_byte) &&
           !subghz_protocol_blocks_key_is_degenerate(key1, 64) && (marker_byte != 0xAAU) &&
           (marker_byte != 0x55U) && (marker_byte != 0x00U) && (marker_byte != 0xFFU)) {
            publish = true;
        }
    }

    if(publish && instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    instance->decode_data_low = 0;
    instance->decode_data_high = 0;
    instance->decode_count_bit = 0;
    instance->state = PSADecoderState0;
    instance->pattern_counter = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

static void psa_decrypt_router(SubGhzProtocolDecoderPSA* instance, bool allow_bruteforce) {
    uint8_t buffer[PSA_BUFFER_SIZE] = {0};

    psa_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);

    uint8_t key2_high_byte = buffer[8];
    uint8_t mode = instance->mode_serialize;

    if(mode == 1 || mode == 2) {
        if(!psa_direct_xor_allowed_by_key2(key2_high_byte)) {
            mode = 0x36;
        } else if(psa_direct_xor_decrypt(instance, buffer)) {
            mode = 0x23;
        } else {
            mode = 0x36;
        }
    }

    if(mode == 0x23) {
        if(psa_direct_xor_decrypt(instance, buffer)) {
            instance->mode_serialize = 0x23;
            instance->decrypted = 0x50;
            return;
        }
        FURI_LOG_W(TAG, "Direct XOR decryption FAILED");
    } else if(mode == 0x36) {
        return;
    } else {
        if(psa_direct_xor_allowed_by_key2(key2_high_byte) &&
           psa_direct_xor_decrypt(instance, buffer)) {
            instance->mode_serialize = 0x23;
            instance->decrypted = 0x50;
            return;
        }
        if(!allow_bruteforce) {
            return;
        }
        uint32_t w0, w1;
        psa_prepare_tea_data(buffer, &w0, &w1);

        if(psa_brute_force_decrypt_bf1(instance, buffer, w0, w1)) {
            instance->mode_serialize = 0x36;
            instance->decrypted = 0x50;
            return;
        }
        if(psa_brute_force_decrypt_bf2(instance, buffer, w0, w1)) {
            instance->mode_serialize = 0x36;
            instance->decrypted = 0x50;
            return;
        }
    }

    FURI_LOG_E(TAG, "=== ALL DECRYPTION ATTEMPTS FAILED ===");
    instance->decrypted = 0x00;
}

static bool psa_load_decrypted_from_flipper_format(
    SubGhzProtocolDecoderPSA* instance,
    FlipperFormat* flipper_format) {
    uint32_t serial = 0;
    uint32_t btn = 0;
    uint32_t cnt = 0;
    uint32_t type = 0;
    flipper_format_rewind(flipper_format);
    psa_encoder_read_fields(flipper_format, &serial, &btn, &cnt, &type);
    if(serial == 0 && btn == 0 && cnt == 0 && type == 0) {
        return false;
    }

    instance->decrypted_serial = serial;
    instance->decrypted_button = (uint8_t)btn;
    instance->decrypted_counter = cnt;
    instance->decrypted_type = (uint8_t)type;
    instance->decrypted = 0x50;

    flipper_format_rewind(flipper_format);
    uint32_t crc = 0;
    if(flipper_format_read_uint32(flipper_format, "CRC", &crc, 1)) {
        instance->decrypted_crc = (uint8_t)crc;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Seed", &instance->decrypted_seed, 1)) {
    }
    return true;
}

void* subghz_protocol_decoder_psa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPSA* instance = malloc(sizeof(SubGhzProtocolDecoderPSA));
    if(instance) {
        memset(instance, 0, sizeof(SubGhzProtocolDecoderPSA));
        instance->base.protocol = &subghz_protocol_psa;
        instance->manchester_state = ManchesterStateMid1;
    }
    return instance;
}

void subghz_protocol_decoder_psa_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_psa_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;
    instance->state = 0;
    instance->status_flag = 0;
    instance->mode_serialize = 0;
    instance->key1_low = 0;
    instance->key1_high = 0;
    instance->key2_low = 0;
    instance->key2_high = 0;
    instance->decode_data_low = 0;
    instance->decode_data_high = 0;
    instance->decode_count_bit = 0;
    instance->pattern_counter = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->decrypted_button = 0;
    instance->decrypted_serial = 0;
    instance->decrypted_counter = 0;
    instance->decrypted_crc = 0;
    instance->decrypted_seed = 0;
    instance->decrypted_type = 0;
    instance->decrypted = 0;
    instance->validation_field = 0;
    instance->prev_duration = 0;
}

void subghz_protocol_decoder_psa_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    uint32_t tolerance;
    uint32_t new_state = instance->state;
    uint32_t prev_dur = instance->prev_duration;
    uint32_t te_short = subghz_protocol_psa_const.te_short;
    uint32_t te_long = subghz_protocol_psa_const.te_long;

    switch(instance->state) {
    case PSADecoderState0:
        if(!level) {
            return;
        }

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance > PSA_TOLERANCE_99) {
                if(!psa_is_te_125_short(duration)) {
                    return;
                }
                new_state = PSADecoderState3;
            } else {
                new_state = PSADecoderState1;
            }
        } else {
            tolerance = duration - te_short;
            if(tolerance > PSA_TOLERANCE_99) {
                return;
            }
            new_state = PSADecoderState1;
        }

        instance->decode_data_low = 0;
        instance->decode_data_high = 0;
        instance->pattern_counter = 0;
        instance->decode_count_bit = 0;
        instance->mode_serialize = 0;
        instance->prev_duration = duration;
        instance->decrypted_type = 0;
        instance->decrypted_button = 0;
        instance->decrypted_serial = 0;
        instance->decrypted_counter = 0;
        instance->decrypted_crc = 0;
        instance->decrypted_seed = 0;
        instance->decrypted = 0x00;
        manchester_advance(
            instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        break;

    case PSADecoderState1:
        if(level) {
            return;
        }

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance < PSA_TOLERANCE_100) {
                uint32_t prev_diff = psa_abs_diff(prev_dur, te_short);
                if(prev_diff <= PSA_TOLERANCE_99) {
                    instance->pattern_counter++;
                }
                instance->prev_duration = duration;
                return;
            }
        } else {
            tolerance = duration - te_short;
            if(tolerance < PSA_TOLERANCE_100) {
                uint32_t prev_diff = psa_abs_diff(prev_dur, te_short);
                if(prev_diff <= PSA_TOLERANCE_99) {
                    instance->pattern_counter++;
                }
                instance->prev_duration = duration;
                return;
            } else {
                uint32_t long_diff;
                if(duration < te_long) {
                    long_diff = te_long - duration;
                } else {
                    long_diff = duration - te_long;
                }
                if(long_diff < 100) {
                    if(instance->pattern_counter >= PSA_PATTERN_THRESHOLD_1) {
                        new_state = PSADecoderState2;
                        instance->decode_data_low = 0;
                        instance->decode_data_high = 0;
                        instance->decode_count_bit = 0;
                        manchester_advance(
                            instance->manchester_state,
                            ManchesterEventReset,
                            &instance->manchester_state,
                            NULL);
                        instance->state = new_state;
                    }
                    instance->pattern_counter = 0;
                    instance->prev_duration = duration;
                    return;
                }
            }
        }

        new_state = PSADecoderState0;
        instance->pattern_counter = 0;
        break;

    case PSADecoderState2:
        if(instance->decode_count_bit >= PSA_MAX_BITS) {
            new_state = PSADecoderState0;
            break;
        }

        if(level && instance->decode_count_bit == PSA_KEY2_BITS) {
            if(duration >= 800) {
                uint32_t end_diff;
                if(duration < PSA_TE_END_1000) {
                    end_diff = PSA_TE_END_1000 - duration;
                } else {
                    end_diff = duration - PSA_TE_END_1000;
                }
                if(end_diff <= 199) {
                    if(psa_try_complete_at_key2(instance, 1)) {
                        return;
                    }
                    return;
                }
            }
        }

        uint8_t manchester_input = 0;
        bool should_process = false;

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance >= PSA_TOLERANCE_100) {
                return;
            }
            manchester_input = ((level ^ 1) & 0x7f) << 1;
            should_process = true;
        } else {
            tolerance = duration - te_short;
            if(tolerance < PSA_TOLERANCE_100) {
                manchester_input = ((level ^ 1) & 0x7f) << 1;
                should_process = true;
            } else if(duration < te_long) {
                uint32_t diff_from_250 = duration - te_short;
                uint32_t diff_from_500 = te_long - duration;

                if(diff_from_500 < 150 || diff_from_250 > diff_from_500) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                    should_process = true;
                } else if(diff_from_250 < 150) {
                    manchester_input = ((level ^ 1) & 0x7f) << 1;
                    should_process = true;
                } else {
                    if(duration > 10000) {
                        new_state = PSADecoderState0;
                        instance->pattern_counter = 0;
                        return;
                    }
                    if(duration >= 350 && duration <= 400) {
                        if(level == 0) {
                            manchester_input = 6;
                        } else {
                            manchester_input = 4;
                        }
                        should_process = true;
                    } else {
                        return;
                    }
                }
            } else {
                uint32_t long_diff = duration - te_long;
                if(long_diff < 100) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                    should_process = true;
                } else {
                    if(!level) {
                        if(duration > 10000) {
                            new_state = PSADecoderState0;
                            instance->pattern_counter = 0;
                            return;
                        }
                        return;
                    }
                    should_process = false;
                }
            }
        }

        if(should_process && instance->decode_count_bit < PSA_KEY2_BITS) {
            bool decoded_bit = false;

            if(manchester_advance(
                   instance->manchester_state,
                   (ManchesterEvent)manchester_input,
                   &instance->manchester_state,
                   &decoded_bit)) {
                uint32_t carry = (instance->decode_data_low >> 31) & 1;
                instance->decode_data_low = (instance->decode_data_low << 1) |
                                            (decoded_bit ? 1 : 0);
                instance->decode_data_high = (instance->decode_data_high << 1) | carry;
                instance->decode_count_bit++;

                if(instance->decode_count_bit == PSA_KEY1_BITS) {
                    instance->key1_low = instance->decode_data_low;
                    instance->key1_high = instance->decode_data_high;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                } else if(instance->decode_count_bit == PSA_KEY2_BITS) {
                    if(psa_try_complete_at_key2(instance, 1)) {
                        return;
                    }
                }
            }
        }

        if(!level) {
            return;
        }

        if(!should_process) {
            uint32_t end_diff;
            if(duration < PSA_TE_END_1000) {
                end_diff = PSA_TE_END_1000 - duration;
            } else {
                end_diff = duration - PSA_TE_END_1000;
            }
            if(end_diff <= 199) {
                if(instance->decode_count_bit != PSA_KEY2_BITS) {
                    return;
                }

                if(psa_try_complete_at_key2(instance, 1)) {
                    return;
                }
            } else {
                return;
            }
        }
        break;

    case PSADecoderState3:
        if(psa_is_te_125_long(duration)) {
            if(instance->pattern_counter >= PSA_PATTERN_THRESHOLD_2) {
                new_state = PSADecoderState4;
                instance->decode_data_low = 0;
                instance->decode_data_high = 0;
                instance->decode_count_bit = 0;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                instance->state = new_state;
                instance->pattern_counter = 0;
                instance->prev_duration = duration;
                return;
            }
            new_state = PSADecoderState0;
            instance->pattern_counter = 0;
            break;
        }

        if(psa_is_te_125_short(duration)) {
            uint32_t prev_diff = psa_abs_diff(prev_dur, PSA_TE_SHORT_125);
            if(prev_diff <= PSA_TOLERANCE_49) {
                instance->pattern_counter++;
            } else {
                instance->pattern_counter = 0;
            }
            instance->prev_duration = duration;
            return;
        }

        new_state = PSADecoderState0;
        instance->pattern_counter = 0;
        break;

    case PSADecoderState4: {
        if(instance->decode_count_bit >= PSA_MAX_BITS) {
            new_state = PSADecoderState0;
            break;
        }

        uint8_t manchester_input = 0;
        bool should_process_125 = false;

        if(psa_is_te_125_short(duration)) {
            manchester_input = ((level ^ 1) & 0x7f) << 1;
            should_process_125 = true;
        } else if(psa_is_te_125_long(duration)) {
            manchester_input = level ? 4 : 6;
            should_process_125 = true;
        }

        if(should_process_125 && instance->decode_count_bit < PSA_KEY2_BITS) {
            bool decoded_bit = false;

            if(manchester_advance(
                   instance->manchester_state,
                   (ManchesterEvent)manchester_input,
                   &instance->manchester_state,
                   &decoded_bit)) {
                uint32_t carry = (instance->decode_data_low >> 31) & 1;
                instance->decode_data_low = (instance->decode_data_low << 1) |
                                            (decoded_bit ? 1 : 0);
                instance->decode_data_high = (instance->decode_data_high << 1) | carry;
                instance->decode_count_bit++;

                if(instance->decode_count_bit == PSA_KEY1_BITS) {
                    instance->key1_low = instance->decode_data_low;
                    instance->key1_high = instance->decode_data_high;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                } else if(instance->decode_count_bit == PSA_KEY2_BITS) {
                    if(psa_try_complete_at_key2(instance, 2)) {
                        return;
                    }
                }
            }
        }

        if(!level) {
            return;
        }

        if(!should_process_125) {
            uint32_t end_diff;
            if(duration < PSA_TE_END_500) {
                end_diff = PSA_TE_END_500 - duration;
            } else {
                end_diff = duration - PSA_TE_END_500;
            }
            if(end_diff <= 99) {
                if(instance->decode_count_bit != PSA_KEY2_BITS) {
                    return;
                }

                if(psa_try_complete_at_key2(instance, 2)) {
                    return;
                }
            } else {
                return;
            }
        }
        break;
    }
    }

    instance->state = new_state;
    instance->prev_duration = duration;
}

uint8_t subghz_protocol_decoder_psa_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;
    const uint64_t combined_data = (((uint64_t)instance->key1_high << 32) | instance->key1_low) ^
                                   (((uint64_t)instance->key2_high << 32) | instance->key2_low);
    SubGhzBlockDecoder decoder = {.decode_data = combined_data, .decode_count_bit = 64};
    return subghz_protocol_blocks_get_hash_data(&decoder, 16);
}

SubGhzProtocolStatus subghz_protocol_decoder_psa_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        if(!subghz_block_generic_serialize_preset(flipper_format, preset)) break;

        if(!flipper_format_write_string_cstr(
               flipper_format, "Protocol", instance->base.protocol->name))
            break;

        uint32_t bits = 128;
        if(!flipper_format_write_uint32(flipper_format, "Bit", &bits, 1)) break;

        char key1_str[20];
        uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
        snprintf(key1_str, sizeof(key1_str), "%016llX", key1);
        if(!flipper_format_write_string_cstr(flipper_format, "Key", key1_str)) break;

        char key2_str[20];
        uint64_t key2 = ((uint64_t)instance->key2_high << 32) | instance->key2_low;
        snprintf(key2_str, sizeof(key2_str), "%016llX", key2);
        if(!flipper_format_write_string_cstr(flipper_format, "Key_2", key2_str)) break;

        uint32_t val_field = instance->validation_field;
        flipper_format_write_uint32(flipper_format, "ValidationField", &val_field, 1);

        if(instance->decrypted == 0x50 && instance->decrypted_type != 0) {
            flipper_format_insert_or_update_uint32(
                flipper_format, "Serial", &instance->decrypted_serial, 1);
            uint32_t btn_w = instance->decrypted_button;
            flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
            uint32_t cnt_w = instance->decrypted_counter;
            flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
            uint32_t type_w = instance->decrypted_type;
            flipper_format_insert_or_update_uint32(flipper_format, "Type", &type_w, 1);

            uint32_t crc_temp = instance->decrypted_crc;
            flipper_format_insert_or_update_uint32(flipper_format, "CRC", &crc_temp, 1);
            flipper_format_insert_or_update_uint32(
                flipper_format, "Seed", &instance->decrypted_seed, 1);
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_psa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        uint64_t key1 = 0;
        if(!subghz_block_generic_read_key_u64(flipper_format, &key1)) break;
        instance->key1_low = (uint32_t)(key1 & 0xFFFFFFFF);
        instance->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFF);

        uint64_t key2 = 0;
        flipper_format_rewind(flipper_format);
        if(!psa_read_key2_u64(flipper_format, &key2)) {
            break;
        }
        instance->key2_low = (uint32_t)(key2 & 0xFFFFFFFF);
        instance->key2_high = (uint32_t)((key2 >> 32) & 0xFFFFFFFF);

        uint32_t validation = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "ValidationField", &validation, 1)) {
            instance->validation_field = (uint16_t)(validation & 0xFFFFU);
        }

        instance->status_flag = 0x80;
        instance->mode_serialize = 0;

        if(!psa_load_decrypted_from_flipper_format(instance, flipper_format)) {
            psa_decrypt_router(instance, false);
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

void subghz_protocol_decoder_psa_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderPSA* instance = context;
    psa_get_string_setup_editor(instance->decrypted_button, instance->decrypted_counter, 4, 16, 4);

    const uint16_t key2_value = (uint16_t)(instance->key2_low & 0xFFFF);

    if(instance->decrypted == 0x50 && instance->decrypted_type != 0) {
        furi_string_cat_printf(output, "%s %ubit\r\n", instance->base.protocol->name, 128);
        furi_string_cat_printf(
            output,
            "Key1:%08lX%08lX\r\nKey2:%04X\r\n",
            (unsigned long)instance->key1_high,
            (unsigned long)instance->key1_low,
            (unsigned)key2_value);
        if(instance->decrypted_type == 0x23) {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\n",
                (int)6,
                (unsigned long)instance->decrypted_serial,
                instance->decrypted_button);
            furi_string_cat_printf(
                output,
                "Cnt:%04lX CRC:%02X [OK]\r\nType:%02X Sd:%06lX",
                (uint32_t)instance->decrypted_counter,
                instance->decrypted_crc,
                instance->decrypted_type,
                instance->decrypted_seed);
        } else if(instance->decrypted_type == 0x36) {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\n",
                (int)6,
                (unsigned long)instance->decrypted_serial,
                instance->decrypted_button);
            furi_string_cat_printf(
                output,
                "Cnt:%lX CRC:%02X [OK]\r\nType:%02X Sd:%06lX",
                instance->decrypted_counter,
                instance->decrypted_crc,
                instance->decrypted_type,
                instance->decrypted_seed);
        } else {
            furi_string_cat_printf(output, "Key2:%04X", (unsigned)key2_value);
        }
    } else {
        furi_string_cat_printf(output, "%s %ubit\r\n", instance->base.protocol->name, 128);
        furi_string_cat_printf(
            output,
            "Key1:%08lX%08lX\r\nKey2:%04X\r\n",
            (unsigned long)instance->key1_high,
            (unsigned long)instance->key1_low,
            (unsigned)key2_value);
    }
}
