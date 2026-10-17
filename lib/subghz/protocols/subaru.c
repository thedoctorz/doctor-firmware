#include "subaru.h"

#include "../blocks/const.h"
#include "../blocks/custom_btn_i.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubaruProtocol"

static const SubGhzBlockConst subghz_protocol_subaru_const = {
    .te_short = 800,
    .te_long = 1600,
    .te_delta = 200,
    .min_count_bit_for_found = 64,
};

#define SUBARU_PREAMBLE_PAIRS  75
#define SUBARU_GAP_US          2800
#define SUBARU_SYNC_US         2800
#define SUBARU_UPLOAD_CAPACITY 320U
typedef struct SubGhzProtocolDecoderSubaru {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;

    uint64_t key;
    uint32_t serial;
    uint8_t btn;
    uint16_t cnt;
} SubGhzProtocolDecoderSubaru;

typedef struct SubGhzProtocolEncoderSubaru {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t key;
    uint32_t serial;
    uint8_t btn;
    uint16_t cnt;
} SubGhzProtocolEncoderSubaru;

typedef enum {
    SubaruDecoderStepReset = 0,
    SubaruDecoderStepCheckPreamble,
    SubaruDecoderStepFoundGap,
    SubaruDecoderStepFoundSync,
    SubaruDecoderStepSaveDuration,
} SubaruDecoderStep;

static const char* subaru_get_button_name(uint8_t btn) {
    static const char* const names[4] = {"Lock", "Unlock", "Trunk", "Panic"};
    return names[btn & 0x03];
}

static void subaru_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)((data >> ((7U - i) * 8U)) & 0xFFU);
    }
}

static uint64_t subaru_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t i = 0; i < 8; i++) {
        data = (data << 8U) | bytes[i];
    }
    return data;
}

static void subaru_encoder_read_fields(
    FlipperFormat* flipper_format,
    uint32_t* serial_out,
    uint32_t* btn_out,
    uint32_t* cnt_out) {
    uint32_t tmp = 0;
    if(serial_out) {
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Serial", &tmp, 1)) {
            *serial_out = tmp;
        }
    }
    if(btn_out) {
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Btn", &tmp, 1)) {
            *btn_out = tmp;
        }
    }
    if(cnt_out) {
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Cnt", &tmp, 1)) {
            *cnt_out = tmp;
        }
    }
}

static uint8_t subaru_get_btn_code(void);

static inline void subaru_encoder_apply_button(uint8_t* btn) {
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(*btn);
    }
    *btn = subaru_get_btn_code();
    subghz_block_generic_global_button_override_get(btn);
}

static inline void subaru_encoder_apply_counter(uint32_t* cnt) {
    if(!subghz_block_generic_global_counter_override_get(cnt)) {
        *cnt += furi_hal_subghz_get_rolling_counter_mult();
    }
}

static uint8_t subghz_protocol_decoder_subaru_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_subaru_decoder = {
    .alloc = subghz_protocol_decoder_subaru_alloc,
    .free = subghz_protocol_decoder_subaru_free,
    .feed = subghz_protocol_decoder_subaru_feed,
    .reset = subghz_protocol_decoder_subaru_reset,
    .get_hash_data = subghz_protocol_decoder_subaru_get_hash_data,
    .serialize = subghz_protocol_decoder_subaru_serialize,
    .deserialize = subghz_protocol_decoder_subaru_deserialize,
    .get_string = subghz_protocol_decoder_subaru_get_string,
};

static uint8_t subaru_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x0;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x3;
    default:
        return subghz_custom_btn_get_original();
    }
}

const SubGhzProtocolEncoder subghz_protocol_subaru_encoder = {
    .alloc = subghz_protocol_encoder_subaru_alloc,
    .free = subghz_protocol_encoder_subaru_free,
    .deserialize = subghz_protocol_encoder_subaru_deserialize,
    .stop = subghz_protocol_encoder_subaru_stop,
    .yield = subghz_protocol_encoder_subaru_yield,
};

const SubGhzProtocol subghz_protocol_subaru = {
    .name = SUBGHZ_PROTOCOL_SUBARU_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_subaru_decoder,
    .encoder = &subghz_protocol_subaru_encoder,
};

// ============================================================================
// DECODER HELPER FUNCTIONS
// ============================================================================

static void subaru_rotate_left_3bytes(uint8_t* b0, uint8_t* b1, uint8_t* b2, uint8_t count) {
    for(uint8_t i = 0; i < count; i++) {
        uint8_t t = *b0;
        *b0 = (uint8_t)((*b0 << 1) | (*b1 >> 7));
        *b1 = (uint8_t)((*b1 << 1) | (*b2 >> 7));
        *b2 = (uint8_t)((*b2 << 1) | (t >> 7));
    }
}

static bool subaru_decode_fields_exact(
    const uint8_t* kb,
    uint32_t* out_serial,
    uint8_t* out_btn,
    uint16_t* out_cnt) {
    const uint8_t b0 = kb[0];
    const uint8_t b1 = kb[1];
    const uint8_t b2 = kb[2];
    const uint8_t b3 = kb[3];
    const uint8_t b4 = kb[4];
    const uint8_t b5 = kb[5];
    const uint8_t b6 = kb[6];
    const uint8_t b7 = kb[7];

    *out_btn = b0 & 0x0F;
    *out_serial = ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;

    uint8_t lo = 0;
    if((b4 & 0x40) == 0) lo |= 0x01;
    if((b4 & 0x80) == 0) lo |= 0x02;
    if((b5 & 0x01) == 0) lo |= 0x04;
    if((b5 & 0x02) == 0) lo |= 0x08;
    if((b6 & 0x01) == 0) lo |= 0x10;
    if((b6 & 0x02) == 0) lo |= 0x20;
    if((b5 & 0x40) == 0) lo |= 0x40;
    if((b5 & 0x80) == 0) lo |= 0x80;
    uint8_t reg_sh1 = (uint8_t)(((b7 & 0x0F) << 4) | (b5 & 0x0C) | ((b6 >> 6) & 0x03));
    uint8_t reg_sh2 = (uint8_t)(((b6 & 0x3C) << 2) | ((b7 >> 4) & 0x0F));

    uint8_t ser0 = b3;
    uint8_t ser1 = b1;
    uint8_t ser2 = b2;
    subaru_rotate_left_3bytes(&ser0, &ser1, &ser2, (uint8_t)(4U + lo));

    uint8_t t1 = (uint8_t)(ser1 ^ reg_sh1);
    uint8_t t2 = (uint8_t)(ser2 ^ reg_sh2);

    uint8_t hi =
        (uint8_t)((((t1 & 0x10) == 0) ? 0x04 : 0x00) | (((t1 & 0x20) == 0) ? 0x08 : 0x00) |
                  (((t2 & 0x80) == 0) ? 0x02 : 0x00) | (((t2 & 0x40) == 0) ? 0x01 : 0x00) |
                  (((t1 & 0x01) == 0) ? 0x40 : 0x00) | (((t1 & 0x02) == 0) ? 0x80 : 0x00) |
                  (((t2 & 0x08) == 0) ? 0x20 : 0x00) | (((t2 & 0x04) == 0) ? 0x10 : 0x00));

    const uint8_t local34 = ser0;

    const uint8_t x1 = (uint8_t)(b1 ^ reg_sh1);
    const uint8_t x2 = (uint8_t)(b2 ^ reg_sh2);
    const uint8_t reg_hi_inv = (uint8_t)(~hi);
    const uint8_t expect1 = (uint8_t)(((x1 ^ (uint8_t)(reg_hi_inv << 2)) & 0x30) |
                                      ((x1 ^ (uint8_t)(reg_hi_inv >> 6)) & 0x03) |
                                      ((x1 ^ (uint8_t)(~lo << 2)) & 0xCC));
    const uint8_t expect2 = (uint8_t)(((x2 ^ (uint8_t)(reg_hi_inv >> 2)) & 0x0C) |
                                      ((x2 ^ (uint8_t)(reg_hi_inv << 6)) & 0xC0) |
                                      ((x2 ^ (uint8_t)(~lo >> 2)) & 0x33));

    const bool valid = (((uint8_t)(b1 ^ expect1) | (uint8_t)(b2 ^ expect2)) == 0) &&
                       ((uint8_t)(b0 ^ local34) == lo);

    *out_cnt = (uint16_t)((uint16_t)hi << 8 | lo);

    return valid;
}

static bool subaru_process_data(SubGhzProtocolDecoderSubaru* instance) {
    if(instance->decoder.decode_count_bit < 64) {
        return false;
    }

    uint8_t b[8];
    subaru_u64_to_bytes_be(instance->decoder.decode_data, b);

    instance->key = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) |
                    ((uint64_t)b[3] << 32) | ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                    ((uint64_t)b[6] << 8) | ((uint64_t)b[7]);

    (void)subaru_decode_fields_exact(b, &instance->serial, &instance->btn, &instance->cnt);

    return true;
}

static uint8_t subghz_protocol_decoder_subaru_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    const uint8_t* p = (const uint8_t*)&instance->decoder.decode_data;
    uint8_t hash = 0;
    const uint8_t bytes = (uint8_t)(instance->decoder.decode_count_bit >> 3);
    for(uint8_t i = 0; i <= bytes; i++) {
        hash ^= p[i];
    }
    return hash;
}

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================
static void subaru_encode_count(uint8_t* KB, uint16_t count) {
    uint8_t lo = count & 0xFF;
    uint8_t hi = (count >> 8) & 0xFF;

    KB[4] &= ~0xC0;
    KB[5] &= ~0xC3;
    KB[6] &= ~0x03;

    if((lo & 0x01) == 0) KB[4] |= 0x40;
    if((lo & 0x02) == 0) KB[4] |= 0x80;
    if((lo & 0x04) == 0) KB[5] |= 0x01;
    if((lo & 0x08) == 0) KB[5] |= 0x02;
    if((lo & 0x10) == 0) KB[6] |= 0x01;
    if((lo & 0x20) == 0) KB[6] |= 0x02;
    if((lo & 0x40) == 0) KB[5] |= 0x40;
    if((lo & 0x80) == 0) KB[5] |= 0x80;

    uint8_t SER0 = KB[3];
    uint8_t SER1 = KB[1];
    uint8_t SER2 = KB[2];

    uint8_t total_rot = 4 + lo;
    for(uint8_t i = 0; i < total_rot; ++i) {
        uint8_t t_bit = (SER0 >> 7) & 1;
        SER0 = ((SER0 << 1) & 0xFE) | ((SER1 >> 7) & 1);
        SER1 = ((SER1 << 1) & 0xFE) | ((SER2 >> 7) & 1);
        SER2 = ((SER2 << 1) & 0xFE) | t_bit;
    }

    const uint8_t rel = (uint8_t)(SER0 ^ KB[0]);
    KB[4] = (uint8_t)((KB[4] & 0xC0) | ((rel >> 2) & 0x3F));
    KB[5] = (uint8_t)((KB[5] & 0xCF) | ((rel << 4) & 0x30));

    uint8_t T1 = 0xFF;
    uint8_t T2 = 0xFF;

    if(hi & 0x04) T1 &= ~0x10;
    if(hi & 0x08) T1 &= ~0x20;
    if(hi & 0x02) T2 &= ~0x80;
    if(hi & 0x01) T2 &= ~0x40;
    if(hi & 0x40) T1 &= ~0x01;
    if(hi & 0x80) T1 &= ~0x02;
    if(hi & 0x20) T2 &= ~0x08;
    if(hi & 0x10) T2 &= ~0x04;

    uint8_t new_REG_SH1 = T1 ^ SER1;
    uint8_t new_REG_SH2 = T2 ^ SER2;

    KB[5] &= ~0x0C;
    KB[6] &= ~0xC0;

    KB[7] = (KB[7] & 0xF0) | ((new_REG_SH1 >> 4) & 0x0F);

    if(new_REG_SH1 & 0x04) KB[5] |= 0x04;
    if(new_REG_SH1 & 0x08) KB[5] |= 0x08;
    if(new_REG_SH1 & 0x02) KB[6] |= 0x80;
    if(new_REG_SH1 & 0x01) KB[6] |= 0x40;

    KB[6] = (KB[6] & 0xC3) | ((new_REG_SH2 >> 2) & 0x3C);

    KB[7] = (KB[7] & 0x0F) | ((new_REG_SH2 << 4) & 0xF0);
}

void* subghz_protocol_encoder_subaru_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderSubaru* instance = malloc(sizeof(SubGhzProtocolEncoderSubaru));

    instance->base.protocol = &subghz_protocol_subaru;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = SUBARU_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    instance->key = 0;
    instance->serial = 0;
    instance->btn = 0;
    instance->cnt = 0;

    return instance;
}

static void subghz_protocol_encoder_subaru_get_upload(SubGhzProtocolEncoderSubaru* instance) {
    furi_check(instance);
    size_t index = 0;

    const uint32_t te_short = subghz_protocol_subaru_const.te_short;
    const uint32_t te_long = subghz_protocol_subaru_const.te_long;
    const uint32_t gap_duration = SUBARU_GAP_US;
    const uint32_t sync_duration = SUBARU_SYNC_US;

    FURI_LOG_I(
        TAG,
        "Building upload: key=0x%016llX, serial=0x%06lX, btn=0x%X, cnt=0x%04X",
        instance->key,
        instance->serial,
        instance->btn,
        instance->cnt);

    LevelDuration* up = instance->encoder.upload;

    for(int i = 0; i < SUBARU_PREAMBLE_PAIRS; i++) {
        up[index++] = level_duration_make(false, te_long);
        up[index++] = level_duration_make(true, te_long);
    }

    up[index++] = level_duration_make(false, gap_duration);
    up[index++] = level_duration_make(true, sync_duration);

    for(int i = 63; i >= 0; i--) {
        bool bit = (instance->key >> i) & 1;
        if(bit) {
            up[index++] = level_duration_make(false, te_long);
            up[index++] = level_duration_make(true, te_short);
        } else {
            up[index++] = level_duration_make(false, te_short);
            up[index++] = level_duration_make(true, te_long);
        }
    }

    up[index++] = level_duration_make(false, gap_duration);
    up[index++] = level_duration_make(true, sync_duration);

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;

    FURI_LOG_I(TAG, "Upload built: %zu elements", instance->encoder.size_upload);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_subaru_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderSubaru* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_subaru_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        instance->key = instance->generic.data;

        uint8_t b[8];
        subaru_u64_to_bytes_be(instance->key, b);

        uint32_t serial = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        uint8_t btn = b[0] & 0x0F;
        uint16_t cnt = 0;
        (void)subaru_decode_fields_exact(b, &serial, &btn, &cnt);

        uint32_t serial_ff = serial;
        uint32_t btn_ff = btn;
        uint32_t cnt_ff = cnt;
        subaru_encoder_read_fields(flipper_format, &serial_ff, &btn_ff, &cnt_ff);

        subaru_encoder_apply_button((uint8_t*)&btn_ff);
        subaru_encoder_apply_counter(&cnt_ff);

        instance->serial = serial_ff & 0xFFFFFF;
        instance->btn = (uint8_t)(btn_ff & 0x0F);
        instance->cnt = (uint16_t)(cnt_ff & 0xFFFF);

        b[0] = (b[0] & 0xF0) | instance->btn;
        b[1] = (uint8_t)(instance->serial >> 16);
        b[2] = (uint8_t)(instance->serial >> 8);
        b[3] = (uint8_t)(instance->serial);
        subaru_encode_count(b, instance->cnt);

        instance->key = subaru_bytes_to_u64_be(b);
        instance->generic.data = instance->key;
        instance->generic.data_count_bit = subghz_protocol_subaru_const.min_count_bit_for_found;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->btn;
        instance->generic.cnt = instance->cnt;

        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &instance->serial, 1);
        {
            uint32_t btn_u32 = instance->btn;
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_u32, 1);
        }
        {
            uint32_t cnt_u32 = instance->cnt;
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_u32, 1);
        }
        {
            uint8_t key_bytes[8];
            subaru_u64_to_bytes_be(instance->key, key_bytes);
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(
                flipper_format, "Key", key_bytes, sizeof(key_bytes));
        }
        subghz_block_generic_global.current_cnt = instance->cnt;

        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(
               flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1)) {
            instance->encoder.repeat = 3;
        }

        subghz_protocol_encoder_subaru_get_upload(instance);
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_subaru_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSubaru* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_subaru_stop(void* context) {
    SubGhzProtocolEncoderSubaru* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_subaru_yield(void* context) {
    SubGhzProtocolEncoderSubaru* instance = context;

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

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_subaru_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSubaru* instance = malloc(sizeof(SubGhzProtocolDecoderSubaru));
    instance->base.protocol = &subghz_protocol_subaru;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_subaru_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_subaru_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    instance->decoder.parser_step = SubaruDecoderStepReset;
}

void subghz_protocol_decoder_subaru_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSubaru* instance = context;

    const uint32_t te_short = subghz_protocol_subaru_const.te_short;
    const uint32_t te_long = subghz_protocol_subaru_const.te_long;
    const uint32_t te_delta = subghz_protocol_subaru_const.te_delta;

    switch(instance->decoder.parser_step) {
    case SubaruDecoderStepReset:

        if(level && (DURATION_DIFF(duration, te_long) < te_delta)) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->header_count = 0;
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = SubaruDecoderStepCheckPreamble;
        }
        break;

    case SubaruDecoderStepCheckPreamble:
        if(level) {
            break;
        }

        if((DURATION_DIFF(duration, te_long) < te_delta) &&
           (DURATION_DIFF(instance->decoder.te_last, te_long) < te_delta)) {
            instance->header_count++;
            break;
        }

        if((instance->header_count >= 0x15) && (DURATION_DIFF(duration, SUBARU_GAP_US) < 800)) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = SubaruDecoderStepFoundGap;
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;

    case SubaruDecoderStepFoundGap:
        if(level && (DURATION_DIFF(duration, SUBARU_SYNC_US) < 800) &&
           (DURATION_DIFF(instance->decoder.te_last, SUBARU_GAP_US) < 800)) {
            instance->decoder.parser_step = SubaruDecoderStepFoundSync;
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;

    case SubaruDecoderStepFoundSync:
        if(!level && ((DURATION_DIFF(duration, te_long) < te_delta) ||
                      (DURATION_DIFF(duration, te_short) < te_delta))) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = SubaruDecoderStepSaveDuration;
        } else {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;

    case SubaruDecoderStepSaveDuration: {
        uint8_t next_step = SubaruDecoderStepReset;
        bool bit = false;
        bool valid = false;

        if(level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                if(DURATION_DIFF(instance->decoder.te_last, te_short) < te_delta) {
                    bit = false;
                    valid = true;
                }
            } else if(DURATION_DIFF(duration, te_short) < te_delta) {
                if(DURATION_DIFF(instance->decoder.te_last, te_long) < te_delta) {
                    bit = true;
                    valid = true;
                }
            }

            if(valid) {
                subghz_protocol_blocks_add_bit(&instance->decoder, bit);
                next_step = SubaruDecoderStepFoundSync;
            }
        }

        instance->decoder.parser_step = next_step;

        if((instance->decoder.decode_count_bit >= 64) &&
           (instance->decoder.decode_count_bit <= 80)) {
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            instance->generic.data = instance->decoder.decode_data;
            if(subaru_process_data(instance)) {
                instance->generic.serial = instance->serial;
                instance->generic.btn = instance->btn;
                instance->generic.cnt = instance->cnt;
            }

            if(instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }

            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;
    }
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSubaru* instance = context;

    uint8_t b[8];
    subaru_u64_to_bytes_be(instance->generic.data, b);
    (void)subaru_decode_fields_exact(b, &instance->serial, &instance->btn, &instance->cnt);
    instance->key = instance->generic.data;
    instance->generic.serial = instance->serial;
    instance->generic.btn = instance->btn;
    instance->generic.cnt = instance->cnt;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_uint32(flipper_format, "Serial", &instance->serial, 1);
    {
        uint32_t btn_u32 = instance->btn;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_u32, 1);
    }
    {
        uint32_t cnt_u32 = instance->cnt;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_u32, 1);
    }

    FuriString* display = furi_string_alloc();
    furi_string_printf(
        display, "%s - %s", instance->generic.protocol_name, subaru_get_button_name(instance->btn));
    flipper_format_rewind(flipper_format);
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(display));
    furi_string_free(display);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_subaru_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSubaru* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_subaru_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_subaru_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderSubaru* instance = context;

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = 4;
    subghz_block_generic_global.cnt_length_bit = 16;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(instance->generic.btn);
    }
    subghz_custom_btn_set_max(4);

    uint8_t b[8];
    subaru_u64_to_bytes_be(instance->generic.data, b);
    (void)subaru_decode_fields_exact(b, &instance->serial, &instance->btn, &instance->cnt);
    instance->key = instance->generic.data;
    instance->generic.serial = instance->serial;
    instance->generic.btn = instance->btn;
    instance->generic.cnt = instance->cnt;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%016llX\r\n"
        "Sn:%06lX Btn:%02X [%s]\r\nCnt:%04lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->serial,
        instance->btn,
        subaru_get_button_name(instance->btn),
        (unsigned long)instance->cnt);
}
