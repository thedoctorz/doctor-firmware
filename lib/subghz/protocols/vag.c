#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/registry.h>
#include "vag.h"
#include "aut64.h"
#include <string.h>
#include <lib/subghz/subghz_keystore.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t vag_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void vag_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t vag_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void vag_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static SubGhzProtocolStatus
    vag_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void vag_encoder_read_fields(
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

static uint32_t vag_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    vag_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static SubGhzProtocolStatus vag_encoder_read_bit(
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

static void vag_get_string_setup_editor(
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

#define TAG "VAGProtocol"

#define VAG_ENCODER_UPLOAD_TYPE12_SIZE 635
#define VAG_ENCODER_UPLOAD_TYPE34_SIZE 518
#define VAG_ENCODER_UPLOAD_MAX_SIZE    VAG_ENCODER_UPLOAD_TYPE12_SIZE
#define VAG_UPLOAD_CAPACITY            2048U
static inline size_t
    vag_emit_manchester_inv(LevelDuration* up, size_t i, size_t cap, bool bit_value, uint32_t te) {
    if(i < cap) up[i++] = level_duration_make(bit_value, te);
    if(i < cap) up[i++] = level_duration_make(!bit_value, te);
    return i;
}

static const SubGhzBlockConst subghz_protocol_vag_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 80,
    .min_count_bit_for_found = 80,
};

#define VAG_T12_TE_SHORT     300u
#define VAG_T12_TE_LONG      600u
#define VAG_T12_TE_DELTA     100u
#define VAG_T12_GAP_DELTA    200u
#define VAG_T12_PREAMBLE_MIN 151u

#define VAG_T34_TE_SHORT     500u
#define VAG_T34_TE_LONG      1000u
#define VAG_T34_TE_DELTA     100u
#define VAG_T34_LONG_DELTA   200u
#define VAG_T34_SYNC         750u
#define VAG_T34_SYNC_DELTA   150u
#define VAG_T34_PREAMBLE_MIN 31u
#define VAG_T34_SYNC_PAIRS   3u

#define VAG_DATA_GAP_MIN    4001u
#define VAG_TOTAL_BITS      80u
#define VAG_KEY1_BITS       64u
#define VAG_PREFIX_BITS     15u
#define VAG_BIT_LIMIT       96u
#define VAG_FRAME_PREFIX_T1 0x2F3Fu
#define VAG_FRAME_PREFIX_T2 0x2F1Cu

#define VAG_KEYS_COUNT 3

static int8_t protocol_vag_keys_loaded = -1;
static struct aut64_key protocol_vag_keys[VAG_KEYS_COUNT];

static void protocol_vag_load_keys(const char* file_name) {
    if(protocol_vag_keys_loaded == VAG_KEYS_COUNT) {
        return;
    }
    protocol_vag_keys_loaded = 0;

    for(uint8_t i = 0; i < VAG_KEYS_COUNT; i++) {
        uint8_t key_packed[AUT64_PACKED_KEY_SIZE];

        if(subghz_keystore_raw_get_data(
               file_name, i * AUT64_PACKED_KEY_SIZE, key_packed, AUT64_PACKED_KEY_SIZE)) {
            int rc = aut64_unpack(&protocol_vag_keys[i], key_packed);
#ifdef AUT64_ENABLE_VALIDATIONS
            if(rc == AUT64_ERR_INVALID_PACKED) {
                FURI_LOG_E(TAG, "Invalid key: %u", i);
            } else if(rc == AUT64_ERR_NULL_POINTER) {
                FURI_LOG_E(TAG, "Key is NULL: %d", i);
            }
            if(rc == AUT64_OK) {
                protocol_vag_keys_loaded++;
            } else {
                break;
            }
#else
            (void)rc;
            protocol_vag_keys_loaded++;
#endif
        } else {
            FURI_LOG_E(TAG, "Unable to load key %u from %s", i, file_name);
            break;
        }
    }

    if(protocol_vag_keys_loaded > 0) {
        return;
    }
    FURI_LOG_E(TAG, "VAG keys not loaded - check %s on SD card", file_name);
}

static struct aut64_key* protocol_vag_get_key(uint8_t index) {
    for(uint8_t i = 0; i < MIN(protocol_vag_keys_loaded, VAG_KEYS_COUNT); i++) {
        if(protocol_vag_keys[i].index == index) {
            return &protocol_vag_keys[i];
        }
    }

    return NULL;
}

#define VAG_TEA_DELTA  0x9E3779B9U
#define VAG_TEA_ROUNDS 32

static const uint32_t vag_tea_key_schedule[] = {0x0B46502D, 0x5E253718, 0x2BF93A19, 0x622C1206};

static const char* vag_button_name(uint8_t btn) {
    switch(btn) {
    case 0x1:
        return "Unlock";
    case 0x2:
        return "Lock";
    case 0x4:
        return "Boot";
    case 0x10:
        return "Unlock";
    case 0x20:
        return "Lock";
    case 0x40:
        return "Boot";
    default:
        return "Unkn";
    }
}

typedef struct SubGhzProtocolDecoderVAG {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t data_low;
    uint32_t data_high;
    uint8_t bit_count;
    uint32_t key1_low;
    uint32_t key1_high;
    uint32_t key2_low;
    uint32_t key2_high;
    uint16_t data_count_bit;
    uint8_t vag_type;
    uint16_t header_count;
    uint8_t mid_count;
    ManchesterState manchester_state;

    uint32_t serial;
    uint32_t cnt;
    uint8_t btn;
    uint8_t check_byte;
    uint8_t key_idx;
    bool decrypted;
} SubGhzProtocolDecoderVAG;

typedef enum {
    VAGDecoderStepReset = 0,
    VAGDecoderStepPreamble1 = 1,
    VAGDecoderStepData1 = 2,
    VAGDecoderStepPreamble2 = 3,
    VAGDecoderStepSync2A = 4,
    VAGDecoderStepSync2B = 5,
    VAGDecoderStepSync2C = 6,
    VAGDecoderStepData2 = 7,
} VAGDecoderStep;

static void vag_tea_decrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key_schedule) {
    uint32_t sum = VAG_TEA_DELTA * VAG_TEA_ROUNDS;
    for(int i = 0; i < VAG_TEA_ROUNDS; i++) {
        *v1 -= (((*v0 << 4) ^ (*v0 >> 5)) + *v0) ^ (sum + key_schedule[(sum >> 11) & 3]);
        sum -= VAG_TEA_DELTA;
        *v0 -= (((*v1 << 4) ^ (*v1 >> 5)) + *v1) ^ (sum + key_schedule[sum & 3]);
    }
}
static void vag_tea_encrypt(uint32_t* v0, uint32_t* v1, const uint32_t* key_schedule) {
    uint32_t sum = 0;
    for(int i = 0; i < VAG_TEA_ROUNDS; i++) {
        *v0 += (((*v1 << 4) ^ (*v1 >> 5)) + *v1) ^ (sum + key_schedule[sum & 3]);
        sum += VAG_TEA_DELTA;
        *v1 += (((*v0 << 4) ^ (*v0 >> 5)) + *v0) ^ (sum + key_schedule[(sum >> 11) & 3]);
    }
}

static bool vag_dispatch_type_1_2(uint8_t dispatch) {
    return (dispatch == 0x2A || dispatch == 0x1C || dispatch == 0x46);
}

static bool vag_dispatch_type_3_4(uint8_t dispatch) {
    return (dispatch == 0x2B || dispatch == 0x1D || dispatch == 0x47);
}

static bool vag_button_valid(const uint8_t* dec) {
    uint8_t dec_byte = dec[7];
    uint8_t dec_btn = (dec_byte >> 4) & 0xF;

    if(dec_btn == 1 || dec_btn == 2 || dec_btn == 4) {
        return true;
    }
    if(dec_byte == 0) {
        return true;
    }
    return false;
}

static bool vag_button_matches(const uint8_t* dec, uint8_t dispatch_byte) {
    uint8_t expected_btn = (dispatch_byte >> 4) & 0xF;
    uint8_t dec_btn = (dec[7] >> 4) & 0xF;

    if(dec_btn == expected_btn) {
        return true;
    }
    if(dec[7] == 0 && expected_btn == 2) {
        return true;
    }
    return false;
}

static void vag_fill_from_decrypted(
    SubGhzProtocolDecoderVAG* instance,
    const uint8_t* dec,
    uint8_t dispatch_byte) {
    uint32_t serial_raw = (uint32_t)dec[0] | ((uint32_t)dec[1] << 8) | ((uint32_t)dec[2] << 16) |
                          ((uint32_t)dec[3] << 24);
    instance->serial = (serial_raw << 24) | ((serial_raw & 0xFF00) << 8) |
                       ((serial_raw >> 8) & 0xFF00) | (serial_raw >> 24);

    instance->cnt = (uint32_t)dec[4] | ((uint32_t)dec[5] << 8) | ((uint32_t)dec[6] << 16);

    instance->btn = (dec[7] >> 4) & 0xF;
    instance->check_byte = dispatch_byte;
    instance->decrypted = true;
}

static bool vag_aut64_decrypt(uint8_t* block, int key_index) {
    struct aut64_key* key = protocol_vag_get_key(key_index + 1);
    if(!key) {
        FURI_LOG_E(TAG, "Key not found: %d", key_index + 1);
        return false;
    }
    int rc = aut64_decrypt(key, block);
#ifdef AUT64_ENABLE_VALIDATIONS
    if(rc == AUT64_ERR_INVALID_KEY) {
        FURI_LOG_E(TAG, "Invalid key: %d", key_index + 1);
    } else if(rc == AUT64_ERR_NULL_POINTER) {
        FURI_LOG_E(TAG, "key is NULL: %d", key_index + 1);
    }
#endif

    return (rc == AUT64_OK) ? true : false;
}

static bool vag_try_parse_type(
    SubGhzProtocolDecoderVAG* instance,
    uint8_t try_type,
    uint32_t key1_low,
    uint32_t key1_high,
    uint32_t key2_low) {
    const uint8_t dispatch_byte = (uint8_t)(key2_low & 0xFF);
    const uint8_t key2_high = (uint8_t)((key2_low >> 8) & 0xFF);

    uint8_t key1_bytes[8];
    key1_bytes[0] = (uint8_t)(key1_high >> 24);
    key1_bytes[1] = (uint8_t)(key1_high >> 16);
    key1_bytes[2] = (uint8_t)(key1_high >> 8);
    key1_bytes[3] = (uint8_t)(key1_high);
    key1_bytes[4] = (uint8_t)(key1_low >> 24);
    key1_bytes[5] = (uint8_t)(key1_low >> 16);
    key1_bytes[6] = (uint8_t)(key1_low >> 8);
    key1_bytes[7] = (uint8_t)(key1_low);

    uint8_t block[8];
    block[0] = key1_bytes[1];
    block[1] = key1_bytes[2];
    block[2] = key1_bytes[3];
    block[3] = key1_bytes[4];
    block[4] = key1_bytes[5];
    block[5] = key1_bytes[6];
    block[6] = key1_bytes[7];
    block[7] = key2_high;

    switch(try_type) {
    case 1:
        if(!vag_dispatch_type_1_2(dispatch_byte)) {
            return false;
        }
        {
            uint8_t block_copy[8];

            for(int key_idx = 0; key_idx < 3; key_idx++) {
                memcpy(block_copy, block, 8);
                if(!vag_aut64_decrypt(block_copy, key_idx)) {
                    continue;
                }

                if(vag_button_valid(block_copy)) {
                    instance->serial = ((uint32_t)block_copy[0] << 24) |
                                       ((uint32_t)block_copy[1] << 16) |
                                       ((uint32_t)block_copy[2] << 8) | (uint32_t)block_copy[3];
                    instance->cnt = (uint32_t)block_copy[4] | ((uint32_t)block_copy[5] << 8) |
                                    ((uint32_t)block_copy[6] << 16);
                    instance->btn = block_copy[7];
                    instance->check_byte = dispatch_byte;
                    instance->key_idx = key_idx;
                    instance->vag_type = 1;
                    instance->decrypted = true;
                    return true;
                }
            }
        }
        return false;

    case 2:
        if(!vag_dispatch_type_1_2(dispatch_byte)) {
            return false;
        }
        {
            uint32_t v0_orig = ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) |
                               ((uint32_t)block[2] << 8) | (uint32_t)block[3];
            uint32_t v1_orig = ((uint32_t)block[4] << 24) | ((uint32_t)block[5] << 16) |
                               ((uint32_t)block[6] << 8) | (uint32_t)block[7];
            uint32_t v0 = v0_orig;
            uint32_t v1 = v1_orig;

            vag_tea_decrypt(&v0, &v1, vag_tea_key_schedule);

            uint8_t tea_dec[8];
            tea_dec[0] = (uint8_t)(v0 >> 24);
            tea_dec[1] = (uint8_t)(v0 >> 16);
            tea_dec[2] = (uint8_t)(v0 >> 8);
            tea_dec[3] = (uint8_t)(v0);
            tea_dec[4] = (uint8_t)(v1 >> 24);
            tea_dec[5] = (uint8_t)(v1 >> 16);
            tea_dec[6] = (uint8_t)(v1 >> 8);
            tea_dec[7] = (uint8_t)(v1);

            if(!vag_button_matches(tea_dec, dispatch_byte)) {
                return false;
            }

            vag_fill_from_decrypted(instance, tea_dec, dispatch_byte);
            instance->key_idx = 0xFF;
            instance->vag_type = 2;
            return true;
        }

    case 3: {
        uint8_t block_copy[8];

        memcpy(block_copy, block, 8);
        if(vag_aut64_decrypt(block_copy, 2) && vag_button_valid(block_copy)) {
            instance->vag_type = 4;
            instance->key_idx = 2;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return true;
        }

        memcpy(block_copy, block, 8);
        if(vag_aut64_decrypt(block_copy, 1) && vag_button_valid(block_copy)) {
            instance->key_idx = 1;
            instance->vag_type = 3;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return true;
        }

        memcpy(block_copy, block, 8);
        if(vag_aut64_decrypt(block_copy, 0) && vag_button_valid(block_copy)) {
            instance->key_idx = 0;
            instance->vag_type = 3;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return true;
        }
        return false;
    }

    case 4:
        if(!vag_dispatch_type_3_4(dispatch_byte)) {
            return false;
        }
        {
            uint8_t block_copy[8];
            memcpy(block_copy, block, 8);

            if(!vag_aut64_decrypt(block_copy, 2)) {
                return false;
            }
            if(!vag_button_matches(block_copy, dispatch_byte)) {
                return false;
            }
            instance->key_idx = 2;
            instance->vag_type = 4;
            vag_fill_from_decrypted(instance, block_copy, dispatch_byte);
            return true;
        }

    default:
        return false;
    }
}

static void vag_parse_data(SubGhzProtocolDecoderVAG* instance) {
    furi_check(instance);

    instance->decrypted = false;
    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;

    const uint8_t detected_type = instance->vag_type;
    const uint32_t raw_k1l = instance->key1_low;
    const uint32_t raw_k1h = instance->key1_high;
    const uint32_t raw_k2l = instance->key2_low;
    const uint32_t inv_k1l = ~raw_k1l;
    const uint32_t inv_k1h = ~raw_k1h;
    const uint32_t inv_k2l = (~raw_k2l) & 0xFFFF;

    typedef struct {
        uint8_t type;
        uint32_t k1l;
        uint32_t k1h;
        uint32_t k2l;
    } VagParseAttempt;

    VagParseAttempt attempts[6];
    size_t attempt_count = 0;

    attempts[attempt_count++] = (VagParseAttempt){detected_type, raw_k1l, raw_k1h, raw_k2l};

    if(detected_type == 3 || detected_type == 4) {
        attempts[attempt_count++] = (VagParseAttempt){1, inv_k1l, inv_k1h, inv_k2l};
        attempts[attempt_count++] = (VagParseAttempt){2, inv_k1l, inv_k1h, inv_k2l};
    } else if(detected_type == 1 || detected_type == 2) {
        attempts[attempt_count++] = (VagParseAttempt){3, inv_k1l, inv_k1h, inv_k2l};
        attempts[attempt_count++] = (VagParseAttempt){4, inv_k1l, inv_k1h, inv_k2l};
    } else {
        attempts[attempt_count++] = (VagParseAttempt){1, raw_k1l, raw_k1h, raw_k2l};
        attempts[attempt_count++] = (VagParseAttempt){1, inv_k1l, inv_k1h, inv_k2l};
        attempts[attempt_count++] = (VagParseAttempt){3, raw_k1l, raw_k1h, raw_k2l};
        attempts[attempt_count++] = (VagParseAttempt){3, inv_k1l, inv_k1h, inv_k2l};
    }

    for(size_t i = 0; i < attempt_count; i++) {
        instance->decrypted = false;
        instance->serial = 0;
        instance->cnt = 0;
        instance->btn = 0;

        if(vag_try_parse_type(
               instance, attempts[i].type, attempts[i].k1l, attempts[i].k1h, attempts[i].k2l)) {
            return;
        }
    }

    instance->decrypted = false;
    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;
    instance->check_byte = 0;
}

static bool vag_load_decrypted_from_flipper_format(
    SubGhzProtocolDecoderVAG* instance,
    FlipperFormat* flipper_format) {
    uint32_t serial = 0;
    uint32_t btn = 0;
    uint32_t cnt = 0;
    uint32_t type = 0;
    flipper_format_rewind(flipper_format);
    vag_encoder_read_fields(flipper_format, &serial, &btn, &cnt, &type);

    if(serial == 0 && instance->generic.serial != 0) {
        serial = instance->generic.serial;
    }
    if(btn == 0 && instance->generic.btn != 0) {
        btn = instance->generic.btn;
    }
    if(cnt == 0 && instance->generic.cnt != 0) {
        cnt = instance->generic.cnt;
    }
    if(type != 0 && instance->vag_type == 0) {
        instance->vag_type = (uint8_t)type;
    }

    uint32_t key_idx = 0xFF;
    flipper_format_rewind(flipper_format);
    flipper_format_read_uint32(flipper_format, "KeyIdx", &key_idx, 1);

    if(serial == 0 && btn == 0 && cnt == 0 && key_idx == 0xFF) {
        return false;
    }

    instance->serial = serial;
    instance->btn = (uint8_t)btn;
    instance->cnt = cnt;
    if(key_idx != 0xFF) {
        instance->key_idx = (uint8_t)key_idx;
    }
    instance->decrypted = true;
    return true;
}

const SubGhzProtocolDecoder subghz_protocol_vag_decoder = {
    .alloc = subghz_protocol_decoder_vag_alloc,
    .free = subghz_protocol_decoder_vag_free,
    .feed = subghz_protocol_decoder_vag_feed,
    .reset = subghz_protocol_decoder_vag_reset,
    .get_hash_data = subghz_protocol_decoder_vag_get_hash_data,
    .serialize = subghz_protocol_decoder_vag_serialize,
    .deserialize = subghz_protocol_decoder_vag_deserialize,
    .get_string = subghz_protocol_decoder_vag_get_string,
};
const SubGhzProtocolEncoder subghz_protocol_vag_encoder = {
    .alloc = subghz_protocol_encoder_vag_alloc,
    .free = subghz_protocol_encoder_vag_free,
    .deserialize = subghz_protocol_encoder_vag_deserialize,
    .stop = subghz_protocol_encoder_vag_stop,
    .yield = subghz_protocol_encoder_vag_yield,
};

const SubGhzProtocol subghz_protocol_vag = {
    .name = SUBGHZ_PROTOCOL_VAG_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_vag_decoder,
    .encoder = &subghz_protocol_vag_encoder,
};

void* subghz_protocol_decoder_vag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVAG* instance = calloc(1, sizeof(SubGhzProtocolDecoderVAG));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_vag;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->decrypted = false;
    instance->key_idx = 0xFF;

    protocol_vag_load_keys(SUBGHZ_VAG_KEYS_FILE);

    return instance;
}

void subghz_protocol_decoder_vag_free(void* context) {
    furi_check(context);
    free(context);
}
void subghz_protocol_decoder_vag_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVAG* instance = context;
    instance->decoder.parser_step = VAGDecoderStepReset;
    instance->decrypted = false;

    /* Reset decoder state to avoid stale parsing after external resets */
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->data_count_bit = 0;
    instance->vag_type = 0;
    instance->header_count = 0;
    instance->mid_count = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);

    instance->serial = 0;
    instance->cnt = 0;
    instance->btn = 0;
    instance->check_byte = 0;
    instance->key_idx = 0xFF;
}

void subghz_protocol_decoder_vag_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderVAG* instance = context;

    switch(instance->decoder.parser_step) {
    case VAGDecoderStepReset:
        if(!level) break;
        if(DURATION_DIFF(duration, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
            instance->decoder.parser_step = VAGDecoderStepPreamble1;
        } else if(DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA) {
            instance->decoder.parser_step = VAGDecoderStepPreamble2;
        } else {
            break;
        }
        instance->data_low = 0;
        instance->data_high = 0;
        instance->header_count = 0;
        instance->mid_count = 0;
        instance->bit_count = 0;
        instance->vag_type = 0;
        instance->decoder.te_last = duration;
        manchester_advance(
            instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        break;

    case VAGDecoderStepPreamble1:
        if(level) break;
        if(DURATION_DIFF(duration, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
            if(DURATION_DIFF(instance->decoder.te_last, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else {
                instance->decoder.parser_step = VAGDecoderStepReset;
            }
            break;
        }
        instance->decoder.parser_step = VAGDecoderStepReset;
        if(instance->header_count < VAG_T12_PREAMBLE_MIN) break;
        if(DURATION_DIFF(duration, VAG_T12_TE_LONG) >= VAG_T12_GAP_DELTA) break;
        if(DURATION_DIFF(instance->decoder.te_last, VAG_T12_TE_SHORT) >= VAG_T12_TE_DELTA) break;
        instance->decoder.parser_step = VAGDecoderStepData1;
        break;

    case VAGDecoderStepData1: {
        if(instance->bit_count >= VAG_BIT_LIMIT) {
            instance->decoder.parser_step = VAGDecoderStepReset;
            break;
        }

        bool bit_value = false;
        ManchesterEvent event = ManchesterEventReset;
        bool got_pulse = false;

        if(DURATION_DIFF(duration, VAG_T12_TE_SHORT) < VAG_T12_TE_DELTA) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            got_pulse = true;
        } else if(
            duration > VAG_T12_TE_SHORT + VAG_T12_TE_DELTA &&
            DURATION_DIFF(duration, VAG_T12_TE_LONG) < VAG_T12_GAP_DELTA) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            got_pulse = true;
        }

        if(got_pulse && instance->bit_count < VAG_BIT_LIMIT) {
            if(manchester_advance(
                   instance->manchester_state, event, &instance->manchester_state, &bit_value)) {
                uint32_t carry = (instance->data_low >> 31) & 1;
                instance->data_low = (instance->data_low << 1) | (bit_value ? 1 : 0);
                instance->data_high = (instance->data_high << 1) | carry;
                instance->bit_count++;

                if(instance->bit_count == VAG_PREFIX_BITS) {
                    if(instance->data_low == VAG_FRAME_PREFIX_T1 && instance->data_high == 0) {
                        instance->data_low = 0;
                        instance->data_high = 0;
                        instance->bit_count = 0;
                        instance->vag_type = 1;
                    } else if(instance->data_low == VAG_FRAME_PREFIX_T2 && instance->data_high == 0) {
                        instance->data_low = 0;
                        instance->data_high = 0;
                        instance->bit_count = 0;
                        instance->vag_type = 2;
                    }
                } else if(instance->bit_count == VAG_KEY1_BITS) {
                    instance->key1_low = ~instance->data_low;
                    instance->key1_high = ~instance->data_high;
                    instance->data_low = 0;
                    instance->data_high = 0;
                }
            }
            break;
        }

        if(level) break;
        if(duration < VAG_DATA_GAP_MIN) break;
        if(instance->bit_count == VAG_TOTAL_BITS) {
            instance->key2_low = (~instance->data_low) & 0xFFFF;
            instance->key2_high = 0;
            instance->data_count_bit = VAG_TOTAL_BITS;
            vag_parse_data(instance);
            if(instance->decrypted && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        }
        instance->data_low = 0;
        instance->data_high = 0;
        instance->bit_count = 0;
        instance->decoder.parser_step = VAGDecoderStepReset;
        break;
    }

    case VAGDecoderStepPreamble2:
        if(!level) {
            if(DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA &&
               DURATION_DIFF(instance->decoder.te_last, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else {
                instance->decoder.parser_step = VAGDecoderStepReset;
            }
            break;
        }
        if(instance->header_count < VAG_T34_PREAMBLE_MIN) break;
        if(DURATION_DIFF(duration, VAG_T34_TE_LONG) >= VAG_T34_LONG_DELTA) break;
        if(DURATION_DIFF(instance->decoder.te_last, VAG_T34_TE_SHORT) >= VAG_T34_TE_DELTA) break;
        instance->decoder.te_last = duration;
        instance->decoder.parser_step = VAGDecoderStepSync2A;
        break;

    case VAGDecoderStepSync2A:
        if(!level && DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA &&
           DURATION_DIFF(instance->decoder.te_last, VAG_T34_TE_LONG) < VAG_T34_LONG_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGDecoderStepSync2B;
        } else {
            instance->decoder.parser_step = VAGDecoderStepReset;
        }
        break;

    case VAGDecoderStepSync2B:
        if(level && DURATION_DIFF(duration, VAG_T34_SYNC) < VAG_T34_SYNC_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGDecoderStepSync2C;
        } else {
            instance->decoder.parser_step = VAGDecoderStepReset;
        }
        break;

    case VAGDecoderStepSync2C:
        if(!level && DURATION_DIFF(duration, VAG_T34_SYNC) < VAG_T34_SYNC_DELTA &&
           DURATION_DIFF(instance->decoder.te_last, VAG_T34_SYNC) < VAG_T34_SYNC_DELTA) {
            instance->mid_count++;
            instance->decoder.parser_step = VAGDecoderStepSync2B;
            if(instance->mid_count == VAG_T34_SYNC_PAIRS) {
                instance->data_low = 1;
                instance->data_high = 0;
                instance->bit_count = 1;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                instance->decoder.parser_step = VAGDecoderStepData2;
            }
        } else {
            instance->decoder.parser_step = VAGDecoderStepReset;
        }
        break;

    case VAGDecoderStepData2: {
        bool bit_value = false;
        ManchesterEvent event = ManchesterEventReset;
        bool got_pulse = false;

        if(DURATION_DIFF(duration, VAG_T34_TE_SHORT) < VAG_T34_TE_DELTA) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            got_pulse = true;
        } else if(DURATION_DIFF(duration, VAG_T34_TE_LONG) < VAG_T34_LONG_DELTA) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            got_pulse = true;
        }

        if(got_pulse) {
            if(manchester_advance(
                   instance->manchester_state, event, &instance->manchester_state, &bit_value)) {
                uint32_t carry = (instance->data_low >> 31) & 1;
                instance->data_low = (instance->data_low << 1) | (bit_value ? 1 : 0);
                instance->data_high = (instance->data_high << 1) | carry;
                instance->bit_count++;

                if(instance->bit_count == VAG_KEY1_BITS) {
                    instance->key1_low = instance->data_low;
                    instance->key1_high = instance->data_high;
                    instance->data_low = 0;
                    instance->data_high = 0;
                }
            }
        }

        if(instance->bit_count != VAG_TOTAL_BITS) break;
        instance->key2_low = instance->data_low & 0xFFFF;
        instance->key2_high = 0;
        instance->data_count_bit = VAG_TOTAL_BITS;
        instance->vag_type = 3;
        vag_parse_data(instance);
        if(instance->decrypted && instance->base.callback) {
            instance->base.callback(&instance->base, instance->base.context);
        }
        instance->data_low = 0;
        instance->data_high = 0;
        instance->bit_count = 0;
        instance->decoder.parser_step = VAGDecoderStepReset;
        break;
    }

    default:
        instance->decoder.parser_step = VAGDecoderStepReset;
        break;
    }
}

uint8_t subghz_protocol_decoder_vag_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVAG* instance = context;
    const uint64_t combined_data = (((uint64_t)instance->key1_high << 32) | instance->key1_low) ^
                                   (uint64_t)(instance->key2_low & 0xFFFF);
    SubGhzBlockDecoder decoder = {.decode_data = combined_data, .decode_count_bit = 64};
    return subghz_protocol_blocks_get_hash_data(&decoder, 16);
}

SubGhzProtocolStatus subghz_protocol_decoder_vag_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderVAG* instance = context;

    if(!instance->decrypted && instance->data_count_bit >= 80) {
        vag_parse_data(instance);
    }
    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint16_t key2_16bit = (uint16_t)(instance->key2_low & 0xFFFF);
    instance->generic.data = key1;
    instance->generic.data_count_bit = instance->data_count_bit;
    instance->generic.type = instance->vag_type;
    instance->generic.key_idx = instance->key_idx;
    instance->generic.serial = instance->serial;
    instance->generic.cnt = instance->cnt;
    instance->generic.btn = instance->btn;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint8_t key2_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        key2_bytes[6] = (uint8_t)((key2_16bit >> 8) & 0xFF);
        key2_bytes[7] = (uint8_t)(key2_16bit & 0xFF);
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(flipper_format, "Key2", key2_bytes, 8);

        uint32_t type_tmp = instance->vag_type;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Type", &type_tmp, 1);

        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &instance->serial, 1);
        uint32_t btn_w = instance->btn;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->cnt;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        uint32_t key_idx_temp = instance->key_idx;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "KeyIdx", &key_idx_temp, 1);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_vag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderVAG* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_vag_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        uint64_t key1 = instance->generic.data;
        instance->key1_low = (uint32_t)key1;
        instance->key1_high = (uint32_t)(key1 >> 32);

        uint8_t key2_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, "Key2", key2_bytes, 8)) {
            uint16_t key2_16bit = ((uint16_t)key2_bytes[6] << 8) | (uint16_t)key2_bytes[7];
            instance->key2_low = (uint32_t)key2_16bit & 0xFFFF;
            instance->key2_high = 0;
        }

        uint32_t type = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Type", &type, 1)) {
            instance->vag_type = (uint8_t)type;
        }

        instance->data_count_bit = instance->generic.data_count_bit;

        instance->decrypted = false;
        if(!vag_load_decrypted_from_flipper_format(instance, flipper_format)) {
            vag_parse_data(instance);
        }
    }

    return ret;
}

static uint8_t vag_get_btn_code(uint8_t original) {
    if(original == 0x10 || original == 0x20 || original == 0x40) {
        switch(subghz_custom_btn_get()) {
        case SUBGHZ_CUSTOM_BTN_UP:
            return 0x20;
        case SUBGHZ_CUSTOM_BTN_OK:
            return 0x10;
        case SUBGHZ_CUSTOM_BTN_DOWN:
            return 0x40;
        default:
            return original;
        }
    }
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x8;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x3;
    default:
        return original;
    }
}

typedef struct SubGhzProtocolEncoderVAG {
    SubGhzProtocolEncoderBase base;
    SubGhzBlockGeneric generic;

    uint32_t key1_low;
    uint32_t key1_high;
    uint32_t key2_low;
    uint32_t key2_high;
    uint32_t serial;
    uint32_t cnt;
    uint8_t vag_type;
    uint8_t btn;
    uint8_t dispatch_byte;
    uint8_t key_idx;

    size_t repeat;
    size_t front;
    size_t size_upload;
    LevelDuration* upload;
    bool is_running;
} SubGhzProtocolEncoderVAG;

static bool vag_aut64_encrypt(uint8_t* block, int key_index) {
    struct aut64_key* key = protocol_vag_get_key(key_index + 1);
    if(!key) {
        FURI_LOG_E(TAG, "Key not found for encryption: %d", key_index + 1);
        return false;
    }
    int rc = aut64_encrypt(key, block);
#ifdef AUT64_ENABLE_VALIDATIONS
    if(rc == AUT64_ERR_INVALID_KEY) {
        FURI_LOG_E(TAG, "Invalid key: %d", key_index + 1);
    } else if(rc == AUT64_ERR_NULL_POINTER) {
        FURI_LOG_E(TAG, "key is NULL");
    }
    return (rc == AUT64_OK) ? true : false;
#else
    UNUSED(rc);
    return true;
#endif
}

static uint8_t vag_get_dispatch_byte(uint8_t btn, uint8_t vag_type) {
    if(vag_type == 1 || vag_type == 2) {
        switch(btn) {
        case 0x20:
        case 2:
            return 0x2A;
        case 0x40:
        case 4:
            return 0x46;
        case 0x10:
        case 1:
            return 0x1C;
        default:
            return 0x2A;
        }
    } else {
        switch(btn) {
        case 0x20:
        case 2:
            return 0x2B;
        case 0x40:
        case 4:
            return 0x47;
        case 0x10:
        case 1:
            return 0x1D;
        default:
            return 0x2B;
        }
    }
}

static uint8_t vag_btn_to_byte(uint8_t btn, uint8_t vag_type) {
    if(vag_type == 1) {
        return btn;
    } else {
        switch(btn) {
        case 0x1:
            return 0x10;
        case 0x2:
            return 0x20;
        case 0x4:
            return 0x40;
        default:
            return btn;
        }
    }
}

static void vag_encoder_build_type1(SubGhzProtocolEncoderVAG* instance) {
    size_t index = 0;
    LevelDuration* upload = instance->upload;
    const size_t cap = VAG_ENCODER_UPLOAD_MAX_SIZE;

    uint8_t btn_byte = vag_btn_to_byte(instance->btn, 1);
    uint8_t dispatch = vag_get_dispatch_byte(btn_byte, 1);
    instance->dispatch_byte = dispatch;
    uint8_t block[8];
    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);

    block[0] = (uint8_t)(instance->serial >> 24);
    block[1] = (uint8_t)(instance->serial >> 16);
    block[2] = (uint8_t)(instance->serial >> 8);
    block[3] = (uint8_t)(instance->serial);

    block[4] = (uint8_t)(instance->cnt);
    block[5] = (uint8_t)(instance->cnt >> 8);
    block[6] = (uint8_t)(instance->cnt >> 16);

    block[7] = btn_byte;
    int key_idx = (instance->key_idx != 0xFF) ? instance->key_idx : 0;
    if(!vag_aut64_encrypt(block, key_idx)) {
        FURI_LOG_E(TAG, "Type1 AUT64 encryption failed! Key not loaded?");
        instance->size_upload = 0;
        return;
    }
    instance->key1_high = ((uint32_t)type_byte << 24) | ((uint32_t)block[0] << 16) |
                          ((uint32_t)block[1] << 8) | (uint32_t)block[2];
    instance->key1_low = ((uint32_t)block[3] << 24) | ((uint32_t)block[4] << 16) |
                         ((uint32_t)block[5] << 8) | (uint32_t)block[6];
    uint32_t key2_upper = ((uint32_t)(block[7] & 0xFF) << 8);
    uint32_t key2_lower = (uint32_t)(dispatch & 0xFF);
    instance->key2_low = (key2_upper | key2_lower) & 0xFFFF;
    instance->key2_high = 0;

    index = (({
        size_t _pi = index;
        for(size_t _pp = 0; _pp < 220; _pp++) {
            if(_pi < cap) upload[_pi++] = level_duration_make(true, 300);
            if(_pi < cap) upload[_pi++] = level_duration_make(false, 300);
        }
        _pi;
    }));
    index =
        (index < cap ? (index++, upload[index - 1] = level_duration_make(false, 300), index) :
                       index);
    index =
        (index < cap ? (index++, upload[index - 1] = level_duration_make(true, 300), index) :
                       index);
    uint16_t prefix = 0xAF3F;
    for(int i = 15; i >= 0; i--) {
        bool bit = (prefix >> i) & 1;
        index = vag_emit_manchester_inv(upload, index, cap, bit, 300);
    }
    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint64_t key1_inv = ~key1;
    for(int i = 63; i >= 0; i--) {
        bool bit = (key1_inv >> i) & 1;
        index = vag_emit_manchester_inv(upload, index, cap, bit, 300);
    }
    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);
    uint16_t key2_inv = ~key2;
    for(int i = 15; i >= 0; i--) {
        bool bit = (key2_inv >> i) & 1;
        index = vag_emit_manchester_inv(upload, index, cap, bit, 300);
    }
    index =
        (index < cap ? (index++, upload[index - 1] = level_duration_make(false, 6000), index) :
                       index);

    instance->size_upload = index;
    if(index != 635) {
        FURI_LOG_W(TAG, "WARNING: Pulse count %zu != expected 635!", index);
    }
}

static void vag_encoder_build_type2(SubGhzProtocolEncoderVAG* instance) {
    size_t index = 0;
    LevelDuration* upload = instance->upload;
    const size_t cap = VAG_ENCODER_UPLOAD_MAX_SIZE;

    uint8_t btn_byte = vag_btn_to_byte(instance->btn, 2);
    uint8_t dispatch = vag_get_dispatch_byte(btn_byte, 2);
    instance->dispatch_byte = dispatch;
    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);

    uint8_t block[8];
    block[0] = (uint8_t)(instance->serial >> 24);
    block[1] = (uint8_t)(instance->serial >> 16);
    block[2] = (uint8_t)(instance->serial >> 8);
    block[3] = (uint8_t)(instance->serial);

    block[4] = (uint8_t)(instance->cnt);
    block[5] = (uint8_t)(instance->cnt >> 8);
    block[6] = (uint8_t)(instance->cnt >> 16);

    block[7] = btn_byte;
    uint32_t v0 = ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) |
                  ((uint32_t)block[2] << 8) | (uint32_t)block[3];
    uint32_t v1 = ((uint32_t)block[4] << 24) | ((uint32_t)block[5] << 16) |
                  ((uint32_t)block[6] << 8) | (uint32_t)block[7];
    vag_tea_encrypt(&v0, &v1, vag_tea_key_schedule);
    block[0] = (uint8_t)(v0 >> 24);
    block[1] = (uint8_t)(v0 >> 16);
    block[2] = (uint8_t)(v0 >> 8);
    block[3] = (uint8_t)(v0);
    block[4] = (uint8_t)(v1 >> 24);
    block[5] = (uint8_t)(v1 >> 16);
    block[6] = (uint8_t)(v1 >> 8);
    block[7] = (uint8_t)(v1);
    instance->key1_high = ((uint32_t)type_byte << 24) | ((uint32_t)block[0] << 16) |
                          ((uint32_t)block[1] << 8) | (uint32_t)block[2];
    instance->key1_low = ((uint32_t)block[3] << 24) | ((uint32_t)block[4] << 16) |
                         ((uint32_t)block[5] << 8) | (uint32_t)block[6];
    uint32_t key2_upper = ((uint32_t)(block[7] & 0xFF) << 8);
    uint32_t key2_lower = (uint32_t)(dispatch & 0xFF);
    instance->key2_low = (key2_upper | key2_lower) & 0xFFFF;
    instance->key2_high = 0;

    index = (({
        size_t _pi = index;
        for(size_t _pp = 0; _pp < 220; _pp++) {
            if(_pi < cap) upload[_pi++] = level_duration_make(true, 300);
            if(_pi < cap) upload[_pi++] = level_duration_make(false, 300);
        }
        _pi;
    }));
    index =
        (index < cap ? (index++, upload[index - 1] = level_duration_make(false, 300), index) :
                       index);
    index =
        (index < cap ? (index++, upload[index - 1] = level_duration_make(true, 300), index) :
                       index);
    uint16_t prefix = 0xAF1C;
    for(int i = 15; i >= 0; i--) {
        bool bit = (prefix >> i) & 1;
        index = vag_emit_manchester_inv(upload, index, cap, bit, 300);
    }
    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint64_t key1_inv = ~key1;
    for(int i = 63; i >= 0; i--) {
        bool bit = (key1_inv >> i) & 1;
        index = vag_emit_manchester_inv(upload, index, cap, bit, 300);
    }
    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);
    uint16_t key2_inv = ~key2;
    for(int i = 15; i >= 0; i--) {
        bool bit = (key2_inv >> i) & 1;
        index = vag_emit_manchester_inv(upload, index, cap, bit, 300);
    }
    index =
        (index < cap ? (index++, upload[index - 1] = level_duration_make(false, 6000), index) :
                       index);

    instance->size_upload = index;
}

static void vag_encoder_build_type3_4(SubGhzProtocolEncoderVAG* instance) {
    size_t index = 0;
    LevelDuration* upload = instance->upload;
    const size_t cap = VAG_ENCODER_UPLOAD_MAX_SIZE;

    uint8_t btn_byte = vag_btn_to_byte(instance->btn, instance->vag_type);
    uint8_t dispatch = vag_get_dispatch_byte(btn_byte, instance->vag_type);
    instance->dispatch_byte = dispatch;
    uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);

    uint8_t block[8];
    block[0] = (uint8_t)(instance->serial >> 24);
    block[1] = (uint8_t)(instance->serial >> 16);
    block[2] = (uint8_t)(instance->serial >> 8);
    block[3] = (uint8_t)(instance->serial);
    block[4] = (uint8_t)(instance->cnt);
    block[5] = (uint8_t)(instance->cnt >> 8);
    block[6] = (uint8_t)(instance->cnt >> 16);
    block[7] = btn_byte;
    int key_idx;
    if(instance->key_idx != 0xFF) {
        key_idx = instance->key_idx;
    } else {
        key_idx = (instance->vag_type == 4) ? 2 : 1;
    }
    if(!vag_aut64_encrypt(block, key_idx)) {
        FURI_LOG_E(TAG, "Type%d AUT64 encryption failed! Key not loaded?", instance->vag_type);
        instance->size_upload = 0;
        return;
    }
    instance->key1_high = ((uint32_t)type_byte << 24) | ((uint32_t)block[0] << 16) |
                          ((uint32_t)block[1] << 8) | (uint32_t)block[2];
    instance->key1_low = ((uint32_t)block[3] << 24) | ((uint32_t)block[4] << 16) |
                         ((uint32_t)block[5] << 8) | (uint32_t)block[6];
    uint32_t key2_upper = ((uint32_t)(block[7] & 0xFF) << 8);
    uint32_t key2_lower = (uint32_t)(dispatch & 0xFF);
    instance->key2_low = (key2_upper | key2_lower) & 0xFFFF;
    instance->key2_high = 0;

    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);

    for(int repeat = 0; repeat < 2; repeat++) {
        index = (({
            size_t _pi = index;
            for(size_t _pp = 0; _pp < 45; _pp++) {
                if(_pi < cap) upload[_pi++] = level_duration_make(true, 500);
                if(_pi < cap) upload[_pi++] = level_duration_make(false, 500);
            }
            _pi;
        }));
        index =
            (index < cap ? (index++, upload[index - 1] = level_duration_make(true, 1000), index) :
                           index);
        index =
            (index < cap ? (index++, upload[index - 1] = level_duration_make(false, 500), index) :
                           index);
        index = (({
            size_t _pi = index;
            for(size_t _pp = 0; _pp < 3; _pp++) {
                if(_pi < cap) upload[_pi++] = level_duration_make(true, 750);
                if(_pi < cap) upload[_pi++] = level_duration_make(false, 750);
            }
            _pi;
        }));

        for(int i = 63; i >= 0; i--) {
            bool bit = (key1 >> i) & 1;

            index = vag_emit_manchester_inv(upload, index, cap, bit, 500);
        }
        for(int i = 15; i >= 0; i--) {
            bool bit = (key2 >> i) & 1;
            index = vag_emit_manchester_inv(upload, index, cap, bit, 500);
        }
        index =
            (index < cap ?
                 (index++, upload[index - 1] = level_duration_make(false, 10000), index) :
                 index);
    }

    instance->size_upload = index;
    if(index != 518) {
        FURI_LOG_W(TAG, "WARNING: Pulse count %zu != expected 518!", index);
    }
}
void subghz_protocol_decoder_vag_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderVAG* instance = context;

    if(!instance->decrypted && instance->data_count_bit >= 80) {
        vag_parse_data(instance);
    }

    vag_get_string_setup_editor(instance->btn, instance->cnt, 4, 24, 4);

    const uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    const uint16_t key2 = (uint16_t)(instance->key2_low & 0xFFFF);

    const uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);
    const char* vehicle_name;
    switch(type_byte) {
    case 0x00:
        vehicle_name = "VW Passat";
        break;
    case 0xC0:
        vehicle_name = "VW";
        break;
    case 0xC1:
        vehicle_name = "Audi";
        break;
    case 0xC2:
        vehicle_name = "Seat";
        break;
    case 0xC3:
        vehicle_name = "Skoda";
        break;
    default:
        vehicle_name = "VAG";
        break;
    }

    if(instance->decrypted) {
        furi_string_cat_printf(output, "%s %ubit\r\n", vehicle_name, instance->data_count_bit);
        furi_string_cat_printf(
            output,
            "Key1:%08lX%08lX\r\nKey2:%04X\r\n",
            (unsigned long)(uint32_t)(key1 >> 32),
            (unsigned long)(uint32_t)(key1 & 0xFFFFFFFFU),
            (unsigned)key2);
        furi_string_cat_printf(
            output,
            "Sn:%08lX Btn:%02X [%s]\r\n",
            (unsigned long)instance->serial,
            instance->btn,
            vag_button_name(instance->btn));
        furi_string_cat_printf(output, "Cnt:%06lX\r\n", (unsigned long)instance->cnt);
    } else {
        furi_string_cat_printf(output, "%s %ubit\r\n", vehicle_name, instance->data_count_bit);
        furi_string_cat_printf(
            output,
            "Key1:%08lX%08lX\r\nKey2:%04X\r\n",
            (unsigned long)(uint32_t)(key1 >> 32),
            (unsigned long)(uint32_t)(key1 & 0xFFFFFFFFU),
            (unsigned)key2);
        furi_string_cat_printf(output, "Key2:%04X (corrupted)\r\n", (unsigned)key2);
    }
}

void* subghz_protocol_encoder_vag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderVAG* instance = calloc(1, sizeof(SubGhzProtocolEncoderVAG));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_vag;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->upload = malloc(VAG_UPLOAD_CAPACITY * sizeof(LevelDuration));
    furi_check(instance->upload);
    instance->repeat = 3;
    instance->is_running = false;
    instance->key_idx = 0xFF;

    protocol_vag_load_keys(SUBGHZ_VAG_KEYS_FILE);
    return instance;
}

void subghz_protocol_encoder_vag_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderVAG* instance = context;
    free(instance->upload);
    free(instance);
}

void subghz_protocol_encoder_vag_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderVAG* instance = context;
    instance->is_running = false;
}

bool subghz_protocol_encoder_vag_get_upload(
    void* context,
    const LevelDuration** upload,
    size_t* size) {
    furi_check(context);
    furi_check(upload);
    furi_check(size);

    SubGhzProtocolEncoderVAG* instance = context;
    if(!instance->upload || instance->size_upload == 0) {
        return false;
    }

    *upload = instance->upload;
    *size = instance->size_upload;
    return true;
}

LevelDuration subghz_protocol_encoder_vag_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderVAG* instance = context;

    if(!instance->is_running || instance->repeat == 0) {
        if(instance->is_running) {
        }
        instance->is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->upload[instance->front];
    instance->front++;

    if(instance->front >= instance->size_upload) {
        instance->front = 0;
        if(!subghz_block_generic_global.endless_tx) instance->repeat--;
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_vag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderVAG* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Encoder deserialize: generic failed (ret=%d)", ret);
            break;
        }

        uint64_t key1 = instance->generic.data;
        instance->key1_low = (uint32_t)key1;
        instance->key1_high = (uint32_t)(key1 >> 32);

        uint16_t key2_16bit = 0;
        uint8_t key2_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, "Key2", key2_bytes, 8)) {
            key2_16bit = ((uint16_t)key2_bytes[6] << 8) | (uint16_t)key2_bytes[7];
        } else if(instance->generic.data_2) {
            key2_16bit = (uint16_t)(instance->generic.data_2 & 0xFFFF);
        }
        instance->key2_low = (uint32_t)key2_16bit & 0xFFFF;
        instance->key2_high = 0;

        uint32_t serial_val = instance->generic.serial;
        uint32_t btn_val = instance->generic.btn;
        uint32_t cnt_val = instance->generic.cnt;
        uint32_t type_val = instance->generic.type;
        vag_encoder_read_fields(flipper_format, &serial_val, &btn_val, &cnt_val, &type_val);

        {
            uint8_t btn = (uint8_t)btn_val;
            if(!subghz_custom_btn_has_original()) {
                subghz_custom_btn_set_original(btn);
            }
            btn = vag_get_btn_code(btn);
            subghz_block_generic_global_button_override_get(&btn);
            btn_val = btn;
        }
        if(!subghz_block_generic_global_counter_override_get(&cnt_val)) {
            cnt_val += furi_hal_subghz_get_rolling_counter_mult();
        }

        instance->vag_type = (uint8_t)type_val;
        if(instance->vag_type == 0) {
            instance->vag_type = 1;
        }

        uint32_t file_key_idx = 0xFF;
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "KeyIdx", &file_key_idx, 1);

        instance->serial = serial_val;
        instance->cnt = cnt_val;
        instance->btn = (uint8_t)btn_val;
        instance->key_idx = (uint8_t)file_key_idx;
        if(instance->key_idx == 0xFF) {
            SubGhzProtocolDecoderVAG decoder;
            memset(&decoder, 0, sizeof(decoder));
            decoder.key1_low = instance->key1_low;
            decoder.key1_high = instance->key1_high;
            decoder.key2_low = instance->key2_low;
            decoder.key2_high = instance->key2_high;
            decoder.vag_type = instance->vag_type;
            decoder.data_count_bit = 80;
            decoder.key_idx = 0xFF;
            vag_parse_data(&decoder);

            if(decoder.decrypted) {
                instance->key_idx = decoder.key_idx;
                if(instance->serial == 0 && instance->cnt == 0) {
                    instance->serial = decoder.serial;
                    instance->cnt = decoder.cnt;
                    instance->btn = decoder.btn;
                }
            } else {
                FURI_LOG_W(
                    TAG,
                    "Could not decode original signal - check if keys are loaded and Type is correct");
            }
        }
        uint8_t type_byte = (uint8_t)(instance->key1_high >> 24);
        if(instance->vag_type == 1 && type_byte == 0x00) {
            instance->vag_type = 2;
        }
        switch(instance->vag_type) {
        case 1:
            vag_encoder_build_type1(instance);
            break;
        case 2:
            vag_encoder_build_type2(instance);
            break;
        case 3:
        case 4:
            vag_encoder_build_type3_4(instance);
            break;
        default:
            FURI_LOG_W(TAG, "Unknown type %d, defaulting to type 1", instance->vag_type);
            instance->vag_type = 1;
            vag_encoder_build_type1(instance);
            break;
        }

        if(instance->size_upload == 0) {
            FURI_LOG_E(TAG, "Failed to build upload buffer!");
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        flipper_format_rewind(flipper_format);
        uint8_t key1_bytes[8];
        key1_bytes[0] = (uint8_t)(instance->key1_high >> 24);
        key1_bytes[1] = (uint8_t)(instance->key1_high >> 16);
        key1_bytes[2] = (uint8_t)(instance->key1_high >> 8);
        key1_bytes[3] = (uint8_t)(instance->key1_high);
        key1_bytes[4] = (uint8_t)(instance->key1_low >> 24);
        key1_bytes[5] = (uint8_t)(instance->key1_low >> 16);
        key1_bytes[6] = (uint8_t)(instance->key1_low >> 8);
        key1_bytes[7] = (uint8_t)(instance->key1_low);
        if(!flipper_format_update_hex(flipper_format, "Key", key1_bytes, 8)) {
            FURI_LOG_W(TAG, "Failed to update Key in file (non-fatal)");
        }

        flipper_format_rewind(flipper_format);
        instance->key2_high = 0;
        uint16_t key2_write = (uint16_t)(instance->key2_low & 0xFFFF);
        instance->key2_low = (uint32_t)key2_write;
        uint8_t key2_write_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        key2_write_bytes[6] = (uint8_t)((key2_write >> 8) & 0xFF);
        key2_write_bytes[7] = (uint8_t)(key2_write & 0xFF);
        if(!flipper_format_update_hex(flipper_format, "Key2", key2_write_bytes, 8)) {
            FURI_LOG_W(TAG, "Failed to update Key2 in file (non-fatal)");
        }

        vag_flipper_update_or_insert_u32(flipper_format, "Serial", instance->serial);
        vag_flipper_update_or_insert_u32(flipper_format, "Cnt", instance->cnt);
        vag_flipper_update_or_insert_u32(flipper_format, "Btn", instance->btn);
        vag_flipper_update_or_insert_u32(flipper_format, "KeyIdx", instance->key_idx);
        vag_flipper_update_or_insert_u32(flipper_format, "Type", instance->vag_type);
        instance->repeat = 3;
        instance->front = 0;
        instance->is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    if(ret != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "=== VAG ENCODER DESERIALIZE FAILED (ret=%d) ===", ret);
    }

    return ret;
}
