#include <lib/subghz/registry.h>
#include <lib/subghz/blocks/generic.h>
#include "fiat_v0.h"
#include <lib/toolbox/manchester_decoder.h>
#include <inttypes.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t
    fiat_v0_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void fiat_v0_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t fiat_v0_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void
    fiat_v0_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static SubGhzProtocolStatus
    fiat_v0_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void fiat_v0_encoder_read_fields(
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

static uint32_t fiat_v0_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    fiat_v0_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static SubGhzProtocolStatus fiat_v0_encoder_read_bit(
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

#pragma GCC diagnostic pop

#define TAG                     "FiatProtocolV0"
#define FIAT_V0_PREAMBLE_PAIRS  150
#define FIAT_V0_GAP_US          800
#define FIAT_V0_TOTAL_BURSTS    3
#define FIAT_V0_INTER_BURST_GAP 25000
#define FIAT_V0_UPLOAD_CAPACITY 1328U
static const SubGhzBlockConst subghz_protocol_fiat_v0_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 100,
    .min_count_bit_for_found = 64,
};

struct SubGhzProtocolDecoderFiatV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint16_t preamble_count;
    uint32_t data_low;
    uint32_t data_high;
    uint8_t bit_count;
    uint32_t hop;
    uint32_t fix;
    uint8_t endbyte;
};

typedef enum {
    FiatV0DecoderStepReset = 0,
    FiatV0DecoderStepPreamble = 1,
    FiatV0DecoderStepData = 2,
} FiatV0DecoderStep;

static const char* fiat_v0_get_button_name(uint8_t endbyte) {
    const uint8_t low_nibble = endbyte & 0x0FU;
    if((low_nibble >= 0x04U) && (low_nibble <= 0x07U)) {
        return "Lock";
    }
    if((low_nibble >= 0x08U) && (low_nibble <= 0x0BU)) {
        return "Unlock";
    }
    return "??";
}

static void fiat_v0_finish_packet(struct SubGhzProtocolDecoderFiatV0* instance) {
    instance->generic.data = ((uint64_t)instance->hop << 32) | instance->fix;
    instance->generic.data_count_bit = 71;
    instance->generic.serial = instance->fix;
    instance->generic.btn = instance->endbyte;
    instance->generic.cnt = instance->hop;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = instance->generic.data_count_bit;
    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->decoder.parser_step = FiatV0DecoderStepReset;
}

struct SubGhzProtocolEncoderFiatV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t hop;
    uint32_t fix;
    uint8_t endbyte;
};

void subghz_protocol_encoder_fiat_v0_free(void* context);
void subghz_protocol_encoder_fiat_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_fiat_v0_yield(void* context);

const SubGhzProtocolDecoder subghz_protocol_fiat_v0_decoder = {
    .alloc = subghz_protocol_decoder_fiat_v0_alloc,
    .free = subghz_protocol_decoder_fiat_v0_free,
    .feed = subghz_protocol_decoder_fiat_v0_feed,
    .reset = subghz_protocol_decoder_fiat_v0_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_v0_serialize,
    .deserialize = subghz_protocol_decoder_fiat_v0_deserialize,
    .get_string = subghz_protocol_decoder_fiat_v0_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_v0_encoder = {
    .alloc = subghz_protocol_encoder_fiat_v0_alloc,
    .free = subghz_protocol_encoder_fiat_v0_free,
    .deserialize = subghz_protocol_encoder_fiat_v0_deserialize,
    .stop = subghz_protocol_encoder_fiat_v0_stop,
    .yield = subghz_protocol_encoder_fiat_v0_yield,
};

const SubGhzProtocol subghz_protocol_fiat_v0 = {
    .name = SUBGHZ_PROTOCOL_FIAT_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_fiat_v0_decoder,
    .encoder = &subghz_protocol_fiat_v0_encoder,
};

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================
void* subghz_protocol_encoder_fiat_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFiatV0* instance = calloc(1, sizeof(SubGhzProtocolEncoderFiatV0));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_fiat_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 0;
    instance->encoder.upload = NULL;
    instance->encoder.is_running = false;

    return instance;
}

static void subghz_protocol_encoder_fiat_v0_get_upload(SubGhzProtocolEncoderFiatV0* instance) {
    furi_check(instance);
    LevelDuration* up = instance->encoder.upload;
    if(up == NULL) return;

    size_t index = 0;
    const size_t cap = FIAT_V0_UPLOAD_CAPACITY;
    uint32_t te_short = subghz_protocol_fiat_v0_const.te_short;
    uint32_t te_long = subghz_protocol_fiat_v0_const.te_long;

    uint64_t data = ((uint64_t)instance->hop << 32) | instance->fix;
    uint8_t endbyte_to_send = instance->endbyte >> 1;

    FURI_LOG_I(
        TAG,
        "Building upload: hop=0x%08lX fix=0x%08lX endbyte=0x%02X send6=0x%02X",
        instance->hop,
        instance->fix,
        instance->endbyte,
        endbyte_to_send);

    for(uint8_t burst = 0; burst < FIAT_V0_TOTAL_BURSTS; burst++) {
        if(burst > 0) {
            index =
                (index < cap ?
                     (index++,
                      up[index - 1] = level_duration_make(false, FIAT_V0_INTER_BURST_GAP),
                      index) :
                     index);
            furi_check(index <= cap);
        }

        for(int i = 0; i < FIAT_V0_PREAMBLE_PAIRS; i++) {
            index =
                (index < cap ?
                     (index++, up[index - 1] = level_duration_make(true, te_short), index) :
                     index);
            index =
                (index < cap ?
                     (index++, up[index - 1] = level_duration_make(false, te_short), index) :
                     index);
        }
        if(index > 0) up[index - 1] = level_duration_make(false, FIAT_V0_GAP_US);

        bool first_bit = (data >> 63) & 1;
        if(first_bit) {
            index =
                (index < cap ?
                     (index++, up[index - 1] = level_duration_make(true, te_long), index) :
                     index);
        } else {
            index =
                (index < cap ?
                     (index++, up[index - 1] = level_duration_make(true, te_short), index) :
                     index);
            index =
                (index < cap ?
                     (index++, up[index - 1] = level_duration_make(false, te_long), index) :
                     index);
        }
        bool prev_bit = first_bit;

        for(int bit = 62; bit >= 0; bit--) {
            bool curr_bit = (data >> bit) & 1;
            if(!prev_bit && !curr_bit) {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(true, te_short), index) :
                         index);
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(false, te_short), index) :
                         index);
            } else if(!prev_bit && curr_bit) {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(true, te_long), index) :
                         index);
            } else if(prev_bit && !curr_bit) {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(false, te_long), index) :
                         index);
            } else {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(false, te_short), index) :
                         index);
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(true, te_short), index) :
                         index);
            }
            prev_bit = curr_bit;
            furi_check(index <= cap);
        }

        for(int bit = 5; bit >= 0; bit--) {
            bool curr_bit = (endbyte_to_send >> bit) & 1;
            if(!prev_bit && !curr_bit) {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(true, te_short), index) :
                         index);
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(false, te_short), index) :
                         index);
            } else if(!prev_bit && curr_bit) {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(true, te_long), index) :
                         index);
            } else if(prev_bit && !curr_bit) {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(false, te_long), index) :
                         index);
            } else {
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(false, te_short), index) :
                         index);
                index =
                    (index < cap ?
                         (index++, up[index - 1] = level_duration_make(true, te_short), index) :
                         index);
            }
            prev_bit = curr_bit;
            furi_check(index <= cap);
        }

        if(prev_bit) {
            index =
                (index < cap ?
                     (index++, up[index - 1] = level_duration_make(false, te_short), index) :
                     index);
        }
        index =
            (index < cap ?
                 (index++, up[index - 1] = level_duration_make(false, te_short * 8), index) :
                 index);
        furi_check(index <= cap);
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;

    FURI_LOG_I(TAG, "Upload built: %zu elements", instance->encoder.size_upload);
}

void subghz_protocol_encoder_fiat_v0_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_fiat_v0_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV0* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_fiat_v0_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV0* instance = context;
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
    subghz_protocol_encoder_fiat_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFiatV0* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(fiat_v0_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    static const uint16_t allowed_bits[] = {64U, 71U};
    uint32_t bit_count = 0;
    if(fiat_v0_encoder_read_bit(flipper_format, allowed_bits, 2, &bit_count) !=
       SubGhzProtocolStatusOk) {
        instance->generic.data_count_bit = 71; // legacy default for garbage Bit values
    } else {
        instance->generic.data_count_bit = bit_count;
    }

    uint64_t key = 0;
    if(!subghz_block_generic_read_key_u64(flipper_format, &key)) {
        return SubGhzProtocolStatusError;
    }
    instance->generic.data = key;
    instance->hop = (uint32_t)(key >> 32);
    instance->fix = (uint32_t)(key & 0xFFFFFFFFU);

    uint32_t eb_read = 0;
    flipper_format_rewind(flipper_format);
    bool have_endbyte = flipper_format_read_uint32(flipper_format, "EndByte", &eb_read, 1);
    if(!have_endbyte) {
        flipper_format_rewind(flipper_format);
        have_endbyte = flipper_format_read_uint32(flipper_format, "CRC", &eb_read, 1);
    }

    uint32_t btn_u32 = 0;
    flipper_format_rewind(flipper_format);
    fiat_v0_encoder_read_fields(flipper_format, NULL, &btn_u32, NULL, NULL);

    instance->endbyte = have_endbyte ? (uint8_t)(eb_read & 0x7FU) : (uint8_t)(btn_u32 & 0x7FU);

    instance->generic.btn = instance->endbyte;
    instance->generic.cnt = instance->hop;
    instance->generic.serial = instance->fix;

    instance->encoder.repeat = fiat_v0_encoder_read_repeat(flipper_format, 3);

    if(!instance->encoder.upload || instance->encoder.size_upload < FIAT_V0_UPLOAD_CAPACITY) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(FIAT_V0_UPLOAD_CAPACITY * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = FIAT_V0_UPLOAD_CAPACITY;
    subghz_protocol_encoder_fiat_v0_get_upload(instance);

    flipper_format_rewind(flipper_format);
    char key_str[20];
    snprintf(key_str, sizeof(key_str), "%08lX%08lX", instance->hop, instance->fix);
    flipper_format_insert_or_update_string_cstr(flipper_format, "Key", key_str);
    uint32_t serial_w = instance->fix;
    flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
    uint32_t btn_w = instance->endbyte;
    flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
    uint32_t cnt_w = instance->hop;
    flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    uint32_t endbyte_w = instance->endbyte;
    flipper_format_insert_or_update_uint32(flipper_format, "EndByte", &endbyte_w, 1);

    instance->encoder.is_running = true;

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_fiat_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFiatV0* instance = calloc(1, sizeof(SubGhzProtocolDecoderFiatV0));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_fiat_v0_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_fiat_v0_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    instance->decoder.parser_step = FiatV0DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->hop = 0;
    instance->fix = 0;
    instance->endbyte = 0;
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_fiat_v0_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV0* instance = context;

    uint32_t te_short = (uint32_t)subghz_protocol_fiat_v0_const.te_short;
    uint32_t te_long = (uint32_t)subghz_protocol_fiat_v0_const.te_long;
    uint32_t te_delta = (uint32_t)subghz_protocol_fiat_v0_const.te_delta;
    uint32_t gap_threshold = FIAT_V0_GAP_US;
    uint32_t diff;

    switch(instance->decoder.parser_step) {
    case FiatV0DecoderStepReset:
        if(!level) return;
        if(duration < te_short) {
            diff = te_short - duration;
        } else {
            diff = duration - te_short;
        }
        if(diff < te_delta) {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->decoder.parser_step = FiatV0DecoderStepPreamble;
            instance->preamble_count = 0;
            instance->bit_count = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case FiatV0DecoderStepPreamble:
        if(level) {
            if(duration < te_short) {
                diff = te_short - duration;
            } else {
                diff = duration - te_short;
            }
            if(diff < te_delta) {
                instance->preamble_count++;
            } else {
                instance->decoder.parser_step = FiatV0DecoderStepReset;
            }
            return;
        }

        if(duration < te_short) {
            diff = te_short - duration;
        } else {
            diff = duration - te_short;
        }

        if(diff < te_delta) {
            instance->preamble_count++;
        } else {
            if(instance->preamble_count >= FIAT_V0_PREAMBLE_PAIRS) {
                if(duration < gap_threshold) {
                    diff = gap_threshold - duration;
                } else {
                    diff = duration - gap_threshold;
                }
                if(diff < te_delta) {
                    instance->decoder.parser_step = FiatV0DecoderStepData;
                    instance->preamble_count = 0;
                    instance->data_low = 0;
                    instance->data_high = 0;
                    instance->bit_count = 0;
                    manchester_advance(
                        instance->manchester_state,
                        ManchesterEventReset,
                        &instance->manchester_state,
                        NULL);
                    return;
                }
            }
            instance->decoder.parser_step = FiatV0DecoderStepReset;
        }

        if(instance->preamble_count >= FIAT_V0_PREAMBLE_PAIRS &&
           instance->decoder.parser_step == FiatV0DecoderStepPreamble) {
            if(duration < gap_threshold) {
                diff = gap_threshold - duration;
            } else {
                diff = duration - gap_threshold;
            }
            if(diff < te_delta) {
                instance->decoder.parser_step = FiatV0DecoderStepData;
                instance->preamble_count = 0;
                instance->data_low = 0;
                instance->data_high = 0;
                instance->bit_count = 0;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                return;
            }
        }
        break;

    case FiatV0DecoderStepData: {
        ManchesterEvent event = ManchesterEventReset;
        if(duration < te_short) {
            diff = te_short - duration;
            if(diff < te_delta) {
                event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            }
        } else {
            diff = duration - te_short;
            if(diff < te_delta) {
                event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            } else {
                if(duration < te_long) {
                    diff = te_long - duration;
                } else {
                    diff = duration - te_long;
                }
                if(diff < te_delta) {
                    event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
                }
            }
        }

        if(event != ManchesterEventReset) {
            bool data_bit_bool;
            if(manchester_advance(
                   instance->manchester_state,
                   event,
                   &instance->manchester_state,
                   &data_bit_bool)) {
                uint32_t new_bit = data_bit_bool ? 1 : 0;
                uint32_t carry = (instance->data_low >> 31) & 1;
                instance->data_low = (instance->data_low << 1) | new_bit;
                instance->data_high = (instance->data_high << 1) | carry;
                instance->bit_count++;

                if(instance->bit_count == 64) {
                    instance->fix = instance->data_low;
                    instance->hop = instance->data_high;
                    instance->data_low = 0;
                    instance->data_high = 0;
                }
                if(instance->bit_count == 0x47) {
                    instance->endbyte = (uint8_t)(instance->data_low & 0x3F);
                    fiat_v0_finish_packet(instance);
                }
            }
        } else {
            if(instance->bit_count == 0x47) {
                instance->endbyte = (uint8_t)(instance->data_low & 0x3F);
                fiat_v0_finish_packet(instance);
            } else if(instance->bit_count < 64) {
                instance->decoder.parser_step = FiatV0DecoderStepReset;
            }
        }
        break;
    }
    default:
        break;
    }
}

uint8_t subghz_protocol_decoder_fiat_v0_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV0* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        if(!subghz_block_generic_serialize_preset(flipper_format, preset)) break;

        if(!flipper_format_write_string_cstr(
               flipper_format, "Protocol", instance->generic.protocol_name))
            break;

        uint32_t bits = instance->generic.data_count_bit;
        if(!flipper_format_write_uint32(flipper_format, "Bit", &bits, 1)) break;

        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%08lX%08lX", instance->hop, instance->fix);
        if(!flipper_format_write_string_cstr(flipper_format, "Key", key_str)) break;

        uint32_t serial_w = instance->fix;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->endbyte;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->hop;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);

        uint32_t endbyte_ff = instance->endbyte;
        if(!flipper_format_write_uint32(flipper_format, "EndByte", &endbyte_ff, 1)) break;
        uint32_t crc_ff = instance->endbyte & 0x3FU;
        flipper_format_insert_or_update_uint32(flipper_format, "CRC", &crc_ff, 1);
        uint32_t btn_disp = instance->endbyte;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_disp, 1);

        ret = fiat_v0_write_display(
            flipper_format,
            instance->generic.protocol_name,
            fiat_v0_get_button_name(instance->endbyte));
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFiatV0* instance = context;

    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(status != SubGhzProtocolStatusOk) {
        return status;
    }

    if(instance->generic.data_count_bit != 64 && instance->generic.data_count_bit != 71) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    instance->hop = (uint32_t)(instance->generic.data >> 32U);
    instance->fix = (uint32_t)(instance->generic.data & 0xFFFFFFFFU);
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = instance->generic.data_count_bit;

    uint32_t endbyte_u32 = 0U;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "EndByte", &endbyte_u32, 1)) {
        instance->endbyte = (uint8_t)(endbyte_u32 & 0x7FU);
    } else {
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "CRC", &endbyte_u32, 1)) {
            instance->endbyte = (uint8_t)(endbyte_u32 & 0x7FU);
        } else {
            fiat_v0_encoder_read_fields(flipper_format, NULL, &endbyte_u32, NULL, NULL);
            instance->endbyte = (uint8_t)(endbyte_u32 & 0x7FU);
        }
    }

    instance->generic.serial = instance->fix;
    instance->generic.btn = instance->endbyte;
    instance->generic.cnt = instance->hop;

    return status;
}

void subghz_protocol_decoder_fiat_v0_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderFiatV0* instance = context;

    subghz_block_generic_global.btn_is_available = false;
    subghz_block_generic_global.btn_length_bit = 6;
    subghz_block_generic_global.current_btn = instance->endbyte & 0x3FU;
    subghz_block_generic_global.cnt_is_available = false;
    subghz_block_generic_global.cnt_length_bit = 32;
    subghz_block_generic_global.current_cnt = instance->hop;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Hop:%08lX\r\n"
        "Sn:%08lX Btn:%02X [%s]\r\n"
        "CRC:%02X EndByte:%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)instance->hop,
        (unsigned long)instance->fix,
        (unsigned long)instance->hop,
        (unsigned long)instance->fix,
        instance->endbyte & 0x3F,
        fiat_v0_get_button_name(instance->endbyte),
        instance->endbyte & 0x3F,
        instance->endbyte);
}
