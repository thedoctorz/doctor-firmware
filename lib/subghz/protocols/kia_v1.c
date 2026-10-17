#include "kia_v1.h"
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
    kia_v1_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void kia_v1_encoder_read_fields(
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

static uint32_t kia_v1_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus kia_v1_encoder_read_bit(
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

static SubGhzProtocolStatus kia_v1_serialize_fields(
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

static void kia_v1_get_string_setup_editor(
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
    kia_v1_emit(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static inline ManchesterEvent
    kia_v1_manchester_event(uint32_t duration, bool level, const SubGhzBlockConst* t) {
    if(DURATION_DIFF(duration, t->te_short) < t->te_delta) {
        return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    }
    if(DURATION_DIFF(duration, t->te_long) < t->te_delta) {
        return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }
    return ManchesterEventReset;
}

static uint8_t kia_v1_reverse_bits8(uint8_t value) {
    reverse_bits_in_bytes(&value, 1);
    return value;
}

static void kia_v1_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t kia_v1_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static SubGhzProtocolStatus
    kia_v1_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

#define TAG "KiaV1"

#define KIA_V1_TOTAL_BURSTS       3
#define KIA_V1_INTER_BURST_GAP_US 25000
#define KIA_V1_HEADER_PULSES      90
#define KIA_V1_LEVEL_GAP_US       4000U
#define KIA_V1_UPLOAD_CAPACITY                                                     \
    ((KIA_V1_TOTAL_BURSTS * ((KIA_V1_HEADER_PULSES * 2) + 1 + ((57U - 1U) * 2))) + \
     (KIA_V1_TOTAL_BURSTS - 1) + (KIA_V1_TOTAL_BURSTS * 2U) + 2U)
static const SubGhzBlockConst subghz_protocol_kia_v1_const = {
    .te_short = 800,
    .te_long = 1600,
    .te_delta = 300,
    .min_count_bit_for_found = 57,
};

struct SubGhzProtocolDecoderKiaV1 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
    ManchesterState manchester_saved_state;
    uint8_t crc;
    bool crc_check;
};

struct SubGhzProtocolEncoderKiaV1 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    KiaV1DecoderStepReset = 0,
    KiaV1DecoderStepCheckPreamble,
    KiaV1DecoderStepDecodeData,
} KiaV1DecoderStep;

void subghz_protocol_decoder_kia_v1_free(void* context) {
    furi_assert(context);
    free(context);
}

uint8_t subghz_protocol_decoder_kia_v1_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}

void subghz_protocol_encoder_kia_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_kia_v1_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_kia_v1_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
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
const SubGhzProtocolDecoder subghz_protocol_kia_v1_decoder = {
    .alloc = subghz_protocol_decoder_kia_v1_alloc,
    .free = subghz_protocol_decoder_kia_v1_free,

    .feed = subghz_protocol_decoder_kia_v1_feed,
    .reset = subghz_protocol_decoder_kia_v1_reset,

    .get_hash_data = subghz_protocol_decoder_kia_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_v1_serialize,
    .deserialize = subghz_protocol_decoder_kia_v1_deserialize,
    .get_string = subghz_protocol_decoder_kia_v1_get_string,
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

const SubGhzProtocolEncoder subghz_protocol_kia_v1_encoder = {
    .alloc = subghz_protocol_encoder_kia_v1_alloc,
    .free = subghz_protocol_encoder_kia_v1_free,

    .deserialize = subghz_protocol_encoder_kia_v1_deserialize,
    .stop = subghz_protocol_encoder_kia_v1_stop,
    .yield = subghz_protocol_encoder_kia_v1_yield,
};

const SubGhzProtocol subghz_protocol_kia_v1 = {
    .name = SUBGHZ_PROTOCOL_KIA_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_kia_v1_decoder,
    .encoder = &subghz_protocol_kia_v1_encoder,
};

static void kia_v1_check_remote_controller(SubGhzProtocolDecoderKiaV1* instance);

static uint8_t kia_v1_crc4(const uint8_t* bytes, int count, uint8_t offset) {
    uint8_t crc = 0;

    for(int i = 0; i < count; i++) {
        uint8_t b = bytes[i];
        crc ^= ((b & 0x0F) ^ (b >> 4));
    }

    crc = (crc + offset) & 0x0F;
    return crc;
}

static void kia_v1_check_remote_controller(SubGhzProtocolDecoderKiaV1* instance) {
    instance->generic.serial = instance->generic.data >> 24;
    instance->generic.btn = (instance->generic.data >> 16) & 0xFF;
    instance->generic.cnt = ((instance->generic.data >> 4) & 0xF) << 8 |
                            ((instance->generic.data >> 8) & 0xFF);

    uint8_t cnt_high = (instance->generic.cnt >> 8) & 0xF;
    uint8_t char_data[7];
    char_data[0] = (instance->generic.serial >> 24) & 0xFF;
    char_data[1] = (instance->generic.serial >> 16) & 0xFF;
    char_data[2] = (instance->generic.serial >> 8) & 0xFF;
    char_data[3] = instance->generic.serial & 0xFF;
    char_data[4] = instance->generic.btn;
    char_data[5] = instance->generic.cnt & 0xFF;

    char_data[6] = cnt_high;
    uint8_t crc = kia_v1_crc4(char_data, 7, 1);

    instance->crc = cnt_high << 4 | crc;
    instance->crc_check = (crc == (instance->generic.data & 0xF));
}

static const char* kia_v1_get_button_name(uint8_t btn) {
    const char* name;
    switch(btn) {
    case 0x1:
        name = "Close";
        break;
    case 0x2:
        name = "Open";
        break;
    case 0x3:
        name = "Boot";
        break;
    default:
        name = "??";
        break;
    }
    return name;
}
void* subghz_protocol_encoder_kia_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV1* instance = malloc(sizeof(SubGhzProtocolEncoderKiaV1));

    instance->base.protocol = &subghz_protocol_kia_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = KIA_V1_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    return instance;
}

static bool kia_v1_push(
    LevelDuration* upload,
    size_t* index,
    bool* have_last,
    bool* last_level,
    bool level,
    uint32_t duration) {
    const size_t need = (*have_last && *last_level == level) ? 2U : 1U;
    if(!upload || *index + need > KIA_V1_UPLOAD_CAPACITY) return false;
    if(need == 2U) {
        upload[(*index)++] = level_duration_make(!level, KIA_V1_LEVEL_GAP_US);
    }
    upload[(*index)++] = level_duration_make(level, duration);
    *last_level = level;
    *have_last = true;
    return true;
}

static bool kia_v1_emit_decoder_bit(
    LevelDuration* upload,
    size_t* index,
    bool* have_last,
    bool* last_level,
    uint8_t* state,
    bool bit,
    uint32_t te) {
    if(*state == ManchesterStateMid1) {
        if(bit) {
            return kia_v1_push(upload, index, have_last, last_level, false, te) &&
                   kia_v1_push(upload, index, have_last, last_level, true, te);
        }
        *state = ManchesterStateMid0;
        return kia_v1_push(upload, index, have_last, last_level, false, te * 2U);
    }

    if(bit) {
        *state = ManchesterStateMid1;
        return kia_v1_push(upload, index, have_last, last_level, true, te * 2U);
    }
    return kia_v1_push(upload, index, have_last, last_level, true, te) &&
           kia_v1_push(upload, index, have_last, last_level, false, te);
}

static void subghz_protocol_encoder_kia_v1_get_upload(SubGhzProtocolEncoderKiaV1* instance) {
    furi_check(instance);
    size_t index = 0;

    uint8_t cnt_high = (instance->generic.cnt >> 8) & 0xF;
    uint8_t char_data[7];
    char_data[0] = (instance->generic.serial >> 24) & 0xFF;
    char_data[1] = (instance->generic.serial >> 16) & 0xFF;
    char_data[2] = (instance->generic.serial >> 8) & 0xFF;
    char_data[3] = instance->generic.serial & 0xFF;
    char_data[4] = instance->generic.btn;
    char_data[5] = instance->generic.cnt & 0xFF;

    char_data[6] = cnt_high;
    uint8_t crc = kia_v1_crc4(char_data, 7, 1);

    instance->generic.data = (uint64_t)instance->generic.serial << 24 |
                             instance->generic.btn << 16 | (instance->generic.cnt & 0xFF) << 8 |
                             ((instance->generic.cnt >> 8) & 0xF) << 4 | crc;

    bool have_last = false;
    bool last_level = false;
    bool fit = true;
    for(uint8_t burst = 0; burst < KIA_V1_TOTAL_BURSTS && fit; burst++) {
        if(burst > 0) {
            fit = kia_v1_push(
                instance->encoder.upload,
                &index,
                &have_last,
                &last_level,
                false,
                KIA_V1_INTER_BURST_GAP_US);
        }

        for(int i = 0; i < KIA_V1_HEADER_PULSES && fit; i++) {
            fit = kia_v1_push(
                      instance->encoder.upload,
                      &index,
                      &have_last,
                      &last_level,
                      false,
                      (uint32_t)subghz_protocol_kia_v1_const.te_long) &&
                  kia_v1_push(
                      instance->encoder.upload,
                      &index,
                      &have_last,
                      &last_level,
                      true,
                      (uint32_t)subghz_protocol_kia_v1_const.te_long);
        }

        if(fit) {
            fit = kia_v1_push(
                instance->encoder.upload,
                &index,
                &have_last,
                &last_level,
                false,
                (uint32_t)subghz_protocol_kia_v1_const.te_short);
        }

        uint8_t manchester = ManchesterStateMid1;
        const uint32_t te = (uint32_t)subghz_protocol_kia_v1_const.te_short;
        for(uint8_t i = instance->generic.data_count_bit; i > 1 && fit; i--) {
            fit = kia_v1_emit_decoder_bit(
                instance->encoder.upload,
                &index,
                &have_last,
                &last_level,
                &manchester,
                bit_read(instance->generic.data, i - 2),
                te);
        }
    }
    if(!fit) index = 0;

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;

    FURI_LOG_I(
        TAG,
        "Upload built: %d bursts, size_upload=%zu, data_count_bit=%u, data=0x%016llX",
        KIA_V1_TOTAL_BURSTS,
        instance->encoder.size_upload,
        instance->generic.data_count_bit,
        instance->generic.data);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    flipper_format_rewind(flipper_format);

    do {
        if(kia_v1_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Missing or wrong Protocol");
            break;
        }

        uint32_t bits = 0;
        if(kia_v1_encoder_read_bit(flipper_format, NULL, 0, &bits) != SubGhzProtocolStatusOk)
            break;

        instance->generic.data_count_bit = subghz_protocol_kia_v1_const.min_count_bit_for_found;

        uint64_t key = 0;
        if(!subghz_block_generic_read_key_u64(flipper_format, &key)) break;

        instance->generic.data = key;
        if(instance->generic.data == 0) break;

        uint32_t serial = UINT32_MAX;
        uint32_t btn = UINT32_MAX;
        uint32_t cnt = UINT32_MAX;
        kia_v1_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        if(serial == UINT32_MAX) {
            serial = (uint32_t)(instance->generic.data >> 24);
        }
        if(btn == UINT32_MAX) {
            btn = (instance->generic.data >> 16) & 0xFF;
        }
        if(cnt == UINT32_MAX) {
            cnt = ((instance->generic.data >> 4) & 0xF) << 8 |
                  ((instance->generic.data >> 8) & 0xFF);
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

        instance->encoder.repeat = (int32_t)kia_v1_encoder_read_repeat(flipper_format, 3);

        free(instance->encoder.upload);
        instance->encoder.size_upload = KIA_V1_UPLOAD_CAPACITY;
        instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
        if(!instance->encoder.upload) break;
        instance->encoder.is_running = false;
        instance->encoder.front = 0;
        subghz_protocol_encoder_kia_v1_get_upload(instance);

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

void subghz_protocol_encoder_kia_v1_set_button(void* context, uint8_t button) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    instance->generic.btn = button & 0xFF;
    subghz_protocol_encoder_kia_v1_get_upload(instance);
    FURI_LOG_I(TAG, "Button set to 0x%02X, upload rebuilt with new CRC", instance->generic.btn);
}

void subghz_protocol_encoder_kia_v1_set_counter(void* context, uint16_t counter) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    instance->generic.cnt = counter & 0xFFF;
    subghz_protocol_encoder_kia_v1_get_upload(instance);
    FURI_LOG_I(
        TAG,
        "Counter set to 0x%03X, upload rebuilt with new CRC",
        (uint16_t)instance->generic.cnt);
}

void subghz_protocol_encoder_kia_v1_increment_counter(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    instance->generic.cnt = (instance->generic.cnt + 1) & 0xFFF;
    subghz_protocol_encoder_kia_v1_get_upload(instance);
    FURI_LOG_I(
        TAG,
        "Counter incremented to 0x%03X, upload rebuilt with new CRC",
        (uint16_t)instance->generic.cnt);
}

uint16_t subghz_protocol_encoder_kia_v1_get_counter(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    return instance->generic.cnt;
}

uint8_t subghz_protocol_encoder_kia_v1_get_button(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV1* instance = context;
    return instance->generic.btn;
}

void* subghz_protocol_decoder_kia_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV1* instance = malloc(sizeof(SubGhzProtocolDecoderKiaV1));
    instance->base.protocol = &subghz_protocol_kia_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_v1_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    instance->decoder.parser_step = KiaV1DecoderStepReset;
}

void subghz_protocol_decoder_kia_v1_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV1* instance = context;

    ManchesterEvent event = ManchesterEventReset;

    switch(instance->decoder.parser_step) {
    case KiaV1DecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_long) <
                       subghz_protocol_kia_v1_const.te_delta)) {
            instance->decoder.parser_step = KiaV1DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);
        }
        break;

    case KiaV1DecoderStepCheckPreamble:
        if(!level) {
            if((DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_long) <
                subghz_protocol_kia_v1_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v1_const.te_long) <
                subghz_protocol_kia_v1_const.te_delta)) {
                instance->header_count++;
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV1DecoderStepReset;
            }
        }
        if(instance->header_count > 70) {
            if((!level) &&
               (DURATION_DIFF(duration, subghz_protocol_kia_v1_const.te_short) <
                subghz_protocol_kia_v1_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_v1_const.te_long) <
                subghz_protocol_kia_v1_const.te_delta)) {
                instance->decoder.decode_count_bit = 1;
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->header_count = 0;
                instance->decoder.parser_step = KiaV1DecoderStepDecodeData;
            }
        }
        break;

    case KiaV1DecoderStepDecodeData:
        event = kia_v1_manchester_event(duration, level, &subghz_protocol_kia_v1_const);

        if(event != ManchesterEventReset) {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);
            if(data_ok) {
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | data;
                instance->decoder.decode_count_bit++;
            }
        }

        if(instance->decoder.decode_count_bit ==
           subghz_protocol_kia_v1_const.min_count_bit_for_found) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            kia_v1_check_remote_controller(instance);
            if(instance->crc_check && instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);

            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = KiaV1DecoderStepReset;
        }
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV1* instance = context;

    kia_v1_check_remote_controller(instance);

    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV1* instance = context;
    flipper_format_rewind(flipper_format);
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_kia_v1_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    uint32_t serial = 0;
    uint32_t btn = 0;
    uint32_t cnt = 0;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Serial", &serial, 1)) {
        instance->generic.serial = serial;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Btn", &btn, 1)) {
        instance->generic.btn = (uint8_t)btn;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt, 1)) {
        instance->generic.cnt = (uint16_t)cnt;
    }

    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_kia_v1_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderKiaV1* instance = context;
    kia_v1_get_string_setup_editor(instance->generic.btn, instance->generic.cnt, 4, 16, 5);

    kia_v1_check_remote_controller(instance);

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key:%08lX%08lX\r\n",
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFFU));
    {
        const char* btn_name = kia_v1_get_button_name(instance->generic.btn);
        if(btn_name && btn_name[0] != '\0') {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X [%s]\r\n",
                8,
                (unsigned long)instance->generic.serial,
                instance->generic.btn,
                btn_name);
        } else {
            furi_string_cat_printf(
                output,
                "Sn:%0*lX Btn:%02X\r\n",
                8,
                (unsigned long)instance->generic.serial,
                instance->generic.btn);
        }
    }
    furi_string_cat_printf(output, "Cnt:%03lX ", instance->generic.cnt);
    furi_string_cat_printf(
        output, "CRC:%02X [%s]", instance->crc, instance->crc_check ? "OK" : "ERR");
}
