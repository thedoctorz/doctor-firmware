#include "kia_v5.h"
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
    kia_v5_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void kia_v5_encoder_read_fields(
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

static uint32_t kia_v5_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus kia_v5_encoder_read_bit(
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

static SubGhzProtocolStatus kia_v5_serialize_fields(
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

static void kia_v5_get_string_setup_editor(
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
    kia_v5_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static inline ManchesterEvent
    kia_v5_manchester_event(uint32_t duration, bool level, const SubGhzBlockConst* t) {
    if(DURATION_DIFF(duration, t->te_short) < t->te_delta) {
        return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    }
    if(DURATION_DIFF(duration, t->te_long) < t->te_delta) {
        return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }
    return ManchesterEventReset;
}

static uint8_t kia_v5_reverse_bits8(uint8_t value) {
    reverse_bits_in_bytes(&value, 1);
    return value;
}

static void kia_v5_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t kia_v5_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static SubGhzProtocolStatus
    kia_v5_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static const char* kia_v5_get_button_name(uint8_t btn) {
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

#include "keys.h"

#define TAG "KiaV5"

static const SubGhzBlockConst subghz_protocol_kia_v5_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 150,
    .min_count_bit_for_found = 64,
};

static void build_keystore_from_mfkey(uint8_t* result) {
    uint64_t mfkey = get_kia_v5_key();
    for(int i = 0; i < 8; i++) {
        result[i] = (uint8_t)((mfkey >> ((7 - i) * 8)) & 0xFFU);
    }
}

static uint8_t keystore_bytes[8] = {0};

static uint16_t mixer_decode(uint32_t encrypted) {
    uint8_t s0 = (encrypted & 0xFF);
    uint8_t s1 = (encrypted >> 8) & 0xFF;
    uint8_t s2 = (encrypted >> 16) & 0xFF;
    uint8_t s3 = (encrypted >> 24) & 0xFF;

    // Prepare key
    build_keystore_from_mfkey(keystore_bytes);

    int round_index = 1;
    for(size_t i = 0; i < 18; i++) {
        uint8_t r = keystore_bytes[round_index] & 0xFF;
        int steps = 8;
        while(steps > 0) {
            uint8_t base;
            if((s3 & 0x40) == 0) {
                base = (s3 & 0x02) == 0 ? 0x74 : 0x2E;
            } else {
                base = (s3 & 0x02) == 0 ? 0x3A : 0x5C;
            }

            if(s2 & 0x08) {
                base = (((base >> 4) & 0x0F) | ((base & 0x0F) << 4)) & 0xFF;
            }
            if(s1 & 0x01) {
                base = ((base & 0x3F) << 2) & 0xFF;
            }
            if(s0 & 0x01) {
                base = (base << 1) & 0xFF;
            }

            uint8_t temp = (s3 ^ s1) & 0xFF;
            s3 = ((s3 & 0x7F) << 1) & 0xFF;
            if(s2 & 0x80) {
                s3 |= 0x01;
            }
            s2 = ((s2 & 0x7F) << 1) & 0xFF;
            if(s1 & 0x80) {
                s2 |= 0x01;
            }
            s1 = ((s1 & 0x7F) << 1) & 0xFF;
            if(s0 & 0x80) {
                s1 |= 0x01;
            }
            s0 = ((s0 & 0x7F) << 1) & 0xFF;

            uint8_t chk = (base ^ (r ^ temp)) & 0xFF;
            if(chk & 0x80) {
                s0 |= 0x01;
            }
            r = ((r & 0x7F) << 1) & 0xFF;
            steps--;
        }
        round_index = (round_index - 1) & 0x7;
    }
    return (s0 + (s1 << 8)) & 0xFFFF;
}

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

static uint32_t mixer_encode(uint32_t serial, uint16_t counter, uint8_t button) {
    build_keystore_from_mfkey(keystore_bytes);

    uint8_t state_a = (uint8_t)(((serial >> 8) & 0x0FU) | ((button & 0x0FU) << 4));
    uint8_t state_b = (uint8_t)((counter >> 8) & 0xFFU);
    uint8_t state_c = (uint8_t)(serial & 0xFFU);
    uint8_t state_d = (uint8_t)(counter & 0xFFU);

    int ks_idx = 0;
    for(int round_i = 0; round_i < 18; round_i++) {
        uint8_t r = keystore_bytes[ks_idx] & 0xFFU;
        ks_idx = (ks_idx + 1) & 0x07;

        uint8_t running_d = state_d;
        for(int step = 0; step < 8; step++) {
            uint8_t base;
            if((state_a & 0x80U) == 0) {
                base = (state_a & 0x04U) == 0 ? 0x74U : 0x2EU;
            } else {
                base = (state_a & 0x04U) == 0 ? 0x3AU : 0x5CU;
            }

            if(state_c & 0x10U) {
                base = (uint8_t)(((base >> 4) & 0x0FU) | ((base & 0x0FU) << 4));
            }
            if(state_b & 0x02U) {
                base = (uint8_t)((base & 0x3FU) << 2);
            }

            uint8_t base_final = base;
            if(running_d & 0x02U) {
                base_final = (uint8_t)((base & 0x7FU) << 1);
            }

            const bool carry_b = (state_b & 0x01U) != 0;
            const bool carry_c = (state_c & 0x01U) != 0;
            const bool carry_a = (state_a & 0x01U) != 0;

            uint8_t new_d = (uint8_t)(running_d >> 1);
            if(carry_b) new_d |= 0x80U;

            running_d ^= state_c;

            state_b = (uint8_t)(state_b >> 1);
            if(carry_c) state_b |= 0x80U;

            state_c = (uint8_t)(state_c >> 1);
            if(carry_a) state_c |= 0x80U;

            const uint8_t feedback = (uint8_t)(((running_d ^ r) << 7) ^ base_final);
            state_a = (uint8_t)(state_a >> 1);
            if(feedback & 0x80U) state_a |= 0x80U;

            r = (uint8_t)(r >> 1);
            running_d = new_d;
        }
        state_d = running_d;
    }

    return ((uint32_t)state_a << 24) | ((uint32_t)state_c << 16) | ((uint32_t)state_b << 8) |
           (uint32_t)state_d;
}

struct SubGhzProtocolDecoderKiaV5 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    ManchesterState manchester_state;
    uint64_t decoded_data;
    uint64_t saved_key;
    uint8_t bit_count;
    uint64_t yek;
    uint8_t crc;
};

struct SubGhzProtocolEncoderKiaV5 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t replay_data;
    uint8_t replay_crc;
};

#define KIA_V5_PREAMBLE_PAIRS 200U
#define KIA_V5_SYNC_ENTRIES   4U
#define KIA_V5_DATA_BITS      64U
#define KIA_V5_CRC_BITS       3U
#define KIA_V5_END_ENTRIES    2U
#define KIA_V5_UPLOAD_CAPACITY                          \
    (KIA_V5_PREAMBLE_PAIRS * 2U + KIA_V5_SYNC_ENTRIES + \
     (KIA_V5_DATA_BITS + KIA_V5_CRC_BITS) * 2U + KIA_V5_END_ENTRIES)
typedef enum {
    KiaV5DecoderStepReset = 0,
    KiaV5DecoderStepCheckPreamble,
    KiaV5DecoderStepData,
} KiaV5DecoderStep;

void subghz_protocol_decoder_kia_v5_free(void* context) {
    furi_assert(context);
    free(context);
}

uint8_t subghz_protocol_decoder_kia_v5_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_encoder_kia_v5_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v5_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v5_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV5* instance = context;
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
const SubGhzProtocolDecoder subghz_protocol_kia_v5_decoder = {
    .alloc = subghz_protocol_decoder_kia_v5_alloc,
    .free = subghz_protocol_decoder_kia_v5_free,
    .feed = subghz_protocol_decoder_kia_v5_feed,
    .reset = subghz_protocol_decoder_kia_v5_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v5_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v5_serialize,
    .deserialize = subghz_protocol_decoder_kia_v5_deserialize,
    .get_string = subghz_protocol_decoder_kia_v5_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_v5_encoder = {
    .alloc = subghz_protocol_encoder_kia_v5_alloc,
    .free = subghz_protocol_encoder_kia_v5_free,
    .deserialize = subghz_protocol_encoder_kia_v5_deserialize,
    .stop = subghz_protocol_encoder_kia_v5_stop,
    .yield = subghz_protocol_encoder_kia_v5_yield,
};

const SubGhzProtocol subghz_protocol_kia_v5 = {
    .name = SUBGHZ_PROTOCOL_KIA_V5_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_v5_decoder,
    .encoder = &subghz_protocol_kia_v5_encoder,
};

static uint8_t kia_v5_calculate_crc(uint64_t data) {
    uint8_t crc = 0;
    for(int i = 63; i >= 0; i--) {
        const uint8_t bit = (data >> i) & 1U;
        const uint8_t shifted_out = (crc >> 1U) & 1U;
        crc = (uint8_t)(((crc & 1U) << 1U) | bit);
        if(shifted_out) {
            crc ^= 3U;
        }
    }
    return (uint8_t)(crc & 3U);
}

void* subghz_protocol_encoder_kia_v5_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolEncoderKiaV5* instance = calloc(1, sizeof(SubGhzProtocolEncoderKiaV5));
    furi_check(instance);

    if(environment) {
        kia_keys_load(environment);
    }

    instance->base.protocol = &subghz_protocol_kia_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 6;
    instance->encoder.size_upload = 0;
    instance->encoder.upload = NULL;
    instance->encoder.is_running = false;
    return instance;
}

static size_t kia_v5_emit_manchester_bit(LevelDuration* up, size_t i, size_t cap, bool bit_value) {
    const uint32_t te = subghz_protocol_kia_v5_const.te_short;
    if(bit_value) {
        i = kia_v5_emit(up, i, cap, false, te);
        i = kia_v5_emit(up, i, cap, true, te);
    } else {
        i = kia_v5_emit(up, i, cap, true, te);
        i = kia_v5_emit(up, i, cap, false, te);
    }
    return i;
}

static void subghz_protocol_encoder_kia_v5_get_upload(SubGhzProtocolEncoderKiaV5* instance) {
    LevelDuration* upload = instance->encoder.upload;
    const size_t cap = KIA_V5_UPLOAD_CAPACITY;
    const uint32_t te_short = subghz_protocol_kia_v5_const.te_short;
    const uint32_t te_long = subghz_protocol_kia_v5_const.te_long;
    size_t i = 0;

    for(size_t p = 0; p < KIA_V5_PREAMBLE_PAIRS; p++) {
        i = kia_v5_emit(upload, i, cap, true, te_short);
        i = kia_v5_emit(upload, i, cap, false, te_short);
    }

    i = kia_v5_emit(upload, i, cap, false, te_short);
    i = kia_v5_emit(upload, i, cap, true, te_long);
    i = kia_v5_emit(upload, i, cap, false, te_short);
    i = kia_v5_emit(upload, i, cap, true, te_short);

    for(int b = (int)KIA_V5_DATA_BITS - 1; b >= 0; b--) {
        const bool bit_value = ((instance->replay_data >> b) & 1ULL) != 0ULL;
        i = kia_v5_emit_manchester_bit(upload, i, cap, bit_value);
    }

    i = kia_v5_emit_manchester_bit(upload, i, cap, false);
    i = kia_v5_emit_manchester_bit(upload, i, cap, ((instance->replay_crc >> 1U) & 1U) != 0U);
    i = kia_v5_emit_manchester_bit(upload, i, cap, (instance->replay_crc & 1U) != 0U);

    i = kia_v5_emit(upload, i, cap, false, te_short);
    i = kia_v5_emit(upload, i, cap, true, te_short);

    instance->encoder.size_upload = i;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV5* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(kia_v5_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "V5 enc: protocol mismatch");
        return SubGhzProtocolStatusError;
    }

    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(status != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "V5 enc: generic deserialize failed (%d)", status);
        return status;
    }
    if(instance->generic.data_count_bit < subghz_protocol_kia_v5_const.min_count_bit_for_found) {
        FURI_LOG_E(
            TAG, "V5 enc: bit count too low: %u", (unsigned)instance->generic.data_count_bit);
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    uint32_t serial_v = UINT32_MAX;
    uint32_t btn_v = UINT32_MAX;
    uint32_t cnt_v = UINT32_MAX;
    kia_v5_encoder_read_fields(flipper_format, &serial_v, &btn_v, &cnt_v, NULL);

    uint64_t yek_captured = 0;
    for(int i = 0; i < 8; i++) {
        const uint8_t b = (uint8_t)((instance->generic.data >> (i * 8)) & 0xFFU);
        yek_captured |= ((uint64_t)kia_v5_reverse_bits8(b) << ((7 - i) * 8));
    }
    if(serial_v == UINT32_MAX) {
        serial_v = (uint32_t)((yek_captured >> 32) & 0x0FFFFFFFU);
    }
    if(btn_v == UINT32_MAX) {
        btn_v = (uint32_t)((yek_captured >> 60) & 0x0FU);
    }
    if(cnt_v == UINT32_MAX) {
        cnt_v = mixer_decode((uint32_t)(yek_captured & 0xFFFFFFFFU));
    }

    serial_v &= 0x0FFFFFFFU;
    btn_v &= 0x0FU;
    cnt_v &= 0xFFFFU;

    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original((uint8_t)btn_v);
    }
    instance->generic.serial = serial_v;
    instance->generic.btn = (uint8_t)btn_v;
    instance->generic.cnt = cnt_v;
    instance->generic.btn = kia_default_get_btn_code();
    subghz_block_generic_global_button_override_get(&instance->generic.btn);
    if(!subghz_block_generic_global_counter_override_get(&instance->generic.cnt)) {
        instance->generic.cnt += furi_hal_subghz_get_rolling_counter_mult();
    }
    btn_v = instance->generic.btn & 0x0FU;
    cnt_v = instance->generic.cnt & 0xFFFFU;

    const uint32_t mixer = mixer_encode(serial_v, (uint16_t)cnt_v, (uint8_t)btn_v);
    const uint64_t yek_new = ((uint64_t)btn_v << 60) | ((uint64_t)serial_v << 32) |
                             (uint64_t)mixer;

    uint64_t data_new = 0;
    for(int i = 0; i < 8; i++) {
        const uint8_t b = (uint8_t)((yek_new >> (i * 8)) & 0xFFU);
        data_new |= ((uint64_t)kia_v5_reverse_bits8(b) << ((7 - i) * 8));
    }

    instance->replay_data = data_new;
    instance->generic.data = data_new;
    instance->generic.serial = serial_v;
    instance->generic.btn = (uint8_t)btn_v;
    instance->generic.cnt = (uint16_t)cnt_v;
    instance->replay_crc = kia_v5_calculate_crc(instance->replay_data);

    FURI_LOG_I(
        TAG,
        "V5 enc reencrypt: sn=%07lX btn=%X cnt=%04lX mixer=%08lX",
        (unsigned long)serial_v,
        (unsigned)btn_v,
        (unsigned long)cnt_v,
        (unsigned long)mixer);

    instance->encoder.repeat = (int32_t)kia_v5_encoder_read_repeat(flipper_format, 6);

    instance->encoder.size_upload = KIA_V5_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    subghz_protocol_encoder_kia_v5_get_upload(instance);

    flipper_format_rewind(flipper_format);
    char key_out[20];
    snprintf(key_out, sizeof(key_out), "%016llX", (unsigned long long)instance->generic.data);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key_out);
    uint32_t serial_w = instance->generic.serial;
    flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
    uint32_t btn_w = instance->generic.btn;
    flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
    uint32_t cnt_w = instance->generic.cnt;
    flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    uint32_t crc_temp = instance->replay_crc;
    flipper_format_insert_or_update_uint32(flipper_format, "CRC", &crc_temp, 1);
    uint32_t raw_high = (uint32_t)(instance->replay_data >> 32);
    flipper_format_insert_or_update_uint32(flipper_format, "DataHi", &raw_high, 1);
    uint32_t raw_low = (uint32_t)(instance->replay_data & 0xFFFFFFFFULL);
    flipper_format_insert_or_update_uint32(flipper_format, "DataLo", &raw_low, 1);
    uint32_t yek_high = (uint32_t)(yek_new >> 32);
    flipper_format_insert_or_update_uint32(flipper_format, "YekHi", &yek_high, 1);
    uint32_t yek_low = (uint32_t)(yek_new & 0xFFFFFFFFULL);
    flipper_format_insert_or_update_uint32(flipper_format, "YekLo", &yek_low, 1);
    subghz_block_generic_global.current_cnt = instance->generic.cnt;

    instance->encoder.is_running = true;

    FURI_LOG_I(
        TAG,
        "V5 enc ready: data=%08lX%08lX crc=%X repeat=%u size=%zu",
        (uint32_t)(instance->replay_data >> 32),
        (uint32_t)(instance->replay_data & 0xFFFFFFFFULL),
        instance->replay_crc,
        (unsigned)instance->encoder.repeat,
        instance->encoder.size_upload);
    FURI_LOG_I(
        TAG,
        "V5 enc preamble[0..3]: %s%lu %s%lu %s%lu %s%lu",
        level_duration_get_level(instance->encoder.upload[0]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[0]),
        level_duration_get_level(instance->encoder.upload[1]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[1]),
        level_duration_get_level(instance->encoder.upload[2]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[2]),
        level_duration_get_level(instance->encoder.upload[3]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[3]));
    FURI_LOG_I(
        TAG,
        "V5 enc sync[400..403]: %s%lu %s%lu %s%lu %s%lu",
        level_duration_get_level(instance->encoder.upload[400]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[400]),
        level_duration_get_level(instance->encoder.upload[401]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[401]),
        level_duration_get_level(instance->encoder.upload[402]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[402]),
        level_duration_get_level(instance->encoder.upload[403]) ? "H" : "L",
        (unsigned long)level_duration_get_duration(instance->encoder.upload[403]));

    return SubGhzProtocolStatusOk;
}

static void kia_v5_add_bit(SubGhzProtocolDecoderKiaV5* instance, bool bit) {
    instance->decoded_data = (instance->decoded_data << 1) | (bit ? 1 : 0);
    instance->bit_count++;
}

void* subghz_protocol_decoder_kia_v5_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderKiaV5* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV5));
    furi_check(instance);

    if(environment) {
        kia_keys_load(environment);
    }

    instance->base.protocol = &subghz_protocol_kia_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v5_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    instance->decoder.parser_step = KiaV5DecoderStepReset;
    instance->header_count = 0;
    instance->bit_count = 0;
    instance->decoded_data = 0;
    instance->saved_key = 0;
    instance->yek = 0;
    instance->crc = 0;
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_kia_v5_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV5DecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
                       subghz_protocol_kia_v5_const.te_delta)) {
            instance->decoder.parser_step = KiaV5DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
            instance->bit_count = 0;
            instance->decoded_data = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case KiaV5DecoderStepCheckPreamble:
        if(level) {
            if(DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_long) <
               subghz_protocol_kia_v5_const.te_delta) {
                if(instance->header_count > 40) {
                    instance->decoder.parser_step = KiaV5DecoderStepData;
                    instance->bit_count = 0;
                    instance->decoded_data = 0;
                    instance->saved_key = 0;
                    instance->header_count = 0;
                } else {
                    instance->decoder.te_last = duration;
                }
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
                subghz_protocol_kia_v5_const.te_delta) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV5DecoderStepReset;
            }
        } else {
            if((DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
                subghz_protocol_kia_v5_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v5_const.te_short) <
                subghz_protocol_kia_v5_const.te_delta)) {
                instance->header_count++;
            } else if(
                (DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_long) <
                 subghz_protocol_kia_v5_const.te_delta) &&
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v5_const.te_short) <
                 subghz_protocol_kia_v5_const.te_delta)) {
                instance->header_count++;
            } else if(
                DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v5_const.te_long) <
                subghz_protocol_kia_v5_const.te_delta) {
                instance->header_count++;
            } else {
                instance->decoder.parser_step = KiaV5DecoderStepReset;
            }
            instance->decoder.te_last = duration;
        }
        break;

    case KiaV5DecoderStepData: {
        ManchesterEvent event;

        if(DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_short) <
           subghz_protocol_kia_v5_const.te_delta) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_kia_v5_const.te_long) <
            subghz_protocol_kia_v5_const.te_delta) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            if(instance->bit_count >= subghz_protocol_kia_v5_const.min_count_bit_for_found) {
                instance->crc = (uint8_t)(instance->decoded_data & 0x07);

                // Same check the encoder applies when it rebuilds a frame. Without it
                // the decoder reported anything that fed 67 Manchester bits through -
                // including AM noise, since the receiver hands every decoder the
                // demodulated stream regardless of the protocol's FM-only flag.
                if((instance->crc & 3U) != kia_v5_calculate_crc(instance->saved_key)) {
                    instance->decoder.parser_step = KiaV5DecoderStepReset;
                    break;
                }

                instance->generic.data = instance->saved_key;
                instance->generic.data_count_bit =
                    (instance->bit_count > 67) ? 67 : instance->bit_count;

                instance->yek = 0;
                for(int i = 0; i < 8; i++) {
                    uint8_t byte = (instance->generic.data >> (i * 8)) & 0xFF;
                    uint8_t reversed = 0;
                    for(int b = 0; b < 8; b++) {
                        if(byte & (1 << b)) reversed |= (1 << (7 - b));
                    }
                    instance->yek |= ((uint64_t)reversed << ((7 - i) * 8));
                }

                instance->generic.serial = (uint32_t)((instance->yek >> 32) & 0x0FFFFFFF);
                instance->generic.btn = (uint8_t)((instance->yek >> 60) & 0x0F);

                uint32_t encrypted = (uint32_t)(instance->yek & 0xFFFFFFFF);
                instance->generic.cnt = mixer_decode(encrypted);

                FURI_LOG_I(
                    TAG,
                    "Key=%08lX%08lX Sn=%07lX Btn=%X Cnt=%04lX CRC=%X",
                    (uint32_t)(instance->generic.data >> 32),
                    (uint32_t)(instance->generic.data & 0xFFFFFFFF),
                    instance->generic.serial,
                    instance->generic.btn,
                    instance->generic.cnt,
                    instance->crc);

                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }

            instance->decoder.parser_step = KiaV5DecoderStepReset;
            break;
        }

        bool data_bit;
        if(instance->bit_count <= 66 &&
           manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            kia_v5_add_bit(instance, data_bit);

            if(instance->bit_count == 64) {
                instance->saved_key = instance->decoded_data;
                instance->decoded_data = 0;
            }
        }

        instance->decoder.te_last = duration;
        break;
    }
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        // Serial/Btn/Cnt are already written by subghz_block_generic_serialize().

        uint32_t crc_temp = instance->crc;
        flipper_format_write_uint32(flipper_format, "CRC", &crc_temp, 1);

        // Save raw bit data for exact reproduction (since V5 has complex bit reversal)
        uint32_t raw_high = (uint32_t)(instance->generic.data >> 32);
        uint32_t raw_low = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
        flipper_format_write_uint32(flipper_format, "DataHi", &raw_high, 1);
        flipper_format_write_uint32(flipper_format, "DataLo", &raw_low, 1);
        uint32_t yek_high = (uint32_t)(instance->yek >> 32);
        uint32_t yek_low = (uint32_t)(instance->yek & 0xFFFFFFFF);
        flipper_format_write_uint32(flipper_format, "YekHi", &yek_high, 1);
        flipper_format_write_uint32(flipper_format, "YekLo", &yek_low, 1);
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(status != SubGhzProtocolStatusOk) {
        return status;
    }
    if(instance->generic.data_count_bit < subghz_protocol_kia_v5_const.min_count_bit_for_found) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    flipper_format_rewind(flipper_format);
    uint32_t yek_high = 0;
    uint32_t yek_low = 0;
    if(flipper_format_read_uint32(flipper_format, "YekHi", &yek_high, 1) &&
       flipper_format_read_uint32(flipper_format, "YekLo", &yek_low, 1)) {
        instance->yek = ((uint64_t)yek_high << 32) | yek_low;
    } else {
        instance->yek = 0;
        for(int i = 0; i < 8; i++) {
            const uint8_t byte = (uint8_t)((instance->generic.data >> (i * 8)) & 0xFFU);
            instance->yek |= ((uint64_t)kia_v5_reverse_bits8(byte) << ((7 - i) * 8));
        }
    }

    flipper_format_rewind(flipper_format);
    uint32_t crc_temp = 0;
    if(flipper_format_read_uint32(flipper_format, "CRC", &crc_temp, 1)) {
        instance->crc = (uint8_t)crc_temp;
    }

    uint32_t serial = instance->generic.serial;
    uint32_t btn = instance->generic.btn;
    uint32_t cnt = instance->generic.cnt;
    flipper_format_rewind(flipper_format);
    kia_v5_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
    if(serial || btn || cnt) {
        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)btn;
        instance->generic.cnt = (uint16_t)cnt;
    } else if(instance->yek) {
        instance->generic.serial = (uint32_t)((instance->yek >> 32) & 0x0FFFFFFFU);
        instance->generic.btn = (uint8_t)((instance->yek >> 60) & 0x0FU);
        instance->generic.cnt = mixer_decode((uint32_t)(instance->yek & 0xFFFFFFFFU));
    }

    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_kia_v5_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV5* instance = context;
    kia_v5_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 16, 5);

    const uint32_t yek_hi = (uint32_t)(instance->yek >> 32);
    const uint32_t yek_lo = (uint32_t)(instance->yek & 0xFFFFFFFFU);

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key:%08lX%08lX\r\n",
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFFU));
    furi_string_cat_printf(
        output, "Yek:%08lX%08lX\r\n", (unsigned long)yek_hi, (unsigned long)yek_lo);
    {
        const char* btn_name = kia_v5_get_button_name(instance->generic.btn);
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
    furi_string_cat_printf(output, "CRC:%02X\r\n", instance->crc);
}
