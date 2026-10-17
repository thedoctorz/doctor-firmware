#include "nord_ice.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "common.h"

#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolNord_Ice"

#define NORD_ICE_COUNT_BIT 33

static const SubGhzBlockConst subghz_protocol_nord_ice_const = {
    .te_short = 270,
    .te_long = 760,
    .te_delta = 150,
    .min_count_bit_for_found = NORD_ICE_COUNT_BIT,
};

#define NORD_ICE_FRAME_FLAG (1ULL << 14)

#define NORD_ICE_GAP_FLAGGED 8980
#define NORD_ICE_GAP_PLAIN   7440
#define NORD_ICE_GAP_DELTA   (subghz_protocol_nord_ice_const.te_delta * 3)

#define NORD_ICE_UPLOAD_SIZE (NORD_ICE_COUNT_BIT * 2 * 2)

#define NORD_ICE_BTN_SHIFT 9
#define NORD_ICE_BTN_MASK  (0xFULL << NORD_ICE_BTN_SHIFT)

static const uint8_t subghz_protocol_nord_ice_btn_order[] = {0x4, 0x8, 0x1, 0x2};

struct SubGhzProtocolDecoderNord_Ice {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};
SUBGHZ_ASSERT_DECODER_COMMON_LAYOUT(SubGhzProtocolDecoderNord_Ice);

struct SubGhzProtocolEncoderNord_Ice {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};
SUBGHZ_ASSERT_ENCODER_GENERIC_LAYOUT(SubGhzProtocolEncoderNord_Ice);

typedef enum {
    Nord_IceDecoderStepReset = 0,
    Nord_IceDecoderStepSaveDuration,
    Nord_IceDecoderStepCheckDuration,
} Nord_IceDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_nord_ice_decoder = {
    .alloc = subghz_protocol_decoder_nord_ice_alloc,
    .free = subghz_protocol_decoder_common_free,

    .feed = subghz_protocol_decoder_nord_ice_feed,
    .reset = subghz_protocol_decoder_common_reset,

    .get_hash_data = subghz_protocol_decoder_common_get_hash_data,
    .serialize = subghz_protocol_decoder_common_serialize,
    .deserialize = subghz_protocol_decoder_nord_ice_deserialize,
    .get_string = subghz_protocol_decoder_nord_ice_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_nord_ice_encoder = {
    .alloc = subghz_protocol_encoder_nord_ice_alloc,
    .free = subghz_protocol_encoder_common_free,

    .deserialize = subghz_protocol_encoder_nord_ice_deserialize,
    .stop = subghz_protocol_encoder_common_stop,
    .yield = subghz_protocol_encoder_common_yield,
};

const SubGhzProtocol subghz_protocol_nord_ice = {
    .name = SUBGHZ_PROTOCOL_NORD_ICE_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_nord_ice_decoder,
    .encoder = &subghz_protocol_nord_ice_encoder,
};

void* subghz_protocol_encoder_nord_ice_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    return subghz_protocol_encoder_common_alloc(
        sizeof(SubGhzProtocolEncoderNord_Ice), &subghz_protocol_nord_ice, 3, NORD_ICE_UPLOAD_SIZE);
}

static void subghz_protocol_nord_ice_check_remote_controller(SubGhzBlockGeneric* instance);

/**
 */
static uint8_t subghz_protocol_nord_ice_get_btn_code(void) {
    const uint8_t custom_btn_id = subghz_custom_btn_get();
    const uint8_t original_btn_code = subghz_custom_btn_get_original();

    uint8_t step = 0;
    if(custom_btn_id == SUBGHZ_CUSTOM_BTN_UP) {
        step = 1;
    } else if(custom_btn_id == SUBGHZ_CUSTOM_BTN_DOWN) {
        step = 2;
    } else if(custom_btn_id == SUBGHZ_CUSTOM_BTN_LEFT) {
        step = 3;
    }
    if(step == 0 || original_btn_code == 0) {
        return original_btn_code;
    }

    for(size_t i = 0; i < COUNT_OF(subghz_protocol_nord_ice_btn_order); i++) {
        if(subghz_protocol_nord_ice_btn_order[i] == original_btn_code) {
            return subghz_protocol_nord_ice_btn_order
                [(i + step) % COUNT_OF(subghz_protocol_nord_ice_btn_order)];
        }
    }
    return original_btn_code;
}

static void subghz_protocol_encoder_nord_ice_add_frame(
    SubGhzProtocolEncoderNord_Ice* instance,
    uint64_t data,
    uint32_t gap,
    size_t* index) {
    size_t index_local = *index;

    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        const bool is_last = (i == 1);
        if(bit_read(data, i - 1)) {
            // Send bit 1
            instance->encoder.upload[index_local++] =
                level_duration_make(true, (uint32_t)subghz_protocol_nord_ice_const.te_long);
            instance->encoder.upload[index_local++] = level_duration_make(
                false, is_last ? gap : (uint32_t)subghz_protocol_nord_ice_const.te_short);
        } else {
            // Send bit 0
            instance->encoder.upload[index_local++] =
                level_duration_make(true, (uint32_t)subghz_protocol_nord_ice_const.te_short);
            instance->encoder.upload[index_local++] = level_duration_make(
                false, is_last ? gap : (uint32_t)subghz_protocol_nord_ice_const.te_long);
        }
    }

    *index = index_local;
}

static bool subghz_protocol_encoder_nord_ice_get_upload(void* context) {
    SubGhzProtocolEncoderNord_Ice* instance = context;
    furi_assert(instance);

    instance->generic.btn = subghz_protocol_nord_ice_get_btn_code();

    if(subghz_block_generic_global_button_override_get(&instance->generic.btn)) {
        FURI_LOG_D(TAG, "Button sucessfully changed to 0x%X", instance->generic.btn);
    }

    instance->generic.data = (instance->generic.data & ~NORD_ICE_BTN_MASK) |
                             ((uint64_t)(instance->generic.btn & 0xF) << NORD_ICE_BTN_SHIFT);

    size_t index = 0;

    subghz_protocol_encoder_nord_ice_add_frame(
        instance, instance->generic.data | NORD_ICE_FRAME_FLAG, NORD_ICE_GAP_FLAGGED, &index);
    subghz_protocol_encoder_nord_ice_add_frame(
        instance, instance->generic.data, NORD_ICE_GAP_PLAIN, &index);

    instance->encoder.size_upload = index;
    return true;
}

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 */
static void subghz_protocol_nord_ice_check_remote_controller(SubGhzBlockGeneric* instance) {
    instance->data &= ~NORD_ICE_FRAME_FLAG;
    instance->serial = (instance->data >> 15) << 9 | (instance->data & 0x1FF);
    instance->btn = (instance->data >> NORD_ICE_BTN_SHIFT) & 0xF;

    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(instance->btn);
    }

    subghz_custom_btn_set_max(3);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_nord_ice_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderNord_Ice* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_nord_ice_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_nord_ice_check_remote_controller(&instance->generic);
        subghz_protocol_encoder_nord_ice_get_upload(instance);

        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Unable to add Key");
            break;
        }

        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void* subghz_protocol_decoder_nord_ice_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    return subghz_protocol_decoder_common_alloc(
        sizeof(SubGhzProtocolDecoderNord_Ice), &subghz_protocol_nord_ice);
}

static bool subghz_protocol_nord_ice_is_gap(uint32_t duration) {
    return (DURATION_DIFF(duration, NORD_ICE_GAP_PLAIN) < NORD_ICE_GAP_DELTA) ||
           (DURATION_DIFF(duration, NORD_ICE_GAP_FLAGGED) < NORD_ICE_GAP_DELTA);
}

void subghz_protocol_decoder_nord_ice_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderNord_Ice* instance = context;

    // Nord ICE Decoder
    // 2026.03 - @xMasterX (MMX)

    // Key samples
    //

    switch(instance->decoder.parser_step) {
    case Nord_IceDecoderStepReset:
        if((!level) && subghz_protocol_nord_ice_is_gap(duration)) {
            //Found GAP
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = Nord_IceDecoderStepSaveDuration;
        }
        break;
    case Nord_IceDecoderStepSaveDuration:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = Nord_IceDecoderStepCheckDuration;
        } else {
            instance->decoder.parser_step = Nord_IceDecoderStepReset;
        }
        break;
    case Nord_IceDecoderStepCheckDuration:
        if(!level) {
            if((DURATION_DIFF(instance->decoder.te_last, subghz_protocol_nord_ice_const.te_short) <
                subghz_protocol_nord_ice_const.te_delta) &&
               (DURATION_DIFF(duration, subghz_protocol_nord_ice_const.te_long) <
                subghz_protocol_nord_ice_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = Nord_IceDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_nord_ice_const.te_long) <
                 subghz_protocol_nord_ice_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_nord_ice_const.te_short) <
                 subghz_protocol_nord_ice_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = Nord_IceDecoderStepSaveDuration;
            } else if(
                // End of the key
                subghz_protocol_nord_ice_is_gap(duration)) {
                if(DURATION_DIFF(
                       instance->decoder.te_last, subghz_protocol_nord_ice_const.te_short) <
                   subghz_protocol_nord_ice_const.te_delta) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                } else if(
                    DURATION_DIFF(
                        instance->decoder.te_last, subghz_protocol_nord_ice_const.te_long) <
                    subghz_protocol_nord_ice_const.te_delta) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                }
                // If got 33 bits key reading is finished
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_nord_ice_const.min_count_bit_for_found) {
                    instance->decoder.decode_data &= ~NORD_ICE_FRAME_FLAG;
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }

                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = Nord_IceDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = Nord_IceDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = Nord_IceDecoderStepReset;
        }
        break;
    }
}

SubGhzProtocolStatus
    subghz_protocol_decoder_nord_ice_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderNord_Ice* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_nord_ice_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_nord_ice_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderNord_Ice* instance = context;

    subghz_protocol_nord_ice_check_remote_controller(&instance->generic);

    uint64_t code_found_reverse = subghz_protocol_blocks_reverse_key(
        instance->generic.data, instance->generic.data_count_bit);

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.btn_length_bit = 4;

    furi_string_cat_printf(
        output,
        "%s %db\r\n"
        "Key: 0x%08llX\r\n"
        "Yek: 0x%08llX\r\n"
        "Serial: 0x%07lX\r\n"
        "Btn: %01X",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint64_t)(instance->generic.data & 0xFFFFFFFFF),
        (code_found_reverse & 0xFFFFFFFFF),
        instance->generic.serial,
        instance->generic.btn);
}
