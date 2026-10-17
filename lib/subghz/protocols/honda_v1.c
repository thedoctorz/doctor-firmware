#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/registry.h>
#include "honda_v1.h"
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t
    honda_v1_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void honda_v1_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t honda_v1_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void
    honda_v1_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static SubGhzProtocolStatus
    honda_v1_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void honda_v1_encoder_read_fields(
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

static uint32_t honda_v1_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    honda_v1_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static SubGhzProtocolStatus honda_v1_encoder_read_bit(
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

static void honda_v1_get_string_setup_editor(
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

#define HONDA_V1_BIT_COUNT             68
#define HONDA_V1_TE_SHORT              1000
#define HONDA_V1_TE_LONG               2000
#define HONDA_V1_TE_DELTA              400
#define HONDA_V1_TE_SHORT_MIN          600
#define HONDA_V1_TE_END                3500
#define HONDA_V1_VALID_MAX             0x4B
#define HONDA_V1_NIBBLE_MASK           0x0FU
#define HONDA_V1_SERIAL_MASK           0x0FFFFFFFU
#define HONDA_V1_COUNTER_MASK          0xFFFFU
#define HONDA_V1_LOW32_MASK            0xFFFFFFFFULL
#define HONDA_V1_BUTTON_MAX            10U
#define HONDA_V1_BUTTON_VALID_MASK     0x701U
#define HONDA_V1_BUTTON_FALLBACK_CODE  0x00088888U
#define HONDA_V1_UPLOAD_CAPACITY       2048U
#define HONDA_V1_PREAMBLE_UPLOAD_COUNT 180U
#define HONDA_V1_FRAME_SYMBOLS         80U
#define HONDA_V1_FRAME_START           12U
#define HONDA_V1_FRAME_SYNC_DROP       2U
#define HONDA_V1_FRAME_REPEAT_PER_CRC  2U
#define HONDA_V1_FRAME_BYTES           9U
#define HONDA_V1_FRAME_CRC_INDEX       8U
#define HONDA_V1_FRAME_GAP_US          5000U
#define HONDA_V1_FRAME_GENERATED_MAX   (HONDA_V1_FRAME_SYMBOLS * 2U)
#define HONDA_V1_FRAME_TAIL_MAX        3U
#define HONDA_V1_DECODE_BUFFER_BYTES   12U
#define HONDA_V1_KEY_BYTES             8U
#define HONDA_V1_CRC_FIELD             "Crc"
#define HONDA_V1_KEY_2_FIELD           "Key_2"

_Static_assert(
    HONDA_V1_PREAMBLE_UPLOAD_COUNT < HONDA_V1_UPLOAD_CAPACITY,
    "HONDA_V1 preamble exceeds upload slab");
_Static_assert(
    HONDA_V1_FRAME_SYNC_DROP < HONDA_V1_FRAME_GENERATED_MAX,
    "HONDA_V1 frame sync drop exceeds generated frame");

typedef enum {
    HondaV1DecoderStepReset = 0,
    HondaV1DecoderStepPreamble,
    HondaV1DecoderStepData,
} HondaV1DecoderStep;

typedef enum {
    HondaV1ButtonUnlock = 0,
    HondaV1ButtonLock = 8,
    HondaV1ButtonTrunk = 9,
    HondaV1ButtonPanic = 10,
} HondaV1Button;

static const char* const honda_v1_button_names[HONDA_V1_BUTTON_MAX + 1U] = {
    [HondaV1ButtonUnlock] = "Unlock",
    [HondaV1ButtonLock] = "Lock",
    [HondaV1ButtonTrunk] = "Trunk",
    [HondaV1ButtonPanic] = "Panic",
};

static const uint32_t honda_v1_button_codes[HONDA_V1_BUTTON_MAX + 1U] = {
    [HondaV1ButtonUnlock] = 0x00080808,
    [HondaV1ButtonLock] = 0x00088888,
    [HondaV1ButtonTrunk] = 0x00099190,
    [HondaV1ButtonPanic] = 0x000FA7A0,
};

struct SubGhzProtocolDecoderHondaV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockGeneric generic;

    uint8_t step;
    uint8_t preamble_count;
    bool preamble_has_long;
    bool data_pending;
    bool last_level;
    uint8_t bits[HONDA_V1_DECODE_BUFFER_BYTES];
    uint8_t bit_count;
    uint32_t pending;
    bool pending_valid;
    uint8_t k2;
};

struct SubGhzProtocolEncoderHondaV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t k2;
};

static bool honda_v1_button_valid(uint8_t b) {
    if(b > HONDA_V1_BUTTON_MAX) return false;
    return ((HONDA_V1_BUTTON_VALID_MASK >> b) & 1U) != 0U;
}

static const char* honda_v1_button_name(uint8_t b) {
    if((b < COUNT_OF(honda_v1_button_names)) && (honda_v1_button_names[b] != NULL)) {
        return honda_v1_button_names[b];
    }
    return "Unknown";
}

static uint32_t honda_v1_button_code(uint8_t button) {
    if(!honda_v1_button_valid(button)) {
        return HONDA_V1_BUTTON_FALLBACK_CODE;
    }
    return honda_v1_button_codes[button];
}

static bool honda_v1_duration_is(uint32_t d, uint32_t t) {
    return (d >= t) ? ((d - t) <= HONDA_V1_TE_DELTA) : ((t - d) <= HONDA_V1_TE_DELTA);
}

static uint8_t honda_v1_crc_fold(uint16_t v) {
    const uint8_t lo = (uint8_t)(v & HONDA_V1_NIBBLE_MASK);
    const uint16_t hi = (uint16_t)(v >> 4U);
    int32_t s = (hi & 1U) ? (int32_t)lo : -(int32_t)lo;
    uint8_t out = (uint8_t)((s - (int32_t)hi) & 7);
    out |= (uint8_t)(((v >> 3U) & 1U) << 3U);
    if(((v >> 1U) & 1U) && (((v >> 4U) ^ (v >> 5U)) & 1U)) {
        out ^= 0x04U;
    }
    return (uint8_t)(out & HONDA_V1_NIBBLE_MASK);
}

static uint8_t honda_v1_checksum_base(uint64_t data) {
    const uint8_t a = honda_v1_crc_fold((uint16_t)(data & HONDA_V1_COUNTER_MASK));
    const uint8_t b = honda_v1_crc_fold((uint8_t)((data >> 40U) & 0xFFU));
    return (uint8_t)((a ^ b ^ 1U) & HONDA_V1_NIBBLE_MASK);
}

static uint8_t honda_v1_checksum_alternate(uint8_t checksum) {
    uint8_t mask = 0x09U;
    if((checksum & 1U) == 0U) {
        mask = (checksum & 2U) ? 0x0BU : HONDA_V1_NIBBLE_MASK;
    }
    return (uint8_t)((checksum ^ mask) & HONDA_V1_NIBBLE_MASK);
}

static void honda_v1_checksum_wire_order(uint64_t data, uint8_t* first, uint8_t* second) {
    const uint8_t checksum = honda_v1_checksum_base(data);
    const uint8_t other = honda_v1_checksum_alternate(checksum);
    if((checksum & 0x08U) != 0U) {
        *first = other;
        *second = checksum;
    } else {
        *first = checksum;
        *second = other;
    }
}

static bool honda_v1_crc_valid(uint64_t data, uint8_t crc) {
    uint8_t first = 0U;
    uint8_t second = 0U;
    honda_v1_checksum_wire_order(data, &first, &second);
    crc &= HONDA_V1_NIBBLE_MASK;
    return (crc == first) || (crc == second);
}

static void honda_v1_decode_fields(SubGhzBlockGeneric* generic) {
    const uint32_t low = (uint32_t)(generic->data & HONDA_V1_LOW32_MASK);

    generic->serial = (uint32_t)((generic->data >> 36U) & HONDA_V1_SERIAL_MASK);
    generic->btn = (uint8_t)((low >> 28U) & HONDA_V1_NIBBLE_MASK);
    generic->cnt = low & HONDA_V1_COUNTER_MASK;
    generic->data_count_bit = HONDA_V1_BIT_COUNT;
}

static uint64_t honda_v1_build_key(uint32_t serial, uint8_t button, uint16_t counter) {
    const uint32_t table = honda_v1_button_code(button);
    const uint32_t low = ((table & HONDA_V1_COUNTER_MASK) << 16U) | counter;
    const uint32_t high = ((serial & HONDA_V1_SERIAL_MASK) << 4U) | (table >> 16U);

    return ((uint64_t)high << 32U) | low;
}

static void honda_v1_state_reset(SubGhzProtocolDecoderHondaV1* instance) {
    instance->step = HondaV1DecoderStepReset;
    instance->preamble_count = 0U;
    instance->preamble_has_long = false;
    instance->data_pending = false;
    instance->last_level = false;
    instance->bit_count = 0U;
    memset(instance->bits, 0, sizeof(instance->bits));
}

static void honda_v1_add_bit(SubGhzProtocolDecoderHondaV1* instance, bool bit) {
    if(instance->bit_count > HONDA_V1_VALID_MAX) return;
    if(bit) {
        instance->bits[instance->bit_count >> 3U] |=
            (uint8_t)(1U << (((uint8_t)~instance->bit_count) & 0x07U));
    }
    instance->bit_count++;
}

static bool honda_v1_commit(SubGhzProtocolDecoderHondaV1* instance) {
    if(instance->bit_count < HONDA_V1_BIT_COUNT) return false;

    uint8_t aligned[sizeof(instance->bits)];
    memcpy(aligned, instance->bits, sizeof(aligned));

    uint8_t shift_count = instance->bit_count - HONDA_V1_BIT_COUNT;
    if(shift_count < 1U) shift_count = 1U;

    for(uint8_t shift = 0U; shift < shift_count; shift++) {
        for(size_t i = 0; i < sizeof(aligned) - 1U; i++) {
            aligned[i] = (uint8_t)((aligned[i] << 1U) | (aligned[i + 1U] >> 7U));
        }
        aligned[sizeof(aligned) - 1U] <<= 1U;
    }

    const uint8_t button = (uint8_t)(aligned[4] >> 4U);
    if(!honda_v1_button_valid(button)) return false;

    instance->generic.data = honda_v1_bytes_to_u64_be(aligned);
    instance->k2 = (uint8_t)(aligned[8] >> 4U);
    honda_v1_decode_fields(&instance->generic);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }

    return true;
}

static void
    honda_v1_symbol(SubGhzProtocolDecoderHondaV1* instance, bool level, uint32_t duration) {
    const bool sh = honda_v1_duration_is(duration, HONDA_V1_TE_SHORT);
    const bool lg = honda_v1_duration_is(duration, HONDA_V1_TE_LONG);

    if(!sh && !lg) {
        if(!level && (duration > HONDA_V1_TE_END) && (instance->step == HondaV1DecoderStepData)) {
            honda_v1_commit(instance);
        }
        honda_v1_state_reset(instance);
        return;
    }

    if(instance->step == HondaV1DecoderStepReset) {
        if(level) {
            instance->step = HondaV1DecoderStepPreamble;
            instance->preamble_count = 1U;
            instance->last_level = level;
        }
        return;
    }

    if(instance->step == HondaV1DecoderStepPreamble) {
        if(lg) {
            if(instance->preamble_count < 0xFFU) instance->preamble_count++;
            instance->preamble_has_long = true;
            instance->last_level = level;
            return;
        }

        if(sh) {
            if(instance->preamble_has_long && (instance->preamble_count > 5U)) {
                instance->step = HondaV1DecoderStepData;
                instance->bit_count = 0U;
                memset(instance->bits, 0, sizeof(instance->bits));
                instance->data_pending = true;
                instance->last_level = level;
                return;
            }

            if(instance->preamble_count < 0xFFU) instance->preamble_count++;
            instance->last_level = level;
            return;
        }

        honda_v1_state_reset(instance);
        return;
    }

    if(sh) {
        if(instance->data_pending) {
            honda_v1_add_bit(instance, level);
            instance->data_pending = false;
            instance->last_level = level;
            return;
        }

        instance->data_pending = true;
        instance->last_level = level;
    } else {
        if(instance->data_pending) {
            honda_v1_add_bit(instance, level);
        } else {
            honda_v1_add_bit(instance, instance->last_level);
        }

        instance->last_level = level;
    }
}

static bool honda_v1_append_frame(
    SubGhzProtocolEncoderHondaV1* instance,
    size_t* index,
    const uint8_t frame[HONDA_V1_FRAME_BYTES]) {
    LevelDuration* upload = instance->encoder.upload;
    LevelDuration generated[HONDA_V1_FRAME_GENERATED_MAX];
    size_t generated_count = 0U;

    for(uint32_t bit_index = 0U; bit_index < HONDA_V1_FRAME_SYMBOLS; bit_index++) {
        uint32_t bit;

        if(bit_index >= HONDA_V1_FRAME_START) {
            const uint32_t data_index = (bit_index - HONDA_V1_FRAME_START) >> 3U;
            const uint8_t shift = (uint8_t)((11U - bit_index) & 0x07U);
            bit = (frame[data_index] >> shift) & 0x01U;
        } else {
            bit = ((uint32_t)~bit_index) & 0x01U;
        }

        generated_count = honda_v1_emit_merge(
            generated, generated_count, COUNT_OF(generated), bit != 0U, HONDA_V1_TE_SHORT);
        generated_count = honda_v1_emit_merge(
            generated, generated_count, COUNT_OF(generated), bit == 0U, HONDA_V1_TE_SHORT);
    }

    if(generated_count <= HONDA_V1_FRAME_SYNC_DROP) {
        return false;
    }

    const size_t copy_count = generated_count - HONDA_V1_FRAME_SYNC_DROP;
    if((*index + copy_count + HONDA_V1_FRAME_TAIL_MAX) > HONDA_V1_UPLOAD_CAPACITY) {
        return false;
    }

    memcpy(
        &upload[*index], &generated[HONDA_V1_FRAME_SYNC_DROP], copy_count * sizeof(LevelDuration));
    *index += copy_count;

    const bool tail_level = !level_duration_get_level(upload[*index - 1U]);
    if(*index < HONDA_V1_UPLOAD_CAPACITY) {
        upload[(*index)++] = level_duration_make(tail_level, HONDA_V1_TE_SHORT);
    }
    if(!tail_level) {
        if(*index < HONDA_V1_UPLOAD_CAPACITY) {
            upload[(*index)++] = level_duration_make(true, HONDA_V1_TE_SHORT);
        }
    }
    if(*index < HONDA_V1_UPLOAD_CAPACITY) {
        upload[(*index)++] = level_duration_make(false, HONDA_V1_FRAME_GAP_US);
    }
    return true;
}

static bool honda_v1_build_upload(SubGhzProtocolEncoderHondaV1* instance) {
    furi_check(instance);

    LevelDuration* upload = instance->encoder.upload;
    if(upload == NULL) return false;

    uint8_t frame[HONDA_V1_FRAME_BYTES] = {0};
    uint8_t first = 0U;
    uint8_t second = 0U;
    size_t index = 0U;

    index = (({
        size_t _pi = index;
        for(size_t _pp = 0; _pp < HONDA_V1_PREAMBLE_UPLOAD_COUNT / 2U; _pp++) {
            if(_pi < HONDA_V1_UPLOAD_CAPACITY)
                upload[_pi++] = level_duration_make(true, HONDA_V1_TE_SHORT);
            if(_pi < HONDA_V1_UPLOAD_CAPACITY)
                upload[_pi++] = level_duration_make(false, HONDA_V1_TE_SHORT);
        }
        _pi;
    }));
    if(index != HONDA_V1_PREAMBLE_UPLOAD_COUNT) {
        return false;
    }
    upload[index - 1U] = level_duration_make(false, HONDA_V1_FRAME_GAP_US);

    honda_v1_u64_to_bytes_be(instance->generic.data, frame);
    honda_v1_checksum_wire_order(instance->generic.data, &first, &second);

    const uint8_t crc_order[] = {first, second};
    for(size_t crc_index = 0U; crc_index < COUNT_OF(crc_order); crc_index++) {
        frame[HONDA_V1_FRAME_CRC_INDEX] = (uint8_t)(crc_order[crc_index] << 4U);
        for(size_t repeat = 0U; repeat < HONDA_V1_FRAME_REPEAT_PER_CRC; repeat++) {
            if(!honda_v1_append_frame(instance, &index, frame)) {
                return false;
            }
        }
    }

    instance->k2 = second;
    instance->encoder.front = 0U;
    instance->encoder.size_upload = index;
    return true;
}

void subghz_protocol_encoder_honda_v1_free(void* context);
void subghz_protocol_encoder_honda_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_v1_yield(void* context);

void subghz_protocol_decoder_honda_v1_free(void* context);

const SubGhzProtocolDecoder subghz_protocol_honda_v1_decoder = {
    .alloc = subghz_protocol_decoder_honda_v1_alloc,
    .free = subghz_protocol_decoder_honda_v1_free,
    .feed = subghz_protocol_decoder_honda_v1_feed,
    .reset = subghz_protocol_decoder_honda_v1_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_honda_v1_serialize,
    .deserialize = subghz_protocol_decoder_honda_v1_deserialize,
    .get_string = subghz_protocol_decoder_honda_v1_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v1_encoder = {
    .alloc = subghz_protocol_encoder_honda_v1_alloc,
    .free = subghz_protocol_encoder_honda_v1_free,
    .deserialize = subghz_protocol_encoder_honda_v1_deserialize,
    .stop = subghz_protocol_encoder_honda_v1_stop,
    .yield = subghz_protocol_encoder_honda_v1_yield,
};

const SubGhzProtocol subghz_protocol_honda_v1 = {
    .name = SUBGHZ_PROTOCOL_HONDA_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v1_decoder,
    .encoder = &subghz_protocol_honda_v1_encoder,
};

void* subghz_protocol_encoder_honda_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderHondaV1* instance = malloc(sizeof(SubGhzProtocolEncoderHondaV1));
    furi_check(instance);
    memset(instance, 0, sizeof(*instance));

    instance->base.protocol = &subghz_protocol_honda_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 1U;
    if(!instance->encoder.upload || instance->encoder.size_upload < HONDA_V1_UPLOAD_CAPACITY) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(HONDA_V1_UPLOAD_CAPACITY * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = HONDA_V1_UPLOAD_CAPACITY;

    return instance;
}

void subghz_protocol_encoder_honda_v1_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHondaV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_honda_v1_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHondaV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_honda_v1_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHondaV1* instance = context;
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
    subghz_protocol_encoder_honda_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);

    SubGhzProtocolEncoderHondaV1* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0U;

    if(honda_v1_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, HONDA_V1_BIT_COUNT);
    if(status != SubGhzProtocolStatusOk) {
        return status;
    }

    instance->generic.protocol_name = instance->base.protocol->name;
    honda_v1_decode_fields(&instance->generic);

    uint32_t serial = instance->generic.serial;
    uint32_t btn = instance->generic.btn;
    uint32_t cnt = instance->generic.cnt;
    honda_v1_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

    serial &= HONDA_V1_SERIAL_MASK;
    uint8_t button = (uint8_t)(btn & HONDA_V1_NIBBLE_MASK);
    if(!honda_v1_button_valid(button)) {
        button = (uint8_t)instance->generic.btn;
    }
    if(!honda_v1_button_valid(button)) {
        button = HondaV1ButtonUnlock;
    }

    instance->generic.serial = serial;
    instance->generic.btn = button;
    instance->generic.cnt = cnt & HONDA_V1_COUNTER_MASK;
    instance->generic.data_count_bit = HONDA_V1_BIT_COUNT;
    instance->generic.data =
        honda_v1_build_key(instance->generic.serial, instance->generic.btn, instance->generic.cnt);

    uint8_t first = 0U;
    uint8_t second = 0U;
    honda_v1_checksum_wire_order(instance->generic.data, &first, &second);
    instance->k2 = second;

    uint8_t key_data[HONDA_V1_KEY_BYTES];
    honda_v1_u64_to_bytes_be(instance->generic.data, key_data);
    flipper_format_rewind(flipper_format);
    bool key_written =
        flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(key_data));
    if(!key_written) {
        flipper_format_rewind(flipper_format);
        key_written =
            flipper_format_insert_or_update_hex(flipper_format, "Key", key_data, sizeof(key_data));
    }
    if(!key_written) {
        return SubGhzProtocolStatusErrorParserKey;
    }

    honda_v1_flipper_update_or_insert_u32(flipper_format, "Serial", instance->generic.serial);
    honda_v1_flipper_update_or_insert_u32(flipper_format, "Btn", instance->generic.btn);
    honda_v1_flipper_update_or_insert_u32(flipper_format, "Cnt", instance->generic.cnt);
    honda_v1_flipper_update_or_insert_u32(flipper_format, HONDA_V1_CRC_FIELD, instance->k2);
    honda_v1_flipper_update_or_insert_u32(flipper_format, HONDA_V1_KEY_2_FIELD, instance->k2);

    instance->encoder.repeat = honda_v1_encoder_read_repeat(flipper_format, 1U);
    if(!honda_v1_build_upload(instance)) {
        return SubGhzProtocolStatusErrorEncoderGetUpload;
    }

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void* subghz_protocol_decoder_honda_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderHondaV1* instance = malloc(sizeof(SubGhzProtocolDecoderHondaV1));
    furi_check(instance);
    memset(instance, 0, sizeof(*instance));

    instance->base.protocol = &subghz_protocol_honda_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_honda_v1_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_honda_v1_reset(void* context) {
    furi_check(context);

    SubGhzProtocolDecoderHondaV1* instance = context;
    instance->pending = 0U;
    instance->pending_valid = false;
    honda_v1_state_reset(instance);
}

void subghz_protocol_decoder_honda_v1_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);

    SubGhzProtocolDecoderHondaV1* instance = context;

    if(duration < HONDA_V1_TE_DELTA) {
        instance->pending += duration;
        instance->pending_valid = true;
        return;
    }

    if(instance->pending_valid) {
        const uint32_t p = instance->pending;
        if(level) {
            instance->pending = p + duration;
            instance->pending_valid = true;
            return;
        }
        if(p >= HONDA_V1_TE_SHORT_MIN) honda_v1_symbol(instance, true, p);
        instance->pending = 0U;
        instance->pending_valid = false;
    }

    if(level) {
        instance->pending = duration;
        instance->pending_valid = true;
        return;
    }

    honda_v1_symbol(instance, false, duration);
}

uint8_t subghz_protocol_decoder_honda_v1_get_hash_data(void* context) {
    furi_check(context);

    SubGhzProtocolDecoderHondaV1* instance = context;
    const uint64_t data = instance->generic.data;

    return (uint8_t)(data ^ (data >> 8U) ^ (data >> 16U) ^ (data >> 24U) ^ (data >> 32U) ^
                     (data >> 40U) ^ (data >> 48U) ^ (data >> 56U));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);

    SubGhzProtocolDecoderHondaV1* instance = context;
    honda_v1_decode_fields(&instance->generic);

    SubGhzProtocolStatus status =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(status != SubGhzProtocolStatusOk) {
        return status;
    }

    status = flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_uint32(flipper_format, "Serial", &instance->generic.serial, 1);
    uint32_t btn_w = instance->generic.btn;
    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
    uint32_t cnt_w = instance->generic.cnt;
    flipper_format_rewind(flipper_format);
    flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    { return status; }

    uint32_t crc = instance->k2 & HONDA_V1_NIBBLE_MASK;
    if(!flipper_format_write_uint32(flipper_format, HONDA_V1_CRC_FIELD, &crc, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    if(!flipper_format_write_uint32(flipper_format, HONDA_V1_KEY_2_FIELD, &crc, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    return honda_v1_write_display(
        flipper_format,
        instance->generic.protocol_name,
        honda_v1_button_name(instance->generic.btn));
}

SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);

    SubGhzProtocolDecoderHondaV1* instance = context;
    SubGhzProtocolStatus status = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, HONDA_V1_BIT_COUNT);
    if(status != SubGhzProtocolStatusOk) {
        return status;
    }

    flipper_format_rewind(flipper_format);
    uint32_t crc = 0U;
    bool crc_found = flipper_format_read_uint32(flipper_format, HONDA_V1_KEY_2_FIELD, &crc, 1);
    if(!crc_found) {
        flipper_format_rewind(flipper_format);
        crc_found = flipper_format_read_uint32(flipper_format, HONDA_V1_CRC_FIELD, &crc, 1);
    }
    if(crc_found) {
        instance->k2 = (uint8_t)(crc & HONDA_V1_NIBBLE_MASK);
    } else {
        uint8_t first = 0U;
        uint8_t second = 0U;
        honda_v1_checksum_wire_order(instance->generic.data, &first, &second);
        instance->k2 = first;
    }

    instance->generic.protocol_name = instance->base.protocol->name;
    honda_v1_decode_fields(&instance->generic);

    return status;
}

void subghz_protocol_decoder_honda_v1_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderHondaV1* instance = context;
    honda_v1_decode_fields(&instance->generic);
    honda_v1_get_string_setup_editor(
        (uint8_t)instance->generic.btn, instance->generic.cnt, 4, 16, HONDA_V1_BUTTON_MAX);

    const uint8_t k2 = instance->k2 & HONDA_V1_NIBBLE_MASK;
    const bool crc_ok = honda_v1_crc_valid(instance->generic.data, k2);

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(output, "Key:%016llX\r\n", (unsigned long long)instance->generic.data);
    furi_string_cat_printf(
        output,
        "Sn:%07lX Btn:%02X [%s]\r\n",
        (unsigned long)instance->generic.serial,
        (uint8_t)instance->generic.btn,
        honda_v1_button_name((uint8_t)instance->generic.btn));
    furi_string_cat_printf(output, "Cnt:%04lX ", (unsigned long)instance->generic.cnt);
    furi_string_cat_printf(output, "CRC:%02X [%s]", k2, crc_ok ? "OK" : "ERR");
}
