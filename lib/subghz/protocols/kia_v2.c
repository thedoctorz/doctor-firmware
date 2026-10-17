#include "kia_v2.h"
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
    kia_v2_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void kia_v2_encoder_read_fields(
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

static uint32_t kia_v2_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus kia_v2_encoder_read_bit(
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

static SubGhzProtocolStatus kia_v2_serialize_fields(
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

static void kia_v2_get_string_setup_editor(
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
    kia_v2_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static inline ManchesterEvent
    kia_v2_manchester_event(uint32_t duration, bool level, const SubGhzBlockConst* t) {
    if(DURATION_DIFF(duration, t->te_short) < t->te_delta) {
        return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    }
    if(DURATION_DIFF(duration, t->te_long) < t->te_delta) {
        return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }
    return ManchesterEventReset;
}

static uint8_t kia_v2_reverse_bits8(uint8_t value) {
    reverse_bits_in_bytes(&value, 1);
    return value;
}

static void kia_v2_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t kia_v2_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static SubGhzProtocolStatus
    kia_v2_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static const char* kia_v2_get_button_name(uint8_t btn) {
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

#include <furi.h>

#define TAG "KiaV2"

#define KIA_V2_HEADER_PAIRS 70
#define KIA_V2_TOTAL_BURSTS 2
#define KIA_V2_UPLOAD_CAPACITY \
    (KIA_V2_TOTAL_BURSTS * ((KIA_V2_HEADER_PAIRS * 2U) + ((53U - 1U) * 2U) + 1U))
static const SubGhzBlockConst subghz_protocol_kia_v2_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 200,
    .min_count_bit_for_found = 53,
};

struct SubGhzProtocolDecoderKiaV2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    ManchesterState manchester_state;
};

struct SubGhzProtocolEncoderKiaV2 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    KiaV2DecoderStepReset = 0,
    KiaV2DecoderStepCheckPreamble,
    KiaV2DecoderStepCollectRawBits,
} KiaV2DecoderStep;

void subghz_protocol_decoder_kia_v2_free(void* context) {
    furi_assert(context);
    free(context);
}

uint8_t subghz_protocol_decoder_kia_v2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_encoder_kia_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v2_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV2* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v2_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV2* instance = context;
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
const SubGhzProtocolDecoder subghz_protocol_kia_v2_decoder = {
    .alloc = subghz_protocol_decoder_kia_v2_alloc,
    .free = subghz_protocol_decoder_kia_v2_free,
    .feed = subghz_protocol_decoder_kia_v2_feed,
    .reset = subghz_protocol_decoder_kia_v2_reset,
    .get_hash_data = subghz_protocol_decoder_kia_v2_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v2_serialize,
    .deserialize = subghz_protocol_decoder_kia_v2_deserialize,
    .get_string = subghz_protocol_decoder_kia_v2_get_string,
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

const SubGhzProtocolEncoder subghz_protocol_kia_v2_encoder = {
    .alloc = subghz_protocol_encoder_kia_v2_alloc,
    .free = subghz_protocol_encoder_kia_v2_free,
    .deserialize = subghz_protocol_encoder_kia_v2_deserialize,
    .stop = subghz_protocol_encoder_kia_v2_stop,
    .yield = subghz_protocol_encoder_kia_v2_yield,
};

const SubGhzProtocol subghz_protocol_kia_v2 = {
    .name = SUBGHZ_PROTOCOL_KIA_V2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_v2_decoder,
    .encoder = &subghz_protocol_kia_v2_encoder,
};

static uint8_t kia_v2_calculate_crc(uint64_t data) {
    // Remove the CRC nibble (last 4 bits) to get the actual data
    uint64_t data_without_crc = data >> 4;

    // Extract 6 bytes from the data
    uint8_t bytes[6];
    bytes[0] = (uint8_t)(data_without_crc);
    bytes[1] = (uint8_t)(data_without_crc >> 8);
    bytes[2] = (uint8_t)(data_without_crc >> 16);
    bytes[3] = (uint8_t)(data_without_crc >> 24);
    bytes[4] = (uint8_t)(data_without_crc >> 32);
    bytes[5] = (uint8_t)(data_without_crc >> 40);

    uint8_t crc = 0;
    for(int i = 0; i < 6; i++) {
        crc ^= (bytes[i] & 0x0F) ^ (bytes[i] >> 4);
    }

    return (crc + 1) & 0x0F;
}
static void kia_v2_push(
    LevelDuration* upload,
    size_t* index,
    bool* have_last,
    bool* last_level,
    bool level,
    uint32_t duration) {
    if(!upload || *index >= KIA_V2_UPLOAD_CAPACITY) {
        *index = KIA_V2_UPLOAD_CAPACITY + 1U;
        return;
    }
    upload[(*index)++] = level_duration_make(level, duration);
    *last_level = level;
    *have_last = true;
}

static void kia_v2_emit_decoder_bit(
    LevelDuration* upload,
    size_t* index,
    bool* have_last,
    bool* last_level,
    uint8_t* state,
    bool bit,
    uint32_t te) {
    if(*state == ManchesterStateMid1) {
        if(bit) {
            kia_v2_push(upload, index, have_last, last_level, false, te);
            kia_v2_push(upload, index, have_last, last_level, true, te);
        } else {
            kia_v2_push(upload, index, have_last, last_level, false, te * 2U);
            *state = ManchesterStateMid0;
        }
        return;
    }

    if(bit) {
        kia_v2_push(upload, index, have_last, last_level, true, te * 2U);
        *state = ManchesterStateMid1;
        return;
    }
    kia_v2_push(upload, index, have_last, last_level, true, te);
    kia_v2_push(upload, index, have_last, last_level, false, te);
}

static void subghz_protocol_encoder_kia_v2_get_upload(SubGhzProtocolEncoderKiaV2* instance) {
    furi_check(instance);
    if(instance->encoder.upload == NULL) return;
    size_t index = 0;

    uint8_t crc = kia_v2_calculate_crc(instance->generic.data);
    instance->generic.data = (instance->generic.data & ~0x0FULL) | crc;

    bool have_last = false;
    bool last_level = false;
    const uint32_t te = (uint32_t)subghz_protocol_kia_v2_const.te_short;
    const uint32_t te_long = (uint32_t)subghz_protocol_kia_v2_const.te_long;
    for(uint8_t burst = 0; burst < KIA_V2_TOTAL_BURSTS; burst++) {
        for(int i = 0; i < KIA_V2_HEADER_PAIRS; i++) {
            const bool sync = (i + 1) == KIA_V2_HEADER_PAIRS;
            kia_v2_push(instance->encoder.upload, &index, &have_last, &last_level, false, te_long);
            kia_v2_push(
                instance->encoder.upload,
                &index,
                &have_last,
                &last_level,
                true,
                sync ? te : te_long);
        }

        uint8_t manchester = ManchesterStateMid1;
        for(uint8_t i = instance->generic.data_count_bit; i > 1; i--) {
            kia_v2_emit_decoder_bit(
                instance->encoder.upload,
                &index,
                &have_last,
                &last_level,
                &manchester,
                bit_read(instance->generic.data, i - 2),
                te);
        }
        if(have_last && !last_level) {
            kia_v2_push(instance->encoder.upload, &index, &have_last, &last_level, true, te);
        }
    }
    if(index > KIA_V2_UPLOAD_CAPACITY) index = 0;

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;

    FURI_LOG_I(
        TAG,
        "Upload built: %d bursts, size_upload=%zu, data_count_bit=%u, data=0x%016llX",
        KIA_V2_TOTAL_BURSTS,
        instance->encoder.size_upload,
        instance->generic.data_count_bit,
        instance->generic.data);
}

void* subghz_protocol_encoder_kia_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV2* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV2));

    instance->base.protocol = &subghz_protocol_kia_v2;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 0;
    instance->encoder.upload = NULL;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV2* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    flipper_format_rewind(flipper_format);

    do {
        if(kia_v2_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Missing or wrong Protocol");
            break;
        }

        uint32_t bits = 0;
        if(kia_v2_encoder_read_bit(flipper_format, NULL, 0, &bits) != SubGhzProtocolStatusOk)
            break;

        instance->generic.data_count_bit = subghz_protocol_kia_v2_const.min_count_bit_for_found;

        uint64_t key = 0;
        if(!subghz_block_generic_read_key_u64(flipper_format, &key)) break;

        instance->generic.data = key;
        if(instance->generic.data == 0) break;

        uint32_t serial = UINT32_MAX;
        uint32_t btn = UINT32_MAX;
        uint32_t cnt = UINT32_MAX;
        kia_v2_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        if(serial == UINT32_MAX) {
            serial = (uint32_t)((instance->generic.data >> 20) & 0xFFFFFFFF);
        }
        if(btn == UINT32_MAX) {
            btn = (instance->generic.data >> 16) & 0x0F;
        }
        if(cnt == UINT32_MAX) {
            uint16_t raw_count = (uint16_t)((instance->generic.data >> 4) & 0xFFF);
            cnt = ((raw_count >> 4) | (raw_count << 8)) & 0xFFF;
        }

        if(!subghz_custom_btn_has_original()) {
            subghz_custom_btn_set_original((uint8_t)btn);
        }
        btn = kia_default_get_btn_code();
        subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        if(!subghz_block_generic_global_counter_override_get(&cnt)) {
            cnt += furi_hal_subghz_get_rolling_counter_mult();
        }
        if(serial == 0 || btn == UINT32_MAX || cnt == UINT32_MAX) break;
        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)btn;
        instance->generic.cnt = (uint16_t)cnt;

        uint64_t new_data = 0;

        new_data |= 1ULL << 52;

        new_data |= ((uint64_t)instance->generic.serial << 20) & 0xFFFFFFFFF00000ULL;

        uint32_t uVar6 = ((uint32_t)(instance->generic.cnt & 0xFF) << 8) |
                         ((uint32_t)(instance->generic.btn & 0x0F) << 16) |
                         ((uint32_t)(instance->generic.cnt >> 4) & 0xF0);

        new_data |= (uint64_t)uVar6;

        instance->generic.data = new_data;
        instance->generic.data_count_bit = 53;

        FURI_LOG_I(
            TAG,
            "Encoder reconstruct: serial=0x%08lX, btn=0x%X, cnt=0x%03lX, uVar6=0x%05lX, data=0x%016llX",
            (unsigned long)instance->generic.serial,
            (unsigned int)instance->generic.btn,
            (unsigned long)instance->generic.cnt,
            (unsigned long)uVar6,
            (unsigned long long)instance->generic.data);

        instance->encoder.repeat = (int32_t)kia_v2_encoder_read_repeat(flipper_format, 3);

        free(instance->encoder.upload);
        instance->encoder.size_upload = KIA_V2_UPLOAD_CAPACITY;
        instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
        if(!instance->encoder.upload) break;
        instance->encoder.is_running = false;
        instance->encoder.front = 0;
        subghz_protocol_encoder_kia_v2_get_upload(instance);

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
        uint32_t crc_w = instance->generic.data & 0x0FU;
        flipper_format_insert_or_update_uint32(flipper_format, "CRC", &crc_w, 1);
        uint32_t raw_count = (uint16_t)((instance->generic.data >> 4) & 0xFFFU);
        flipper_format_insert_or_update_uint32(flipper_format, "RawCnt", &raw_count, 1);
        subghz_block_generic_global.current_cnt = instance->generic.cnt;

        instance->encoder.is_running = true;

        FURI_LOG_I(
            TAG,
            "Encoder deserialized: repeat=%u, size_upload=%zu, is_running=%d, front=%zu",
            instance->encoder.repeat,
            instance->encoder.size_upload,
            instance->encoder.is_running,
            instance->encoder.front);

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void* subghz_protocol_decoder_kia_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV2* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV2));
    instance->base.protocol = &subghz_protocol_kia_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v2_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV2* instance = context;
    instance->decoder.parser_step = KiaV2DecoderStepReset;
    instance->header_count = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

void subghz_protocol_decoder_kia_v2_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV2* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV2DecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_kia_v2_const.te_long) <
                       subghz_protocol_kia_v2_const.te_delta)) {
            instance->decoder.parser_step = KiaV2DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case KiaV2DecoderStepCheckPreamble:
        if(level) // HIGH pulse
        {
            if(DURATION_DIFF(duration, subghz_protocol_kia_v2_const.te_long) <
               subghz_protocol_kia_v2_const.te_delta) {
                instance->decoder.te_last = duration;
                instance->header_count++;
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v2_const.te_short) <
                subghz_protocol_kia_v2_const.te_delta) {
                if(instance->header_count >= 100) {
                    instance->header_count = 0;
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 1;
                    instance->decoder.parser_step = KiaV2DecoderStepCollectRawBits;
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                } else {
                    instance->decoder.te_last = duration;
                }
            } else {
                instance->decoder.parser_step = KiaV2DecoderStepReset;
            }
        } else {
            if(DURATION_DIFF(duration, subghz_protocol_kia_v2_const.te_long) <
               subghz_protocol_kia_v2_const.te_delta) {
                instance->header_count++;
                instance->decoder.te_last = duration;
            } else if(
                DURATION_DIFF(duration, subghz_protocol_kia_v2_const.te_short) <
                subghz_protocol_kia_v2_const.te_delta) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV2DecoderStepReset;
            }
        }
        break;

    case KiaV2DecoderStepCollectRawBits: {
        ManchesterEvent event =
            kia_v2_manchester_event(duration, level, &subghz_protocol_kia_v2_const);
        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = KiaV2DecoderStepReset;
            break;
        }

        bool data_bit;
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            instance->decoder.decode_data = (instance->decoder.decode_data << 1) | data_bit;
            instance->decoder.decode_count_bit++;

            if(instance->decoder.decode_count_bit == 53) {
                const uint64_t data = instance->decoder.decode_data;
                if((data & 0x0F) != kia_v2_calculate_crc(data)) {
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 0;
                    instance->header_count = 0;
                    instance->decoder.parser_step = KiaV2DecoderStepReset;
                    break;
                }

                instance->generic.data = data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;

                instance->generic.serial = (uint32_t)((instance->generic.data >> 20) & 0xFFFFFFFF);
                instance->generic.btn = (uint8_t)((instance->generic.data >> 16) & 0x0F);

                uint16_t raw_count = (uint16_t)((instance->generic.data >> 4) & 0xFFF);
                instance->generic.cnt = ((raw_count >> 4) | (raw_count << 8)) & 0xFFF;

                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);

                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_count = 0;
                instance->decoder.parser_step = KiaV2DecoderStepReset;
            }
        }
        break;
    }
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV2* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t crc = instance->generic.data & 0x0F;
    if(!flipper_format_write_uint32(flipper_format, "CRC", &crc, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    uint32_t raw_count = (uint16_t)((instance->generic.data >> 4) & 0xFFF);
    if(!flipper_format_write_uint32(flipper_format, "RawCnt", &raw_count, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV2* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_kia_v2_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_kia_v2_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV2* instance = context;
    kia_v2_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 16, 5);

    const uint8_t crc = instance->generic.data & 0x0F;
    const bool crc_valid = crc == kia_v2_calculate_crc(instance->generic.data);

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(output, "Key:%013llX\r\n", (unsigned long long)instance->generic.data);
    {
        const char* btn_name = kia_v2_get_button_name(instance->generic.btn);
        if(btn_name && btn_name[0] != '\0') {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X [%s]\r\nCnt:%0*lX\r\n",
                (int)8,
                (unsigned long)instance->generic.serial,
                instance->generic.btn,
                btn_name,
                (int)3,
                (unsigned long)instance->generic.cnt);
        } else {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\nCnt:%0*lX\r\n",
                (int)8,
                (unsigned long)instance->generic.serial,
                instance->generic.btn,
                (int)3,
                (unsigned long)instance->generic.cnt);
        }
    }
    furi_string_cat_printf(output, "CRC:%02X [%s]", crc, crc_valid ? "OK" : "ERR");
}
