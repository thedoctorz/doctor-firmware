#include <lib/subghz/blocks/custom_btn_i.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/registry.h>
#include "ford_v0.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static size_t
    ford_v0_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static void ford_v0_u64_to_bytes_be(uint64_t data, uint8_t bytes[8]) {
    for(size_t j = 0; j < 8; j++)
        bytes[j] = (uint8_t)((data >> ((7U - j) * 8U)) & 0xFFU);
}

static uint64_t ford_v0_bytes_to_u64_be(const uint8_t bytes[8]) {
    uint64_t data = 0;
    for(size_t j = 0; j < 8; j++)
        data = (data << 8U) | bytes[j];
    return data;
}

static void
    ford_v0_flipper_update_or_insert_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static SubGhzProtocolStatus
    ford_v0_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
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

static void ford_v0_encoder_read_fields(
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

static uint32_t ford_v0_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    flipper_format_rewind(ff);
    uint32_t tmp = 0;
    return flipper_format_read_uint32(ff, "Repeat", &tmp, 1) ? tmp : default_repeat;
}

static SubGhzProtocolStatus
    ford_v0_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
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

static SubGhzProtocolStatus ford_v0_encoder_read_bit(
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

static void ford_v0_get_string_setup_editor(
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

#define TAG "FordProtocolV0"

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================

// Uncomment to enable bit-level debug logging (WARNING: 80 log calls per signal)
// #define FORD_V0_DEBUG_BITS

static const SubGhzBlockConst subghz_protocol_ford_v0_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 64,
};

#define FORD_V0_PREAMBLE_PAIRS  4
#define FORD_V0_GAP_US          3500
#define FORD_V0_TOTAL_BURSTS    6
#define FORD_V0_UPLOAD_CAPACITY (((FORD_V0_TOTAL_BURSTS - 1U) * 169U) + 168U)
// =============================================================================
// CRC MATRIX
// Ford V0 uses matrix multiplication in GF(2) for CRC calculation
// =============================================================================

static const uint8_t ford_v0_crc_matrix[64] = {
    0xDA, 0xB5, 0x55, 0x6A, 0xAA, 0xAA, 0xAA, 0xD5, 0xB6, 0x6C, 0xCC, 0xD9, 0x99, 0x99, 0x99, 0xB3,
    0x71, 0xE3, 0xC3, 0xC7, 0x87, 0x87, 0x87, 0x8F, 0x0F, 0xE0, 0x3F, 0xC0, 0x7F, 0x80, 0x7F, 0x80,
    0x00, 0x1F, 0xFF, 0xC0, 0x00, 0x7F, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0xFF, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x23, 0x12, 0x94, 0x84, 0x35, 0xF4, 0x55, 0x84,
};

// =============================================================================
// STRUCT DEFINITIONS
// =============================================================================

typedef struct SubGhzProtocolDecoderFordV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;

    uint64_t data_low;
    uint64_t data_high;
    uint8_t bit_count;

    uint16_t header_count;

    uint64_t key1;
    uint16_t key2;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
} SubGhzProtocolDecoderFordV0;
static uint8_t ford_other_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_OK:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x8;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x10;
    default:
        return subghz_custom_btn_get_original();
    }
}

typedef struct SubGhzProtocolEncoderFordV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t key1;
    uint16_t key2;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
    uint8_t checksum;
} SubGhzProtocolEncoderFordV0;
typedef enum {
    FordV0DecoderStepReset = 0,
    FordV0DecoderStepPreamble,
    FordV0DecoderStepPreambleCheck,
    FordV0DecoderStepGap,
    FordV0DecoderStepData,
} FordV0DecoderStep;

// =============================================================================
// FUNCTION PROTOTYPES
// =============================================================================

static void ford_v0_add_bit(SubGhzProtocolDecoderFordV0* instance, bool bit);
static void decode_ford_v0(
    uint64_t key1,
    uint16_t key2,
    uint32_t* serial,
    uint8_t* button,
    uint32_t* count);
static void encode_ford_v0(
    uint8_t header_byte,
    uint32_t serial,
    uint8_t button,
    uint32_t count,
    uint8_t checksum,
    uint64_t* key1);
static bool ford_v0_process_data(SubGhzProtocolDecoderFordV0* instance);

// =============================================================================
// PROTOCOL INTERFACE DEFINITIONS
// =============================================================================

void subghz_protocol_encoder_ford_v0_free(void* context);
void subghz_protocol_encoder_ford_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_ford_v0_yield(void* context);

uint8_t subghz_protocol_decoder_ford_v0_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_ford_v0_decoder = {
    .alloc = subghz_protocol_decoder_ford_v0_alloc,
    .free = subghz_protocol_decoder_ford_v0_free,
    .feed = subghz_protocol_decoder_ford_v0_feed,
    .reset = subghz_protocol_decoder_ford_v0_reset,
    .get_hash_data = subghz_protocol_decoder_ford_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_v0_serialize,
    .deserialize = subghz_protocol_decoder_ford_v0_deserialize,
    .get_string = subghz_protocol_decoder_ford_v0_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ford_v0_encoder = {
    .alloc = subghz_protocol_encoder_ford_v0_alloc,
    .free = subghz_protocol_encoder_ford_v0_free,
    .deserialize = subghz_protocol_encoder_ford_v0_deserialize,
    .stop = subghz_protocol_encoder_ford_v0_stop,
    .yield = subghz_protocol_encoder_ford_v0_yield,
};

const SubGhzProtocol subghz_protocol_ford_v0 = {
    .name = SUBGHZ_PROTOCOL_FORD_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_ford_v0_decoder,
    .encoder = &subghz_protocol_ford_v0_encoder,
};

// =============================================================================
// CHECKSUM CALCULATION
// =============================================================================
static uint8_t ford_v0_calculate_checksum(uint32_t serial, uint32_t count, uint8_t button) {
    return (uint8_t)((((count >> 24) & 0xFF) + ((count >> 16) & 0xFF) + ((count >> 8) & 0xFF) +
                      (count & 0xFF) + ((serial >> 24) & 0xFF) + ((serial >> 16) & 0xFF) +
                      ((serial >> 8) & 0xFF) + (serial & 0xFF) + (button << 3)) &
                     0xFF);
}
// =============================================================================
// CRC FUNCTIONS
// =============================================================================

static uint8_t ford_v0_calculate_crc(uint8_t* buf) {
    uint8_t crc = 0;

    for(int row = 0; row < 8; row++) {
        uint8_t xor_sum = 0;
        for(int col = 0; col < 8; col++) {
            xor_sum ^= (ford_v0_crc_matrix[row * 8 + col] & buf[col + 1]);
        }
        uint8_t parity = subghz_protocol_blocks_parity8(xor_sum);
        if(parity) {
            crc |= (1 << row);
        }
    }

    return crc;
}
static uint8_t ford_v0_calculate_crc_for_tx(uint64_t key1, uint8_t checksum) {
    uint8_t buf[16] = {0};

    for(int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }

    buf[8] = checksum;

    uint8_t crc = ford_v0_calculate_crc(buf);
    return crc ^ 0x80;
}
static bool ford_v0_verify_crc(uint64_t key1, uint16_t key2) {
    uint8_t buf[16] = {0};

    for(int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }

    buf[8] = (uint8_t)(key2 >> 8);

    uint8_t calculated_crc = ford_v0_calculate_crc(buf);
    uint8_t received_crc = (uint8_t)(key2 & 0xFF) ^ 0x80;

    return ((calculated_crc & 0x7F) == (received_crc & 0x7F));
}

// =============================================================================
// DECODE FUNCTION
// =============================================================================

static void decode_ford_v0(
    uint64_t key1,
    uint16_t key2,
    uint32_t* serial,
    uint8_t* button,
    uint32_t* count) {
    uint8_t buf[13] = {0};

    for(int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }

    buf[8] = (uint8_t)(key2 >> 8);
    buf[9] = (uint8_t)(key2 & 0xFF);

    uint8_t tmp = buf[8];
    uint8_t parity = 0;
    uint8_t parity_any = (tmp != 0);
    while(tmp) {
        parity ^= (tmp & 1);
        tmp >>= 1;
    }
    buf[11] = parity_any ? parity : 0;

    uint8_t xor_byte;
    uint8_t limit;
    if(buf[11]) {
        xor_byte = buf[7];
        limit = 7;
    } else {
        xor_byte = buf[6];
        limit = 6;
    }

    for(int idx = 1; idx < limit; ++idx) {
        buf[idx] ^= xor_byte;
    }

    if(buf[11] == 0) {
        buf[7] ^= xor_byte;
    }

    uint8_t orig_b7 = buf[7];
    buf[7] = (orig_b7 & 0xAA) | (buf[6] & 0x55);
    uint8_t mixed = (buf[6] & 0xAA) | (orig_b7 & 0x55);
    buf[12] = mixed;
    buf[6] = mixed;

    uint32_t serial_le = ((uint32_t)buf[1]) | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16) |
                         ((uint32_t)buf[4] << 24);

    *serial = ((serial_le & 0xFF) << 24) | (((serial_le >> 8) & 0xFF) << 16) |
              (((serial_le >> 16) & 0xFF) << 8) | ((serial_le >> 24) & 0xFF);

    *button = (buf[5] >> 3) & 0x0F;

    *count = ((buf[5] & 0x07) << 16) | (buf[6] << 8) | buf[7];
}

// =============================================================================
// ENCODE FUNCTION
// =============================================================================
static void encode_ford_v0(
    uint8_t header_byte,
    uint32_t serial,
    uint8_t button,
    uint32_t count,
    uint8_t checksum,
    uint64_t* key1) {
    if(!key1) {
        FURI_LOG_E(TAG, "encode_ford_v0: NULL key1 pointer");
        return;
    }

    uint8_t buf[8] = {0};

    buf[0] = header_byte;

    buf[1] = (serial >> 24) & 0xFF;
    buf[2] = (serial >> 16) & 0xFF;
    buf[3] = (serial >> 8) & 0xFF;
    buf[4] = serial & 0xFF;

    buf[5] = ((button & 0x0F) << 3) | ((count >> 16) & 0x0F);

    uint8_t count_mid = (count >> 8) & 0xFF;
    uint8_t count_low = count & 0xFF;

    uint8_t post_xor_6 = (count_mid & 0xAA) | (count_low & 0x55);
    uint8_t post_xor_7 = (count_low & 0xAA) | (count_mid & 0x55);

    uint8_t parity = 0;
    uint8_t tmp = checksum;
    while(tmp) {
        parity ^= (tmp & 1);
        tmp >>= 1;
    }
    bool parity_bit = (checksum != 0) ? (parity != 0) : false;

    if(parity_bit) {
        uint8_t xor_byte = post_xor_7;
        buf[1] ^= xor_byte;
        buf[2] ^= xor_byte;
        buf[3] ^= xor_byte;
        buf[4] ^= xor_byte;
        buf[5] ^= xor_byte;
        buf[6] = post_xor_6 ^ xor_byte;
        buf[7] = post_xor_7;
    } else {
        uint8_t xor_byte = post_xor_6;
        buf[1] ^= xor_byte;
        buf[2] ^= xor_byte;
        buf[3] ^= xor_byte;
        buf[4] ^= xor_byte;
        buf[5] ^= xor_byte;
        buf[6] = post_xor_6;
        buf[7] = post_xor_7 ^ xor_byte;
    }

    *key1 = 0;
    for(int i = 0; i < 8; i++) {
        *key1 = (*key1 << 8) | buf[i];
    }

    FURI_LOG_I(
        TAG,
        "Encode: Sn=%08lX Btn=%d Cnt=%05lX Checksum=%02X",
        (unsigned long)serial,
        button,
        (unsigned long)count,
        checksum);
    FURI_LOG_I(
        TAG,
        "Encode key1: %08lX%08lX",
        (unsigned long)(*key1 >> 32),
        (unsigned long)(*key1 & 0xFFFFFFFF));
}

// =============================================================================
// ENCODER IMPLEMENTATION
// =============================================================================

void* subghz_protocol_encoder_ford_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFordV0* instance = calloc(1, sizeof(SubGhzProtocolEncoderFordV0));

    instance->base.protocol = &subghz_protocol_ford_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    if(!instance->encoder.upload || instance->encoder.size_upload < FORD_V0_UPLOAD_CAPACITY) {
        free(instance->encoder.upload);
        instance->encoder.upload = malloc(FORD_V0_UPLOAD_CAPACITY * sizeof(LevelDuration));
        furi_check(instance->encoder.upload);
    }
    instance->encoder.size_upload = FORD_V0_UPLOAD_CAPACITY;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    instance->key1 = 0;
    instance->key2 = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->count = 0;
    instance->checksum = 0;

    FURI_LOG_I(TAG, "Encoder allocated");
    return instance;
}

static void subghz_protocol_encoder_ford_v0_get_upload(SubGhzProtocolEncoderFordV0* instance) {
    furi_check(instance);
    size_t index = 0;

    uint64_t tx_key1 = ~instance->key1;
    uint16_t tx_key2 = ~instance->key2;

    FURI_LOG_I(
        TAG,
        "Building upload: key1=%08lX%08lX key2=%04X",
        (unsigned long)(instance->key1 >> 32),
        (unsigned long)(instance->key1 & 0xFFFFFFFF),
        instance->key2);

    uint32_t te_short = subghz_protocol_ford_v0_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v0_const.te_long;

#define ADD_LEVEL(lvl, dur)     \
    index = ford_v0_emit_merge( \
        instance->encoder.upload, index, FORD_V0_UPLOAD_CAPACITY, (lvl), (dur))

    for(uint8_t burst = 0; burst < FORD_V0_TOTAL_BURSTS; burst++) {
        ADD_LEVEL(true, te_short);
        ADD_LEVEL(false, te_long);

        for(int i = 0; i < FORD_V0_PREAMBLE_PAIRS; i++) {
            ADD_LEVEL(true, te_long);
            ADD_LEVEL(false, te_long);
        }

        ADD_LEVEL(true, te_short);
        ADD_LEVEL(false, FORD_V0_GAP_US);

        bool first_bit = (tx_key1 >> 62) & 1;
        if(first_bit) {
            ADD_LEVEL(true, te_long);
        } else {
            ADD_LEVEL(true, te_short);
            ADD_LEVEL(false, te_long);
        }

        bool prev_bit = first_bit;

        for(int bit = 61; bit >= 0; bit--) {
            bool curr_bit = (tx_key1 >> bit) & 1;

            if(!prev_bit && !curr_bit) {
                ADD_LEVEL(true, te_short);
                ADD_LEVEL(false, te_short);
            } else if(!prev_bit && curr_bit) {
                ADD_LEVEL(true, te_long);
            } else if(prev_bit && !curr_bit) {
                ADD_LEVEL(false, te_long);
            } else {
                ADD_LEVEL(false, te_short);
                ADD_LEVEL(true, te_short);
            }

            prev_bit = curr_bit;
        }

        for(int bit = 15; bit >= 0; bit--) {
            bool curr_bit = (tx_key2 >> bit) & 1;

            if(!prev_bit && !curr_bit) {
                ADD_LEVEL(true, te_short);
                ADD_LEVEL(false, te_short);
            } else if(!prev_bit && curr_bit) {
                ADD_LEVEL(true, te_long);
            } else if(prev_bit && !curr_bit) {
                ADD_LEVEL(false, te_long);
            } else {
                ADD_LEVEL(false, te_short);
                ADD_LEVEL(true, te_short);
            }

            prev_bit = curr_bit;
        }

        if(burst < FORD_V0_TOTAL_BURSTS - 1) {
            ADD_LEVEL(false, te_long * 100);
        }
    }

#undef ADD_LEVEL

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;

    FURI_LOG_I(TAG, "Upload built: %d bursts, size=%zu", FORD_V0_TOTAL_BURSTS, index);
}

void subghz_protocol_encoder_ford_v0_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_ford_v0_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_ford_v0_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFordV0* instance = context;
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
    subghz_protocol_encoder_ford_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFordV0* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(ford_v0_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        uint64_t original_key1 = instance->generic.data;
        uint8_t header_byte = (uint8_t)(original_key1 >> 56);

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        ford_v0_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        (({
            if(!subghz_custom_btn_has_original()) subghz_custom_btn_set_original(*(uint8_t*)&btn);
            *(uint8_t*)&btn = ford_other_get_btn_code();
            subghz_block_generic_global_button_override_get((uint8_t*)&btn);
        }));
        if(!subghz_block_generic_global_counter_override_get(&cnt)) {
            cnt += furi_hal_subghz_get_rolling_counter_mult();
        }

        instance->serial = serial;
        instance->button = (uint8_t)btn;
        instance->count = cnt;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;

        // Calculate Checksum from counter and button.
        instance->checksum =
            ford_v0_calculate_checksum(instance->serial, instance->count, instance->button);
        FURI_LOG_I(
            TAG,
            "Calculated Checksum: 0x%02X (from Cnt=0x%05lX, Btn=0x%02X)",
            instance->checksum,
            (unsigned long)instance->count,
            instance->button);

        encode_ford_v0(
            header_byte,
            instance->serial,
            instance->button,
            instance->count,
            instance->checksum,
            &instance->key1);

        instance->generic.data = instance->key1;
        instance->generic.data_count_bit = 64;

        uint8_t calculated_crc = ford_v0_calculate_crc_for_tx(instance->key1, instance->checksum);
        instance->key2 = ((uint16_t)instance->checksum << 8) | calculated_crc;

        FURI_LOG_I(
            TAG,
            "Final key2: 0x%04X (Checksum=0x%02X, CRC=0x%02X)",
            instance->key2,
            instance->checksum,
            calculated_crc);

        instance->encoder.repeat = ford_v0_encoder_read_repeat(flipper_format, 3);

        subghz_protocol_encoder_ford_v0_get_upload(instance);

        if(instance->encoder.size_upload == 0) {
            FURI_LOG_E(TAG, "Upload build failed");
            break;
        }

        //Update the PSF file, since we have overwritten the COUNTER and BUTTON
        //This makes the file's nummers wrong, and fails tests. It wasnt causing a TX bug, but manual tests failed.
        flipper_format_rewind(flipper_format);
        uint32_t temp = calculated_crc;
        flipper_format_insert_or_update_uint32(flipper_format, "CRC", &temp, 1);
        temp = instance->checksum;
        flipper_format_insert_or_update_uint32(flipper_format, "Checksum", &temp, 1);

        flipper_format_rewind(flipper_format);
        uint8_t key_bytes[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_bytes[sizeof(uint64_t) - i - 1] = (instance->generic.data >> (i * 8)) & 0xFF;
        }
        flipper_format_insert_or_update_hex(flipper_format, "Key", key_bytes, sizeof(key_bytes));
        uint32_t serial_w = instance->serial;
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->button;
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->count;
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
        subghz_block_generic_global.current_cnt = instance->count;

        instance->encoder.is_running = true;

        FURI_LOG_I(
            TAG,
            "Encoder ready: size=%zu repeat=%u",
            instance->encoder.size_upload,
            instance->encoder.repeat);

        ret = SubGhzProtocolStatusOk;
    } while(false);

    FURI_LOG_I(TAG, "Encoder deserialize finished, status=%d", ret);
    return ret;
}

// =============================================================================
// DECODER IMPLEMENTATION
// =============================================================================

static void ford_v0_add_bit(SubGhzProtocolDecoderFordV0* instance, bool bit) {
    uint32_t low = (uint32_t)instance->data_low;
    instance->data_low = (instance->data_low << 1) | (bit ? 1 : 0);
    instance->data_high = (instance->data_high << 1) | ((low >> 31) & 1);
    instance->bit_count++;
}

static bool ford_v0_process_data(SubGhzProtocolDecoderFordV0* instance) {
    if(instance->bit_count == 64) {
        uint64_t combined = ((uint64_t)instance->data_high << 32) | instance->data_low;
        instance->key1 = ~combined;
        instance->data_low = 0;
        instance->data_high = 0;
        return false;
    }

    if(instance->bit_count == 80) {
        uint16_t key2_raw = (uint16_t)(instance->data_low & 0xFFFF);
        uint16_t key2 = ~key2_raw;

        decode_ford_v0(
            instance->key1, key2, &instance->serial, &instance->button, &instance->count);

        instance->key2 = key2;
        return true;
    }

    return false;
}

void* subghz_protocol_decoder_ford_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFordV0* instance = malloc(sizeof(SubGhzProtocolDecoderFordV0));
    instance->base.protocol = &subghz_protocol_ford_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_ford_v0_free(void* context) {
    furi_check(context);
    free(context);
}

uint8_t subghz_protocol_decoder_ford_v0_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8U) + 1U);
}
void subghz_protocol_decoder_ford_v0_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFordV0* instance = context;

    instance->decoder.parser_step = FordV0DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->header_count = 0;
    instance->key1 = 0;
    instance->key2 = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->count = 0;
}

void subghz_protocol_decoder_ford_v0_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFordV0* instance = context;

    uint32_t te_short = subghz_protocol_ford_v0_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v0_const.te_long;
    uint32_t te_delta = subghz_protocol_ford_v0_const.te_delta;
    uint32_t gap_threshold = 3500;

    switch(instance->decoder.parser_step) {
    case FordV0DecoderStepReset:
        if(level && (DURATION_DIFF(duration, te_short) < te_delta)) {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->decoder.parser_step = FordV0DecoderStepPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            instance->bit_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case FordV0DecoderStepPreamble:
        if(!level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = FordV0DecoderStepPreambleCheck;
            } else {
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        break;

    case FordV0DecoderStepPreambleCheck:
        if(level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                instance->header_count++;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = FordV0DecoderStepPreamble;
            } else if(DURATION_DIFF(duration, te_short) < te_delta) {
                instance->decoder.parser_step = FordV0DecoderStepGap;
            } else {
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        break;

    case FordV0DecoderStepGap:
        if(!level && (DURATION_DIFF(duration, gap_threshold) < 500)) {
            instance->data_low = 1;
            instance->data_high = 0;
            instance->bit_count = 1;
            instance->decoder.parser_step = FordV0DecoderStepData;
        } else if(!level && duration > gap_threshold + 500) {
            instance->decoder.parser_step = FordV0DecoderStepReset;
        }
        break;

    case FordV0DecoderStepData: {
        ManchesterEvent event = (({
            ManchesterEvent _ev = ManchesterEventReset;
            if(DURATION_DIFF(duration, subghz_protocol_ford_v0_const.te_short) <
               subghz_protocol_ford_v0_const.te_delta)
                _ev = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            else if(
                DURATION_DIFF(duration, subghz_protocol_ford_v0_const.te_long) <
                subghz_protocol_ford_v0_const.te_delta)
                _ev = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            _ev;
        }));
        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = FordV0DecoderStepReset;
            break;
        }

        bool data_bit;
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            ford_v0_add_bit(instance, data_bit);

            if(ford_v0_process_data(instance)) {
                instance->generic.data = instance->key1;
                instance->generic.data_count_bit = 64;
                instance->generic.serial = instance->serial;
                instance->generic.btn = instance->button;
                instance->generic.cnt = instance->count;

                if(instance->base.callback) {
                    instance->base.callback(&instance->base, instance->base.context);
                }

                instance->data_low = 0;
                instance->data_high = 0;
                instance->bit_count = 0;
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }

        instance->decoder.te_last = duration;
        break;
    }
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFordV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t temp = (instance->key2 >> 8) & 0xFF;
        flipper_format_write_uint32(flipper_format, "Checksum", &temp, 1);

        temp = instance->key2 & 0xFF;
        flipper_format_write_uint32(flipper_format, "CRC", &temp, 1);
        uint32_t serial_w = instance->serial;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Serial", &serial_w, 1);
        uint32_t btn_w = instance->button;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_w, 1);
        uint32_t cnt_w = instance->count;
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_w, 1);
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ford_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFordV0* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_ford_v0_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        instance->key1 = instance->generic.data;

        flipper_format_rewind(flipper_format);

        uint32_t checksum_temp = 0;
        uint32_t crc_temp = 0;
        flipper_format_read_uint32(flipper_format, "Checksum", &checksum_temp, 1);
        flipper_format_read_uint32(flipper_format, "CRC", &crc_temp, 1);
        instance->key2 = ((checksum_temp & 0xFF) << 8) | (crc_temp & 0xFF);

        uint32_t serial = instance->serial;
        uint32_t btn = instance->button;
        uint32_t cnt = instance->count;
        ford_v0_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
        instance->serial = serial;
        instance->button = (uint8_t)btn;
        instance->count = cnt;
        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->count;
    }

    return ret;
}

void subghz_protocol_decoder_ford_v0_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderFordV0* instance = context;
    ford_v0_get_string_setup_editor(instance->button, instance->count, 4, 16, 5);

    const uint32_t code_found_hi = (uint32_t)(instance->key1 >> 32);
    const uint32_t code_found_lo = (uint32_t)(instance->key1 & 0xFFFFFFFFU);
    const bool crc_ok = ford_v0_verify_crc(instance->key1, instance->key2);

    const char* button_name = "??";
    if(instance->button == 0x01)
        button_name = "Panic";
    else if(instance->button == 0x02)
        button_name = "Lock";
    else if(instance->button == 0x04)
        button_name = "Unlock";
    else if(instance->button == 0x08)
        button_name = "Boot";

    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, instance->generic.data_count_bit);
    furi_string_cat_printf(
        output,
        "Key1:%08lX%08lX\r\nKey2:%04X\r\n",
        (unsigned long)code_found_hi,
        (unsigned long)code_found_lo,
        (unsigned)instance->key2);
    ((button_name && button_name[0] != '\0') ? furi_string_cat_printf(
                                                   output,
                                                   "Sn:%0*lX Btn:%02X [%s]\r\n",
                                                   (int)8,
                                                   (unsigned long)instance->serial,
                                                   instance->button,
                                                   button_name) :
                                               furi_string_cat_printf(
                                                   output,
                                                   "Sn:%0*lX Btn:%02X\r\n",
                                                   (int)8,
                                                   (unsigned long)instance->serial,
                                                   instance->button));
    furi_string_cat_printf(
        output,
        "Cnt:%05lX CRC:%02X [%s]\r\n",
        (unsigned long)instance->count,
        instance->key2 & 0xFF,
        crc_ok ? "OK" : "ERR");
}
