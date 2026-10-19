#include "renault_v1.h"
#include "hitag2_key_db.h"

#include "../blocks/const.h"
#include "../blocks/custom_btn_i.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi_hal.h>
#include <lib/toolbox/manchester_decoder.h>
#include <stdlib.h>
#include <string.h>

#define TAG "RenaultV1Protocol"

#define HITAG2_TE_US                   125U
#define HITAG2_HEADER_LOW_US           1500U
#define HITAG2_HEADER_HIGH_US          1000U
#define HITAG2_SHORT_GAP_US            20000U
#define HITAG2_LONG_TO_SHORT_GAP_US    115500U
#define HITAG2_PREAMBLE_PAIRS          255U
#define HITAG2_SHORT_FRAMES            8U
#define HITAG2_UPLOAD_CAPACITY         1792U
#define HITAG2_TYPE_FIELD              "Type"
#define HITAG2_VARIANT_OLD             0U
#define HITAG2_VARIANT_KANGOO          1U
#define HITAG2_DEFAULT_VARIANT         HITAG2_VARIANT_KANGOO
#define HITAG2_OLD_LONG_FRAMES         2U
#define HITAG2_KANGOO_LONG_FRAMES      3U
#define HITAG2_OLD_GAP_MIN_US          80000U
#define HITAG2_OLD_GAP_MAX_US          140000U
#define HITAG2_SHORT_GAP_DETECT_US     18000U
#define HITAG2_SHORT_GAP_DETECT_MAX_US 25000U
#define HITAG2_MIN_COUNT_BIT           88U
#define HITAG2_HEADER_BITS             16U
#define HITAG2_KEY_BITS                64U
#define HITAG2_KEY2_BITS               24U
#define HITAG2_SHORT_KEY_BITS          10U
#define HITAG2_KEY_END_BITS            (HITAG2_HEADER_BITS + HITAG2_KEY_BITS)
#define HITAG2_LONG_FRAME_BITS         (HITAG2_KEY_END_BITS + HITAG2_KEY2_BITS)
#define HITAG2_RECOVERED_YES           1U
#define HITAG2_RECOVERED_BF_MISS       2U
#define HITAG2_HELL_KEY_FIELD          "Hitag2 Key"
#define HITAG2_HELL_EPOCH_FIELD        "Hitag2 Epoch"

#define HITAG2_HEADER_LOW_MIN_US  1400U
#define HITAG2_HEADER_LOW_MAX_US  2200U
#define HITAG2_HEADER_HIGH_MIN_US 920U
#define HITAG2_HEADER_HIGH_MAX_US 1150U
#define HITAG2_DATA_IGNORE_US     49U
#define HITAG2_DATA_RESET_US      520U
#define HITAG2_TE_HIGH_INIT_US    120U
#define HITAG2_TE_LOW_INIT_US     150U
#define HITAG2_HOP_FIELD          "Hop"

static const SubGhzBlockConst subghz_protocol_hitag2_const = {
    .te_short = HITAG2_TE_US,
    .te_long = HITAG2_TE_US * 2U,
    .te_delta = 50,
    .min_count_bit_for_found = HITAG2_MIN_COUNT_BIT,
};

struct SubGhzProtocolDecoderHitag2 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint16_t te_high;
    uint16_t te_low;
    uint16_t header;
    uint8_t recovered;
    uint8_t hitag2_key[6];
    uint32_t hitag2_epoch;
    uint32_t hop;
    uint8_t tail_bits;
    bool hell_key_valid;
    uint64_t last_data;
    uint64_t last_data_2;
    bool last_frame_valid;
    uint8_t tx_variant;
    uint8_t identical_count;
    bool pending_notify;
};

struct SubGhzProtocolEncoderHitag2 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t recovered;
    uint8_t hitag2_key[6];
    uint32_t hitag2_epoch;
    uint8_t tail_bits;
    bool hell_key_valid;
    uint8_t tx_variant;
};

typedef enum {
    Hitag2DecoderStepReset = 0,
    Hitag2DecoderStepCheckSync = 2,
    Hitag2DecoderStepData = 3,
} Hitag2DecoderStep;

static uint8_t subghz_protocol_decoder_hitag2_get_hash_data(void* context);

const SubGhzProtocolDecoder subghz_protocol_hitag2_decoder = {
    .alloc = subghz_protocol_decoder_hitag2_alloc,
    .free = subghz_protocol_decoder_hitag2_free,

    .feed = subghz_protocol_decoder_hitag2_feed,
    .reset = subghz_protocol_decoder_hitag2_reset,

    .get_hash_data = subghz_protocol_decoder_hitag2_get_hash_data,
    .serialize = subghz_protocol_decoder_hitag2_serialize,
    .deserialize = subghz_protocol_decoder_hitag2_deserialize,
    .get_string = subghz_protocol_decoder_hitag2_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_hitag2_encoder = {
    .alloc = subghz_protocol_encoder_hitag2_alloc,
    .free = subghz_protocol_encoder_hitag2_free,

    .deserialize = subghz_protocol_encoder_hitag2_deserialize,
    .stop = subghz_protocol_encoder_hitag2_stop,
    .yield = subghz_protocol_encoder_hitag2_yield,
};

const SubGhzProtocol subghz_protocol_renault_v1 = {
    .name = SUBGHZ_PROTOCOL_RENAULT_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868 |
            SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_hitag2_decoder,
    .encoder = &subghz_protocol_hitag2_encoder,
};

static const char* hitag2_get_button_name(uint8_t btn) {
    static const char* const names[] = {
        "Sync",
        "Lock",
        "Unlock",
        "??",
        "Trunk",
        "??",
        "??",
        "??",
        "Panic",
    };
    return (btn < COUNT_OF(names)) ? names[btn] : "??";
}

static void hitag2_u64_to_bytes_be(uint64_t value, uint8_t* out, size_t nbytes) {
    for(size_t i = 0; i < nbytes; i++) {
        out[i] = (uint8_t)(value >> (8U * (nbytes - 1U - i)));
    }
}

static uint64_t hitag2_bytes_to_u64_be(const uint8_t* data, size_t nbytes) {
    uint64_t value = 0;
    for(size_t i = 0; i < nbytes; i++) {
        value = (value << 8U) | data[i];
    }
    return value;
}

static void hitag2_pack_key_bytes(uint64_t key, uint64_t key_2, uint8_t raw[11]) {
    hitag2_u64_to_bytes_be(key, raw, 8);
    raw[8] = (uint8_t)(key_2 >> 16U);
    raw[9] = (uint8_t)(key_2 >> 8U);
    raw[10] = (uint8_t)key_2;
}

static uint8_t hitag2_frame_xor(const uint8_t raw[11]) {
    uint8_t value = 0;
    for(size_t i = 0; i < 10; i++) {
        value ^= raw[i];
    }
    return value;
}

static uint8_t hitag2_hell_i4(uint64_t x, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint8_t)((((x >> a) & 1U) << 3U) | (((x >> b) & 1U) << 2U) | (((x >> c) & 1U) << 1U) |
                     ((x >> d) & 1U));
}

static uint8_t hitag2_hell_f20(uint64_t state) {
    const uint8_t s0 = (uint8_t)((0x3C65U >> hitag2_hell_i4(state, 2U, 3U, 5U, 6U)) & 1U);
    const uint8_t s1 = (uint8_t)((0x0EE5U >> hitag2_hell_i4(state, 8U, 12U, 14U, 15U)) & 1U);
    const uint8_t s2 = (uint8_t)((0x0EE5U >> hitag2_hell_i4(state, 17U, 21U, 23U, 26U)) & 1U);
    const uint8_t s3 = (uint8_t)((0x0EE5U >> hitag2_hell_i4(state, 28U, 29U, 31U, 33U)) & 1U);
    const uint8_t s4 = (uint8_t)((0x3C65U >> hitag2_hell_i4(state, 34U, 43U, 44U, 46U)) & 1U);
    return (uint8_t)((0x0DD3929BUL >> ((s0 << 4U) | (s1 << 3U) | (s2 << 2U) | (s3 << 1U) | s4)) &
                     1U);
}

static uint64_t hitag2_hell_lfsr(uint64_t state) {
    const uint64_t fb =
        (state ^ (state >> 2U) ^ (state >> 3U) ^ (state >> 6U) ^ (state >> 7U) ^ (state >> 8U) ^
         (state >> 16U) ^ (state >> 22U) ^ (state >> 23U) ^ (state >> 26U) ^ (state >> 30U) ^
         (state >> 41U) ^ (state >> 42U) ^ (state >> 43U) ^ (state >> 46U) ^ (state >> 47U)) &
        1ULL;
    return (state >> 1U) | (fb << 47U);
}

static uint64_t hitag2_hell_key_u64(const uint8_t key[6]) {
    uint64_t key64 = 0ULL;
    for(size_t i = 0; i < 6U; i++) {
        key64 = (key64 << 8U) | key[i];
    }
    return key64;
}

static uint32_t hitag2_hell_authenticator(
    uint32_t uid,
    uint8_t button,
    uint32_t counter,
    const uint8_t key[6]) {
    const uint64_t key64 = hitag2_hell_key_u64(key);
    const uint32_t nonce = (counter << 4U) | ((uint32_t)button & 0x0FU);
    uint64_t state = 0ULL;
    for(uint8_t i = 32U; i < 48U; i++) {
        state = (state << 1U) | ((key64 >> i) & 1ULL);
    }
    for(uint8_t i = 0U; i < 32U; i++) {
        state = (state << 1U) | ((uint64_t)((uid >> i) & 1U));
    }
    for(uint8_t i = 0U; i < 32U; i++) {
        const uint64_t nonce_bit = (uint64_t)hitag2_hell_f20(state) ^ ((nonce >> (31U - i)) & 1U);
        state = (state >> 1U) | (((nonce_bit ^ ((key64 >> (31U - i)) & 1ULL)) & 1ULL) << 47U);
    }
    uint32_t hop = 0U;
    for(uint8_t i = 0U; i < 32U; i++) {
        hop = (hop << 1U) | hitag2_hell_f20(state);
        state = hitag2_hell_lfsr(state);
    }
    return hop;
}

static uint16_t hitag2_hell_cnt10(const uint8_t raw[11]) {
    return (uint16_t)(((uint16_t)(raw[4] & 0x0FU) << 6U) | (raw[5] >> 2U));
}

static uint32_t hitag2_hell_hop(const uint8_t raw[11]) {
    return ((uint32_t)(raw[5] & 3U) << 30U) | ((uint32_t)raw[6] << 22U) |
           ((uint32_t)raw[7] << 14U) | ((uint32_t)raw[8] << 6U) | (raw[9] >> 2U);
}

static uint8_t hitag2_hell_tail(const uint8_t raw[11]) {
    return (uint8_t)(raw[9] & 3U);
}

static void hitag2_pack_hell_frame(
    uint32_t uid,
    uint8_t btn,
    uint16_t cnt10,
    uint32_t hop,
    uint8_t tail,
    uint8_t raw[11]) {
    raw[0] = (uint8_t)(uid >> 24U);
    raw[1] = (uint8_t)(uid >> 16U);
    raw[2] = (uint8_t)(uid >> 8U);
    raw[3] = (uint8_t)uid;
    raw[4] = (uint8_t)(((btn & 0x0FU) << 4U) | ((cnt10 >> 6U) & 0x0FU));
    raw[5] = (uint8_t)(((cnt10 & 0x3FU) << 2U) | ((hop >> 30U) & 3U));
    raw[6] = (uint8_t)(hop >> 22U);
    raw[7] = (uint8_t)(hop >> 14U);
    raw[8] = (uint8_t)(hop >> 6U);
    raw[9] = (uint8_t)(((hop << 2U) & 0xFCU) | (tail & 3U));
    raw[10] = hitag2_frame_xor(raw);
}

static void hitag2_apply_raw(uint8_t raw[11], uint64_t* data, uint64_t* data_2) {
    *data = hitag2_bytes_to_u64_be(raw, 8);
    *data_2 = ((uint64_t)raw[8] << 16U) | ((uint64_t)raw[9] << 8U) | raw[10];
}

static bool hitag2_hell_key_nonzero(const uint8_t key[6]) {
    for(size_t i = 0; i < 6U; i++) {
        if(key[i]) return true;
    }
    return false;
}

static void hitag2_flipper_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

static uint8_t hitag2_clamp_variant(uint32_t type) {
    return (type == HITAG2_VARIANT_OLD) ? HITAG2_VARIANT_OLD : HITAG2_VARIANT_KANGOO;
}

static uint8_t hitag2_read_variant(FlipperFormat* ff) {
    uint32_t type = HITAG2_DEFAULT_VARIANT;
    if(ff && flipper_format_rewind(ff)) {
        flipper_format_read_uint32(ff, HITAG2_TYPE_FIELD, &type, 1);
    }
    return hitag2_clamp_variant(type);
}

static uint8_t hitag2_variant_long_frames(uint8_t variant) {
    return (variant == HITAG2_VARIANT_KANGOO) ? HITAG2_KANGOO_LONG_FRAMES : HITAG2_OLD_LONG_FRAMES;
}

static void hitag2_decoder_notify(SubGhzProtocolDecoderHitag2* instance) {
    if(!instance->pending_notify) {
        return;
    }
    instance->pending_notify = false;
    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

static bool hitag2_read_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint8_t* data,
    size_t nbytes);
static bool renault_v1_apply_stored_secret(FlipperFormat* ff);

static bool hitag2_read_hell_key(FlipperFormat* ff, uint8_t key[6], uint32_t* epoch_out) {
    memset(key, 0, 6U);
    if(!hitag2_read_hex_be(ff, HITAG2_HELL_KEY_FIELD, key, 6U)) {
        return false;
    }
    uint32_t epoch = 0U;
    flipper_format_rewind(ff);
    if(flipper_format_read_uint32(ff, HITAG2_HELL_EPOCH_FIELD, &epoch, 1)) {
        epoch &= 0x3FFFFUL;
    }
    if(epoch_out) {
        *epoch_out = epoch;
    }
    return hitag2_hell_key_nonzero(key);
}

static bool hitag2_hell_find_epoch(
    const uint8_t key[6],
    uint32_t uid,
    uint8_t btn,
    uint16_t cnt10,
    uint32_t hop,
    uint32_t* epoch_out) {
    uint32_t start = epoch_out ? (*epoch_out & 0x3FFFFUL) : 0U;
    for(uint32_t i = 0U; i < 8U; i++) {
        const uint32_t epoch = (start + i) & 0x3FFFFUL;
        const uint32_t full = (epoch << 10U) | (cnt10 & 0x3FFU);
        if(hitag2_hell_authenticator(uid, btn, full, key) == hop) {
            if(epoch_out) {
                *epoch_out = epoch;
            }
            return true;
        }
    }
    return false;
}

static void hitag2_unpack_frame(
    uint64_t data,
    uint64_t data_2,
    uint32_t* serial,
    uint8_t* btn,
    uint16_t* cnt10,
    uint32_t* hop,
    uint8_t* tail) {
    uint8_t raw[11];
    hitag2_pack_key_bytes(data, data_2, raw);
    if(serial) {
        *serial = (uint32_t)(data >> 32U);
    }
    if(btn) {
        *btn = (uint8_t)((raw[4] >> 4U) & 0x0FU);
    }
    if(cnt10) {
        *cnt10 = hitag2_hell_cnt10(raw);
    }
    if(hop) {
        *hop = hitag2_hell_hop(raw);
    }
    if(tail) {
        *tail = hitag2_hell_tail(raw);
    }
}

static bool hitag2_read_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint8_t* data,
    size_t nbytes) {
    if(!flipper_format_rewind(flipper_format)) {
        return false;
    }
    return flipper_format_read_hex(flipper_format, name, data, nbytes);
}

static bool hitag2_write_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint64_t value,
    size_t nbytes) {
    uint8_t data[8] = {0};
    furi_check(nbytes <= sizeof(data));
    hitag2_u64_to_bytes_be(value, data, nbytes);
    return flipper_format_insert_or_update_hex(flipper_format, name, data, nbytes);
}

static uint8_t hitag2_ubfx(uint32_t value, uint8_t lsb, uint8_t width) {
    return (uint8_t)((value >> lsb) & ((1U << width) - 1U));
}

static void hitag2_serial_permute(const uint8_t serial[4], uint8_t perm[6]) {
    const uint8_t sn0 = serial[0];
    const uint8_t sn1 = serial[1];
    const uint8_t sn2 = serial[2];
    const uint8_t sn3 = serial[3];

    uint8_t acc =
        (uint8_t)(((sn0 >> 6) & 2U) | ((sn1 >> 4) & 8U) | hitag2_ubfx(sn0 ^ 0x10U, 4, 1));
    acc |= (uint8_t)((~(uint32_t)(sn0 << 2)) & 0x20U);
    acc |= (uint8_t)((sn0 << 5) & 0x40U);
    acc |= (uint8_t)((sn2 << 1) & 0x80U);
    acc |= (uint8_t)((~(uint32_t)(sn2 >> 5)) & 4U);
    acc |= (uint8_t)((~(uint32_t)(sn0 >> 2)) & 0x10U);
    perm[0] = acc;

    const uint8_t sn1_inv_shr3 = (uint8_t) ~(sn1 >> 3);
    const uint8_t sn3_inv_shl3 = (uint8_t) ~(sn3 << 3);
    acc = (uint8_t)(hitag2_ubfx(sn1, 5, 1) | (sn1_inv_shr3 & 4U) | (sn0 & 0x20U));
    acc |= (uint8_t)(sn3_inv_shl3 & 8U);
    acc |= (uint8_t)((~(uint32_t)(sn2 << 2)) & 0x10U);
    acc |= (uint8_t)((~(uint32_t)(sn2 << 3)) & 0x40U);
    acc |= 0x80U;
    perm[1] = acc;

    const uint8_t sn0_shr3 = (uint8_t)(sn0 >> 3);
    acc = (uint8_t)(hitag2_ubfx(sn0, 2, 1) | (sn0_shr3 & 2U) | ((~(uint32_t)(sn3 >> 2)) & 4U) |
                    (sn1 & 0x10U));
    acc |= (uint8_t)((~(uint32_t)(sn1 << 4)) & 0x20U);
    acc |= (uint8_t)(sn3_inv_shl3 & 0x40U);
    acc |= 0x80U;
    perm[2] = acc;

    const uint8_t sn2_inv_shl6 = (uint8_t) ~(sn2 << 6);
    acc = (uint8_t)(((sn0 >> 2) & 2U) | ((sn1 >> 3) & 8U) | hitag2_ubfx(sn2 ^ 0x20U, 5, 1));
    acc |= (uint8_t)((sn1 << 5) & 0x20U);
    acc |= (uint8_t)((sn3 << 1) & 0x40U);
    acc |= (uint8_t)(((uint8_t)~sn0_shr3) & 4U);
    acc |= (uint8_t)(sn1_inv_shr3 & 0x10U);
    acc |= (uint8_t)(sn2_inv_shl6 & 0x80U);
    perm[3] = acc;

    uint8_t perm4_lo =
        (uint8_t)(((sn3 << 2) & 8U) | ((sn0 << 4) & 0x10U) | hitag2_ubfx(sn0 ^ 4U, 2, 1));
    perm4_lo |= (uint8_t)((~(uint32_t)(sn3 >> 1)) & 2U);
    perm4_lo |= (uint8_t)((~(uint32_t)(sn1 >> 4)) & 4U);
    uint8_t perm4_hi = (uint8_t)((~(uint32_t)(sn0 << 4)) & 0x20U);
    perm4_hi |= (uint8_t)(sn2_inv_shl6 & 0x40U);
    const uint8_t sn1_inv_shl3 = (uint8_t) ~(sn1 << 3);
    perm4_hi |= (uint8_t)(sn1_inv_shl3 & 0x80U);
    perm[4] = (uint8_t)(perm4_lo | perm4_hi);

    uint8_t perm5 =
        (uint8_t)(((sn3 >> 2) & 0x10U) | ((sn2 >> 3) & 2U) | (((uint8_t)~sn0) & 0x80U));
    perm5 |= (uint8_t)((~(uint32_t)(sn0 << 3)) & 8U);
    perm5 |= (uint8_t)((sn0 >> 2) & 0x10U);
    perm5 |= (uint8_t)(sn1_inv_shl3 & 0x20U);
    perm5 |= (uint8_t)((sn3 >> 1) & 0x40U);
    perm5 |= (uint8_t)((~(uint32_t)(sn1 >> 1)) & 4U);
    perm[5] = perm5;
}

static uint8_t hitag2_truth(uint32_t table, uint8_t index) {
    return (uint8_t)((table >> index) & 1U);
}

static uint8_t hitag2_filter_index(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint8_t)((a << 3U) | (b << 2U) | (c << 1U) | d);
}

static uint8_t hitag2_byte_bit(uint8_t byte, uint8_t bit) {
    return (uint8_t)((byte >> bit) & 1U);
}

static uint8_t hitag2_filter(const uint8_t state[6]) {
    uint8_t group = 0;
    group |= hitag2_truth(
        0x2C79U,
        hitag2_filter_index(
            hitag2_byte_bit(state[0], 1),
            hitag2_byte_bit(state[0], 2),
            hitag2_byte_bit(state[0], 4),
            hitag2_byte_bit(state[0], 5)));
    group |= (uint8_t)(hitag2_truth(
                           0x6671U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[1], 0),
                               hitag2_byte_bit(state[1], 1),
                               hitag2_byte_bit(state[1], 3),
                               hitag2_byte_bit(state[1], 7)))
                       << 1U);
    group |= (uint8_t)(hitag2_truth(
                           0x6671U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[3], 5),
                               hitag2_byte_bit(state[2], 0),
                               hitag2_byte_bit(state[2], 2),
                               hitag2_byte_bit(state[2], 6)))
                       << 2U);
    group |= (uint8_t)(hitag2_truth(
                           0x6671U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[4], 6),
                               hitag2_byte_bit(state[3], 0),
                               hitag2_byte_bit(state[3], 2),
                               hitag2_byte_bit(state[3], 3)))
                       << 3U);
    group |= (uint8_t)(hitag2_truth(
                           0x2C79U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[5], 1),
                               hitag2_byte_bit(state[5], 3),
                               hitag2_byte_bit(state[5], 4),
                               hitag2_byte_bit(state[4], 5)))
                       << 4U);
    return hitag2_truth(0x7907287BUL, group);
}

static uint8_t hitag2_parity8(uint8_t value) {
    value ^= (uint8_t)(value >> 4U);
    value ^= (uint8_t)(value >> 2U);
    value ^= (uint8_t)(value >> 1U);
    return (uint8_t)(value & 1U);
}

static uint8_t hitag2_feedback(const uint8_t state[6]) {
    static const uint8_t masks[6] = {0xB3U, 0x80U, 0x83U, 0x22U, 0x00U, 0x73U};
    uint8_t feedback = 0;
    for(uint8_t i = 0; i < 6; i++) {
        feedback ^= hitag2_parity8((uint8_t)(state[i] & masks[i]));
    }
    return (uint8_t)(feedback & 1U);
}

static void hitag2_shift_state(uint8_t state[6], uint8_t input) {
    for(uint8_t i = 0; i < 5; i++) {
        state[i] = (uint8_t)((state[i] << 1U) | (state[i + 1U] >> 7U));
    }
    state[5] = (uint8_t)((state[5] << 1U) | (input & 1U));
}

static void hitag2_shift_u32(uint8_t buf[4], uint8_t inject) {
    const uint8_t b0 = buf[0];
    const uint8_t b1 = buf[1];
    const uint8_t b2 = buf[2];
    const uint8_t b3 = buf[3];
    buf[3] = (uint8_t)((b2 >> 7U) | (b3 << 1U));
    buf[2] = (uint8_t)((b1 >> 7U) | (b2 << 1U));
    buf[1] = (uint8_t)((b0 >> 7U) | (b1 << 1U));
    buf[0] = (uint8_t)((b0 << 1U) | (inject & 1U));
}

static void hitag2_clock_cipher(uint8_t state[6], uint8_t iv_work[4], uint8_t iv_orig[4]) {
    for(uint8_t i = 0; i < 32; i++) {
        const uint8_t filter = hitag2_filter(state);
        uint8_t mix = (iv_work[3] & 0x80U) ? (filter ? 0U : 1U) : (filter ? 1U : 0U);
        if(iv_orig[3] & 0x80U) {
            mix ^= 5U;
        }
        hitag2_shift_state(state, (uint8_t)(mix & 1U));
        hitag2_shift_u32(iv_work, 0);
        hitag2_shift_u32(iv_orig, (uint8_t)((mix >> 2U) & 1U));
    }

    for(uint8_t i = 0; i < 32; i++) {
        hitag2_shift_u32(iv_work, 0);
        if(hitag2_filter(state)) {
            iv_work[0] |= 1U;
        }
        hitag2_shift_state(state, hitag2_feedback(state));
    }
}

static void hitag2_build_iv(uint32_t cnt, uint8_t btn, uint32_t seed, uint8_t iv[4]) {
    iv[0] = (uint8_t)(((cnt << 4U) & 0xF0U) | (btn & 0x0FU));
    iv[1] = (uint8_t)((cnt >> 4U) & 0xFFU);
    iv[2] = (uint8_t)(((seed >> 8U) & 0xF0U) | ((cnt >> 12U) & 0x0FU));
    iv[3] = (uint8_t)(seed & 0xFFU);
}

static void
    hitag2_encrypt_from_iv(const uint8_t serial_be[4], const uint8_t iv[4], uint8_t out[11]) {
    uint8_t perm[6];
    uint8_t state[6];
    uint8_t iv_work[4];
    uint8_t iv_orig[4];

    hitag2_serial_permute(serial_be, perm);
    state[0] = serial_be[0];
    state[1] = serial_be[1];
    state[2] = serial_be[2];
    state[3] = serial_be[3];
    state[4] = perm[4];
    state[5] = perm[5];
    iv_work[0] = perm[0];
    iv_work[1] = perm[1];
    iv_work[2] = perm[2];
    iv_work[3] = perm[3];
    iv_orig[0] = iv[0];
    iv_orig[1] = iv[1];
    iv_orig[2] = iv[2];
    iv_orig[3] = iv[3];
    hitag2_clock_cipher(state, iv_work, iv_orig);

    const uint8_t hop0 = iv_work[0];
    uint8_t hop1 = (uint8_t)((hop0 >> 7U) | (iv_work[1] << 1U));
    uint8_t hop2 = (uint8_t)((iv_work[1] >> 7U) | (iv_work[2] << 1U));
    const uint8_t hop_ext = (uint8_t)(((hop0 >> 6U) & 1U) | (hop1 << 1U));
    uint8_t hop3 = (uint8_t)((iv_work[2] >> 7U) | (iv_work[3] << 1U));
    uint8_t hop4 = (uint8_t)((iv_work[3] >> 7U) | (state[5] << 1U));
    hop1 = (uint8_t)((hop1 >> 7U) | (hop2 << 1U));
    hop2 = (uint8_t)((hop2 >> 7U) | (hop3 << 1U));
    hop3 = (uint8_t)((hop3 >> 7U) | (hop4 << 1U));

    uint32_t mix = ((uint32_t)iv[1] << 4U) | ((uint32_t)iv[0] >> 4U);
    mix = (mix | (((uint32_t)iv[2] << 12U) & 0xFFFFU)) & 0xFFFFU;

    out[0] = serial_be[0];
    out[1] = serial_be[1];
    out[2] = serial_be[2];
    out[3] = serial_be[3];
    out[4] = (uint8_t)(((mix >> 6U) & 0x0FU) | ((iv[0] << 4U) & 0xF0U));
    out[5] = (uint8_t)((hop3 & 3U) | ((mix << 2U) & 0xFCU));
    out[6] = hop2;
    out[7] = hop1;
    out[8] = hop_ext;
    out[9] = (uint8_t)((hop0 << 2U) | 2U);
    out[10] = hitag2_frame_xor(out);
}

static void hitag2_encrypt_frame(
    uint32_t serial,
    uint32_t cnt,
    uint8_t btn,
    uint32_t seed,
    uint8_t out[11],
    uint8_t iv[4]) {
    const uint8_t serial_be[4] = {
        (uint8_t)(serial >> 24U),
        (uint8_t)(serial >> 16U),
        (uint8_t)(serial >> 8U),
        (uint8_t)serial,
    };
    hitag2_build_iv(cnt, btn, seed, iv);
    hitag2_encrypt_from_iv(serial_be, iv, out);
}

static void hitag2_apply_check_remote(
    uint64_t data,
    uint64_t data_2,
    uint8_t recovered,
    uint32_t seed,
    uint32_t* serial,
    uint8_t* btn,
    uint32_t* cnt,
    uint32_t* hop,
    uint8_t* tail) {
    uint16_t cnt10 = 0;
    hitag2_unpack_frame(data, data_2, serial, btn, &cnt10, hop, tail);
    if(recovered == HITAG2_RECOVERED_YES) {
        *cnt = ((seed >> 12U) & 0xFF0U) | ((seed << 4U) & 0xF000U) | (seed >> 28U);
    } else {
        *cnt = cnt10;
    }
}

static void subghz_protocol_hitag2_check_remote_controller(SubGhzProtocolDecoderHitag2* instance) {
    hitag2_apply_check_remote(
        instance->generic.data,
        instance->generic.data_2,
        instance->recovered,
        instance->generic.seed,
        &instance->generic.serial,
        &instance->generic.btn,
        &instance->generic.cnt,
        &instance->hop,
        &instance->tail_bits);
}

static void
    hitag2_setup_button_editor(uint8_t btn, uint32_t cnt, uint8_t recovered, bool hell_key) {
    subghz_block_generic_global.current_btn = btn;
    if(subghz_block_generic_global.hitag2_replay_only) {
        subghz_block_generic_global.btn_is_available = false;
        subghz_block_generic_global.cnt_is_available = false;
        if(!subghz_custom_btn_has_original()) {
            subghz_custom_btn_set_original(btn);
        }
        subghz_custom_btn_set_max(1);
        return;
    }
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 4;
    if(hell_key) {
        subghz_block_generic_global.cnt_is_available = true;
        subghz_block_generic_global.cnt_length_bit = 10;
        subghz_block_generic_global.current_cnt = cnt & 0x3FFU;
    } else if(recovered == HITAG2_RECOVERED_YES) {
        subghz_block_generic_global.cnt_is_available = true;
        subghz_block_generic_global.cnt_length_bit = 16;
        subghz_block_generic_global.current_cnt = cnt;
    }
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(btn);
    }
    subghz_custom_btn_set_max(4);
}

static uint8_t hitag2_get_btn_code(void) {
    switch(subghz_custom_btn_get()) {
    case SUBGHZ_CUSTOM_BTN_OK:
        return subghz_custom_btn_get_original();
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x1;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x2;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return 0x4;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return 0x8;
    default:
        return subghz_custom_btn_get_original();
    }
}

static inline void hitag2_encoder_apply_button(uint8_t* btn) {
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(*btn);
    }
    *btn = hitag2_get_btn_code();
    subghz_block_generic_global_button_override_get(btn);
}

static inline void hitag2_encoder_apply_counter(uint32_t* cnt) {
    if(!subghz_block_generic_global_counter_override_get(cnt)) {
        *cnt = (*cnt + furi_hal_subghz_get_rolling_counter_mult()) & 0xFFFFU;
    }
}

static bool hitag2_encoder_hell_next_frame(SubGhzProtocolEncoderHitag2* instance) {
    uint8_t raw[11];
    uint32_t uid = 0;
    uint8_t btn = 0;
    uint16_t cnt10 = 0;
    uint32_t hop = 0;
    uint8_t tail = 0;
    hitag2_unpack_frame(
        instance->generic.data, instance->generic.data_2, &uid, &btn, &cnt10, &hop, &tail);

    uint32_t epoch = instance->hitag2_epoch;
    if(!hitag2_hell_find_epoch(instance->hitag2_key, uid, btn, cnt10, hop, &epoch)) {
        return false;
    }

    uint8_t tx_btn = btn;
    hitag2_encoder_apply_button(&tx_btn);
    if(tx_btn == 0U) {
        tx_btn = btn;
    }

    uint32_t full = (epoch << 10U) | (uint32_t)(cnt10 & 0x3FFU);
    uint32_t override = full;
    if(!subghz_block_generic_global_counter_override_get(&override)) {
        full += (uint32_t)furi_hal_subghz_get_rolling_counter_mult();
    } else {
        full = (epoch << 10U) | (override & 0x3FFU);
    }
    epoch = (full >> 10U) & 0x3FFFFUL;
    cnt10 = (uint16_t)(full & 0x3FFU);
    hop = hitag2_hell_authenticator(uid, tx_btn, full, instance->hitag2_key);

    hitag2_pack_hell_frame(uid, tx_btn, cnt10, hop, tail, raw);
    hitag2_apply_raw(raw, &instance->generic.data, &instance->generic.data_2);
    instance->generic.serial = uid;
    instance->generic.btn = tx_btn;
    instance->generic.cnt = cnt10;
    instance->hitag2_epoch = epoch;
    instance->tail_bits = tail;
    instance->hell_key_valid = true;
    return true;
}

static void hitag2_write_named_fields(
    FlipperFormat* ff,
    uint32_t serial,
    uint8_t btn,
    uint32_t cnt,
    uint32_t hop) {
    hitag2_flipper_u32(ff, "Serial", serial);
    hitag2_flipper_u32(ff, "Btn", btn);
    hitag2_flipper_u32(ff, "Cnt", cnt);
    hitag2_flipper_u32(ff, HITAG2_HOP_FIELD, hop);
}

void* subghz_protocol_encoder_hitag2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHitag2* instance = calloc(1, sizeof(SubGhzProtocolEncoderHitag2));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_renault_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 1;
    instance->encoder.size_upload = HITAG2_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    furi_check(instance->encoder.upload);
    instance->encoder.is_running = false;

    return instance;
}

void subghz_protocol_encoder_hitag2_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static bool
    hitag2_encoder_add_level(LevelDuration* upload, size_t* index, bool level, uint32_t duration) {
    if(*index > 0 && level_duration_get_level(upload[*index - 1U]) == level) {
        const uint32_t prev = level_duration_get_duration(upload[*index - 1U]);
        upload[*index - 1U] = level_duration_make(level, prev + duration);
        return true;
    }
    if(*index >= HITAG2_UPLOAD_CAPACITY) {
        return false;
    }
    upload[(*index)++] = level_duration_make(level, duration);
    return true;
}

static bool hitag2_encoder_add_bits(
    LevelDuration* upload,
    size_t* index,
    uint64_t value,
    uint8_t bit_count) {
    for(uint8_t i = bit_count; i > 0; i--) {
        const bool one = ((value >> (i - 1U)) & 1ULL) != 0;
        if(one) {
            if(!hitag2_encoder_add_level(upload, index, true, HITAG2_TE_US)) return false;
            if(!hitag2_encoder_add_level(upload, index, false, HITAG2_TE_US)) return false;
        } else {
            if(!hitag2_encoder_add_level(upload, index, false, HITAG2_TE_US)) return false;
            if(!hitag2_encoder_add_level(upload, index, true, HITAG2_TE_US)) return false;
        }
    }
    return true;
}

static bool subghz_protocol_encoder_hitag2_get_upload(SubGhzProtocolEncoderHitag2* instance) {
    furi_check(instance);

    size_t index = 0;
    LevelDuration* upload = instance->encoder.upload;
    const uint64_t key = instance->generic.data;
    const uint64_t key_2 = instance->generic.data_2 & 0xFFFFFFULL;

    for(size_t i = 0; i < HITAG2_PREAMBLE_PAIRS; i++) {
        if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_TE_US)) return false;
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_TE_US)) return false;
    }
    if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_TE_US)) return false;

    const uint8_t long_frames = hitag2_variant_long_frames(instance->tx_variant);
    for(uint8_t frame = 0; frame < long_frames; frame++) {
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_HEADER_LOW_US)) return false;
        if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_HEADER_HIGH_US)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, 1, HITAG2_HEADER_BITS)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, key, HITAG2_KEY_BITS)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, key_2, HITAG2_KEY2_BITS)) return false;
    }

    if(instance->tx_variant != HITAG2_VARIANT_KANGOO) {
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_LONG_TO_SHORT_GAP_US))
            return false;
    }

    const uint64_t short_key = (key >> 18U) & 0x3FFULL;
    for(uint8_t frame = 0; frame < HITAG2_SHORT_FRAMES; frame++) {
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_HEADER_LOW_US)) return false;
        if(!hitag2_encoder_add_level(upload, &index, true, HITAG2_HEADER_HIGH_US)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, 1, HITAG2_HEADER_BITS)) return false;
        if(!hitag2_encoder_add_bits(upload, &index, short_key, HITAG2_SHORT_KEY_BITS))
            return false;
        if(!hitag2_encoder_add_level(upload, &index, false, HITAG2_SHORT_GAP_US)) return false;
    }

    instance->encoder.size_upload = index;
    return true;
}

static void hitag2_read_recovered_and_seed(
    FlipperFormat* flipper_format,
    uint8_t* recovered,
    uint32_t* seed) {
    *recovered = 0;
    *seed = 0;

    uint8_t recovered_hex = 0;
    if(hitag2_read_hex_be(flipper_format, "Recovered", &recovered_hex, 1)) {
        *recovered = recovered_hex;
    } else {
        uint32_t recovered_u32 = 0;
        if(flipper_format_rewind(flipper_format) &&
           flipper_format_read_uint32(flipper_format, "Recovered", &recovered_u32, 1)) {
            *recovered = (uint8_t)recovered_u32;
        }
    }

    uint8_t seed_be[4] = {0};
    if(hitag2_read_hex_be(flipper_format, "Seed", seed_be, 4)) {
        *seed = (uint32_t)hitag2_bytes_to_u64_be(seed_be, 4);
        return;
    }

    if(flipper_format_rewind(flipper_format)) {
        flipper_format_read_uint32(flipper_format, "Seed", seed, 1);
    }
}

SubGhzProtocolStatus
    subghz_protocol_encoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_hitag2_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        uint8_t key2[8] = {0};
        if(!hitag2_read_hex_be(flipper_format, "Key_2", key2, sizeof(key2))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->generic.data_2 = hitag2_bytes_to_u64_be(key2, sizeof(key2));

        if(!subghz_block_generic_global.hitag2_replay_only) {
            (void)renault_v1_apply_stored_secret(flipper_format);
        }
        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        uint32_t hop = 0;
        uint8_t tail = 0;
        hitag2_apply_check_remote(
            instance->generic.data,
            instance->generic.data_2,
            instance->recovered,
            instance->generic.seed,
            &instance->generic.serial,
            &instance->generic.btn,
            &instance->generic.cnt,
            &hop,
            &tail);
        instance->tail_bits = tail;
        UNUSED(hop);

        instance->hell_key_valid = false;
        memset(instance->hitag2_key, 0, 6U);
        instance->hitag2_epoch = 0;
        if(!subghz_block_generic_global.hitag2_replay_only &&
           hitag2_read_hell_key(flipper_format, instance->hitag2_key, &instance->hitag2_epoch)) {
            (void)hitag2_encoder_hell_next_frame(instance);
        }

        instance->tx_variant = hitag2_read_variant(flipper_format);

        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        hitag2_setup_button_editor(
            instance->generic.btn,
            instance->generic.cnt,
            instance->recovered,
            instance->hell_key_valid);

        /* Hell 48-bit key and BCM seed are different secrets. Hell
         * wins when Hitag2 Key authenticates; otherwise Recovered=1
         * plus Seed (the IV) rebuilds the next 11-byte V1 frame. */
        if(!subghz_block_generic_global.hitag2_replay_only && !instance->hell_key_valid &&
           instance->recovered == HITAG2_RECOVERED_YES) {
            hitag2_encoder_apply_counter(&instance->generic.cnt);
            hitag2_encoder_apply_button(&instance->generic.btn);

            uint8_t out[11];
            uint8_t iv[4];
            hitag2_encrypt_frame(
                instance->generic.serial,
                instance->generic.cnt,
                instance->generic.btn,
                instance->generic.seed,
                out,
                iv);
            instance->generic.data = hitag2_bytes_to_u64_be(out, 8);
            instance->generic.data_2 = ((uint64_t)out[8] << 16U) | ((uint64_t)out[9] << 8U) |
                                       out[10];
            instance->generic.seed = ((uint32_t)iv[0] << 24U) | ((uint32_t)iv[1] << 16U) |
                                     ((uint32_t)iv[2] << 8U) | iv[3];
        }

        if(!subghz_protocol_encoder_hitag2_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }

        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data[8];
        hitag2_u64_to_bytes_be(instance->generic.data, key_data, 8);
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(key_data))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        hitag2_u64_to_bytes_be(instance->generic.data_2, key_data, 8);
        if(!flipper_format_update_hex(flipper_format, "Key_2", key_data, sizeof(key_data))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        if(instance->recovered == HITAG2_RECOVERED_YES) {
            const uint8_t recovered_hex = HITAG2_RECOVERED_YES;
            if(!flipper_format_insert_or_update_hex(
                   flipper_format, "Recovered", &recovered_hex, 1)) {
                ret = SubGhzProtocolStatusErrorParserOthers;
                break;
            }
            if(!hitag2_write_hex_be(flipper_format, "Seed", instance->generic.seed, 4)) {
                ret = SubGhzProtocolStatusErrorParserOthers;
                break;
            }
        }

        {
            uint32_t hop = 0;
            hitag2_unpack_frame(
                instance->generic.data, instance->generic.data_2, NULL, NULL, NULL, &hop, NULL);
            hitag2_write_named_fields(
                flipper_format,
                instance->generic.serial,
                instance->generic.btn,
                instance->generic.cnt,
                hop);
        }
        if(instance->hell_key_valid) {
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(
                flipper_format, HITAG2_HELL_KEY_FIELD, instance->hitag2_key, 6U);
            hitag2_flipper_u32(
                flipper_format, HITAG2_HELL_EPOCH_FIELD, instance->hitag2_epoch & 0x3FFFFUL);
        }
        hitag2_flipper_u32(flipper_format, HITAG2_TYPE_FIELD, instance->tx_variant);

        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_hitag2_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_hitag2_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderHitag2* instance = context;

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

void* subghz_protocol_decoder_hitag2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHitag2* instance = calloc(1, sizeof(SubGhzProtocolDecoderHitag2));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_renault_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_hitag2_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_hitag2_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    hitag2_decoder_notify(instance);
    instance->decoder.parser_step = Hitag2DecoderStepReset;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

static bool hitag2_duration_is_header_low(uint32_t duration) {
    return (duration >= HITAG2_HEADER_LOW_MIN_US) && (duration <= HITAG2_HEADER_LOW_MAX_US);
}

static uint32_t hitag2_data_threshold(uint16_t te) {
    const uint32_t triple = (uint32_t)te * 3U;
    if(triple < 300U) {
        return 150U;
    }
    if(triple >= 422U) {
        return 210U;
    }
    return triple / 2U;
}

static uint16_t hitag2_adapt_te(uint16_t te, uint32_t duration) {
    const uint32_t mixed = ((uint32_t)te * 7U) + duration;
    if(mixed < 560U) {
        return 70;
    }
    if(mixed >= 1488U) {
        return 185;
    }
    return (uint16_t)(mixed / 8U);
}

static bool hitag2_accept_frame(SubGhzProtocolDecoderHitag2* instance, uint64_t key_2) {
    if(instance->header != 1U) {
        return false;
    }

    uint8_t raw[11];
    hitag2_pack_key_bytes(instance->generic.data, key_2, raw);
    if(hitag2_frame_xor(raw) != raw[10]) {
        return false;
    }

    if(instance->last_frame_valid && instance->last_data == instance->generic.data &&
       instance->last_data_2 == key_2) {
        if(instance->identical_count < 0xFFU) {
            instance->identical_count++;
        }
        instance->tx_variant = (instance->identical_count >= HITAG2_KANGOO_LONG_FRAMES) ?
                                   HITAG2_VARIANT_KANGOO :
                                   HITAG2_VARIANT_OLD;
        return false;
    }

    instance->generic.data_2 = key_2;
    instance->generic.data_count_bit = HITAG2_MIN_COUNT_BIT;
    instance->recovered = 0;
    instance->generic.seed = 0;
    uint16_t cnt10 = 0;
    hitag2_unpack_frame(
        instance->generic.data,
        instance->generic.data_2,
        &instance->generic.serial,
        &instance->generic.btn,
        &cnt10,
        &instance->hop,
        &instance->tail_bits);
    instance->generic.cnt = cnt10;
    instance->last_data = instance->generic.data;
    instance->last_data_2 = key_2;
    instance->last_frame_valid = true;
    instance->identical_count = 1;
    instance->tx_variant = HITAG2_DEFAULT_VARIANT;
    instance->pending_notify = true;
    FURI_LOG_D(
        TAG,
        "Decode Sn:%08lX Btn:%X Cnt:%03X Hop:%08lX",
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        instance->generic.cnt & 0x3FFU,
        (unsigned long)instance->hop);
    hitag2_decoder_notify(instance);
    return true;
}

void subghz_protocol_decoder_hitag2_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;

    while(true) {
        switch(instance->decoder.parser_step) {
        case Hitag2DecoderStepReset:
            if((!level) && hitag2_duration_is_header_low(duration)) {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = Hitag2DecoderStepCheckSync;
            }
            return;

        case Hitag2DecoderStepCheckSync:
            if(level) {
                if((duration < HITAG2_HEADER_HIGH_MIN_US) ||
                   (duration > HITAG2_HEADER_HIGH_MAX_US) ||
                   (instance->decoder.te_last < HITAG2_HEADER_LOW_MIN_US) ||
                   (instance->decoder.te_last > HITAG2_HEADER_LOW_MAX_US)) {
                    instance->decoder.parser_step = Hitag2DecoderStepReset;
                    return;
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->manchester_state = ManchesterStateStart1;
                instance->te_high = HITAG2_TE_HIGH_INIT_US;
                instance->te_low = HITAG2_TE_LOW_INIT_US;
                instance->decoder.parser_step = Hitag2DecoderStepData;
                return;
            }
            instance->decoder.parser_step = Hitag2DecoderStepReset;
            if(hitag2_duration_is_header_low(duration)) {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = Hitag2DecoderStepCheckSync;
            }
            return;

        case Hitag2DecoderStepData:
            if(duration <= HITAG2_DATA_IGNORE_US) {
                return;
            }
            if(duration > HITAG2_DATA_RESET_US) {
                instance->decoder.parser_step = Hitag2DecoderStepReset;
                continue;
            }

            uint16_t* te = level ? &instance->te_high : &instance->te_low;
            ManchesterEvent event;
            if(duration > hitag2_data_threshold(*te)) {
                event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
            } else {
                event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                *te = hitag2_adapt_te(*te, duration);
            }

            bool bit = false;
            if(!manchester_advance(
                   instance->manchester_state, event, &instance->manchester_state, &bit)) {
                return;
            }

            instance->decoder.decode_data = (instance->decoder.decode_data << 1U) |
                                            (bit ? 1ULL : 0ULL);
            instance->decoder.decode_count_bit++;

            if(instance->decoder.decode_count_bit == HITAG2_HEADER_BITS) {
                instance->header = (uint16_t)~instance->decoder.decode_data;
                instance->decoder.decode_data = 0;
                return;
            }
            if(instance->decoder.decode_count_bit == HITAG2_KEY_END_BITS) {
                instance->generic.data = ~instance->decoder.decode_data;
                instance->decoder.decode_data = 0;
                return;
            }
            if(instance->decoder.decode_count_bit == HITAG2_LONG_FRAME_BITS) {
                const uint64_t key_2 = (~instance->decoder.decode_data) & 0xFFFFFFULL;
                (void)hitag2_accept_frame(instance, key_2);
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = Hitag2DecoderStepData;
            }
            return;

        default:
            instance->decoder.parser_step = Hitag2DecoderStepReset;
            return;
        }
    }
}

static uint8_t subghz_protocol_decoder_hitag2_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    uint8_t hash = 0;
    const uint8_t* p = (const uint8_t*)&instance->header;
    hash ^= p[0];
    hash ^= p[1];
    p = (const uint8_t*)&instance->generic.data;
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        hash ^= p[i];
    }
    p = (const uint8_t*)&instance->generic.data_2;
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        hash ^= p[i];
    }
    return hash;
}

static SubGhzProtocolStatus hitag2_write_extra_fields(
    FlipperFormat* flipper_format,
    uint64_t key_2,
    uint8_t recovered,
    uint32_t seed) {
    if(!hitag2_write_hex_be(flipper_format, "Key_2", key_2, 8)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    uint8_t recovered_hex = 0;
    if(recovered == HITAG2_RECOVERED_YES) {
        recovered_hex = HITAG2_RECOVERED_YES;
    } else if(recovered == HITAG2_RECOVERED_BF_MISS) {
        recovered_hex = HITAG2_RECOVERED_BF_MISS;
    }
    if(!flipper_format_insert_or_update_hex(flipper_format, "Recovered", &recovered_hex, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    if(!hitag2_write_hex_be(flipper_format, "Seed", seed, 4)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_hitag2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    subghz_protocol_hitag2_check_remote_controller(instance);

    const uint32_t serial = instance->generic.serial;
    const uint8_t btn = instance->generic.btn;
    const uint32_t cnt = instance->generic.cnt;
    const uint32_t seed = instance->generic.seed;
    const uint64_t data_2 = instance->generic.data_2;
    instance->generic.serial = 0;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;
    instance->generic.seed = 0;
    instance->generic.data_2 = 0;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = cnt;
    instance->generic.seed = seed;
    instance->generic.data_2 = data_2;
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    ret = hitag2_write_extra_fields(
        flipper_format, instance->generic.data_2, instance->recovered, instance->generic.seed);
    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }
    hitag2_write_named_fields(flipper_format, serial, btn, cnt, instance->hop);
    hitag2_flipper_u32(flipper_format, HITAG2_TYPE_FIELD, instance->tx_variant);
    if(instance->hell_key_valid) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(
            flipper_format, HITAG2_HELL_KEY_FIELD, instance->hitag2_key, 6U);
        hitag2_flipper_u32(
            flipper_format, HITAG2_HELL_EPOCH_FIELD, instance->hitag2_epoch & 0x3FFFFUL);
    }
    const bool had_secret = instance->hell_key_valid ||
                            instance->recovered == HITAG2_RECOVERED_YES;
    if(renault_v1_apply_stored_secret(flipper_format) && !had_secret) {
        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);
        if(hitag2_read_hell_key(flipper_format, instance->hitag2_key, &instance->hitag2_epoch)) {
            instance->hell_key_valid = hitag2_hell_find_epoch(
                instance->hitag2_key,
                serial,
                btn,
                (uint16_t)(cnt & 0x3FFU),
                instance->hop,
                &instance->hitag2_epoch);
        }
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_hitag2_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        uint8_t key2[8] = {0};
        if(!hitag2_read_hex_be(flipper_format, "Key_2", key2, sizeof(key2))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->generic.data_2 = hitag2_bytes_to_u64_be(key2, sizeof(key2));

        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);

        subghz_protocol_hitag2_check_remote_controller(instance);
        (void)renault_v1_apply_stored_secret(flipper_format);
        hitag2_read_recovered_and_seed(
            flipper_format, &instance->recovered, &instance->generic.seed);
        subghz_protocol_hitag2_check_remote_controller(instance);
        instance->hell_key_valid = false;
        memset(instance->hitag2_key, 0, 6U);
        instance->hitag2_epoch = 0;
        if(hitag2_read_hell_key(flipper_format, instance->hitag2_key, &instance->hitag2_epoch)) {
            instance->hell_key_valid = hitag2_hell_find_epoch(
                instance->hitag2_key,
                instance->generic.serial,
                instance->generic.btn,
                (uint16_t)(instance->generic.cnt & 0x3FFU),
                instance->hop,
                &instance->hitag2_epoch);
        }
        instance->tx_variant = hitag2_read_variant(flipper_format);
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_decoder_hitag2_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHitag2* instance = context;

    subghz_protocol_hitag2_check_remote_controller(instance);
    hitag2_setup_button_editor(
        instance->generic.btn,
        instance->generic.cnt,
        instance->recovered,
        instance->hell_key_valid);

    furi_string_printf(
        output,
        "%s\r\nK1:%016llX\r\nK2:%06llX Sn:%08lX\r\nBtn:%02X [%s] %db T:%u",
        instance->generic.protocol_name,
        (unsigned long long)instance->generic.data,
        (unsigned long long)(instance->generic.data_2 & 0xFFFFFFULL),
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        hitag2_get_button_name(instance->generic.btn),
        instance->generic.data_count_bit,
        instance->tx_variant);

    if(instance->recovered == HITAG2_RECOVERED_YES) {
        furi_string_cat_printf(
            output,
            "\r\nIV:%08lX Cnt:%04lX",
            (unsigned long)instance->generic.seed,
            (unsigned long)(instance->generic.cnt & 0xFFFFU));
    } else if(instance->recovered == HITAG2_RECOVERED_BF_MISS) {
        furi_string_cat_printf(output, "\r\nBF not found");
    } else {
        furi_string_cat_printf(
            output,
            "\r\nCnt:%03lX Hop:%08lX",
            (unsigned long)(instance->generic.cnt & 0x3FFU),
            (unsigned long)instance->hop);
        if(instance->hell_key_valid) {
            furi_string_cat_printf(output, "\r\nHell");
        } else {
            furi_string_cat_printf(output, "\r\nHell (key needed)");
        }
    }
}

#define HITAG2_BF_PROGRESS_INTERVAL 0x400U
#define HITAG2_BF_CANDIDATES        0x40000U

static uint32_t hitag2_seed_from_iv(const uint8_t iv[4]) {
    return ((uint32_t)iv[0] << 24U) | ((uint32_t)iv[1] << 16U) | ((uint32_t)iv[2] << 8U) | iv[3];
}

static void hitag2_bf_rearrange_dest(uint8_t dest[11]) {
    const uint8_t frame4 = dest[6];
    uint8_t frame5 = dest[5];
    uint8_t frame6 = dest[4];
    uint8_t frame7 = dest[3];
    const uint8_t frame8 = dest[2];
    const uint8_t frame9 = dest[1];

    uint8_t hop0 = (uint8_t)(((frame4 & 1U) << 7U) | (frame5 >> 1U));
    frame5 = (uint8_t)(((frame5 & 1U) << 7U) | (frame6 >> 1U));
    const uint8_t hop0_hi = (uint8_t)(hop0 >> 1U);
    hop0 = (uint8_t)(hop0 & 1U);
    hop0 = (uint8_t)((hop0 << 7U) | (frame5 >> 1U));
    frame6 = (uint8_t)(((frame6 & 1U) << 7U) | (frame7 >> 1U));
    frame5 = (uint8_t)(((frame5 & 1U) << 7U) | (frame6 >> 1U));
    dest[3] = frame5;
    frame7 = (uint8_t)(((frame7 & 1U) << 7U) | (frame8 >> 1U));
    const uint8_t hop2 = (uint8_t)(((frame6 & 1U) << 7U) | (frame7 >> 1U));
    frame6 = (uint8_t)(((frame8 & 1U) << 7U) | (frame9 >> 1U));
    dest[6] = (uint8_t)(((frame9 & 1U) << 7U) | (frame4 >> 2U));
    dest[1] = (uint8_t)(((frame7 & 1U) << 7U) | (frame6 >> 1U));
    dest[5] = (uint8_t)(hop0_hi | (((frame4 >> 1U) & 1U) << 7U));
    dest[2] = hop2;
    dest[4] = hop0;
}

static void hitag2_bf_prepare(
    const uint8_t frame[11],
    uint8_t serial_be[4],
    uint8_t perm[6],
    uint8_t hop_target[4],
    uint8_t* iv0,
    uint8_t* fp) {
    uint8_t dest[11];
    for(size_t i = 0; i < 11; i++) {
        dest[i] = frame[10U - i];
    }
    hitag2_bf_rearrange_dest(dest);

    hop_target[0] = dest[1];
    hop_target[1] = dest[2];
    hop_target[2] = dest[3];
    hop_target[3] = dest[4];
    *iv0 = (uint8_t)((frame[4] >> 4U) | (dest[5] << 4U));
    *fp = (uint8_t)((dest[5] >> 4U) | ((dest[6] & 3U) << 4U));

    serial_be[0] = frame[0];
    serial_be[1] = frame[1];
    serial_be[2] = frame[2];
    serial_be[3] = frame[3];
    hitag2_serial_permute(serial_be, perm);
}

static bool hitag2_bf_hop_matches(
    const uint8_t serial_be[4],
    const uint8_t perm[6],
    const uint8_t iv[4],
    const uint8_t hop_target[4]) {
    uint8_t state[6];
    uint8_t iv_work[4];
    uint8_t iv_orig[4];

    state[0] = serial_be[0];
    state[1] = serial_be[1];
    state[2] = serial_be[2];
    state[3] = serial_be[3];
    state[4] = perm[4];
    state[5] = perm[5];
    iv_work[0] = perm[0];
    iv_work[1] = perm[1];
    iv_work[2] = perm[2];
    iv_work[3] = perm[3];
    iv_orig[0] = iv[0];
    iv_orig[1] = iv[1];
    iv_orig[2] = iv[2];
    iv_orig[3] = iv[3];
    hitag2_clock_cipher(state, iv_work, iv_orig);

    return (iv_work[0] == hop_target[0]) && (iv_work[1] == hop_target[1]) &&
           (iv_work[2] == hop_target[2]) && (iv_work[3] == hop_target[3]);
}

void hitag2_brute_force_run(Hitag2BfState* state) {
    furi_check(state);

    uint8_t serial_be[4];
    uint8_t perm[6];
    uint8_t hop_target[4];
    uint8_t iv0 = 0;
    uint8_t fp = 0;
    hitag2_bf_prepare(state->frame, serial_be, perm, hop_target, &iv0, &fp);

    state->progress_current = 0;
    state->progress_total = HITAG2_BF_CANDIDATES;
    state->status = HITAG2_BF_STATUS_RUNNING;

    uint8_t iv[4];
    for(uint32_t cand = 0; cand < HITAG2_BF_CANDIDATES; cand++) {
        if((cand & (HITAG2_BF_PROGRESS_INTERVAL - 1U)) == 0U) {
            state->progress_current = cand;
            if(state->cancel) {
                state->status = (uint8_t)(state->cancel + 3U);
                return;
            }
        }

        iv[0] = iv0;
        iv[1] = (uint8_t)(fp | ((cand & 3U) << 6U));
        iv[2] = (uint8_t)(cand >> 2U);
        iv[3] = (uint8_t)(cand >> 10U);
        if(hitag2_bf_hop_matches(serial_be, perm, iv, hop_target)) {
            state->iv[0] = iv[0];
            state->iv[1] = iv[1];
            state->iv[2] = iv[2];
            state->iv[3] = iv[3];
            state->progress_current = HITAG2_BF_CANDIDATES;
            state->status = HITAG2_BF_STATUS_FOUND;
            return;
        }
    }

    state->progress_current = HITAG2_BF_CANDIDATES;
    state->status = (uint8_t)(state->cancel + 3U);
}

int32_t hitag2_brute_force_thread_entry(void* arg) {
    Hitag2BfState* state = arg;
    hitag2_brute_force_run(state);
    if(state->on_done) {
        state->on_done(state->on_done_ctx);
    }
    return 0;
}

bool hitag2_bf_state_from_flipper_format(Hitag2BfState* state, FlipperFormat* ff) {
    furi_check(state);
    furi_check(ff);

    uint8_t key1[8] = {0};
    uint8_t key2[8] = {0};
    if(!hitag2_read_hex_be(ff, "Key", key1, sizeof(key1))) {
        return false;
    }
    if(!hitag2_read_hex_be(ff, "Key_2", key2, sizeof(key2))) {
        return false;
    }

    for(size_t i = 0; i < 8; i++) {
        state->frame[i] = key1[i];
    }
    state->frame[8] = key2[5];
    state->frame[9] = key2[6];
    state->frame[10] = key2[7];

    state->cancel = 0;
    state->progress_current = 0;
    state->progress_total = HITAG2_BF_CANDIDATES;
    state->status = HITAG2_BF_STATUS_IDLE;
    state->on_done = NULL;
    state->on_done_ctx = NULL;
    state->iv[0] = 0;
    state->iv[1] = 0;
    state->iv[2] = 0;
    state->iv[3] = 0;
    return true;
}

bool hitag2_bf_needs_bruteforce(FlipperFormat* ff, bool require_hitag2_protocol) {
    if(!ff) {
        return false;
    }

    FuriString* s = furi_string_alloc();
    flipper_format_rewind(ff);
    if(require_hitag2_protocol) {
        if(!flipper_format_read_string(ff, "Protocol", s) ||
           !furi_string_equal_str(s, SUBGHZ_PROTOCOL_RENAULT_V1_NAME)) {
            furi_string_free(s);
            return false;
        }
        flipper_format_rewind(ff);
    }

    bool has_key = flipper_format_read_string(ff, "Key", s);
    furi_string_free(s);
    if(!has_key) {
        return false;
    }

    uint8_t recovered = 0;
    uint32_t seed = 0;
    hitag2_read_recovered_and_seed(ff, &recovered, &seed);
    UNUSED(seed);
    if(recovered != 0) {
        return false;
    }
    uint8_t hell_key[6];
    uint32_t epoch = 0;
    if(hitag2_read_hell_key(ff, hell_key, &epoch)) {
        return false;
    }
    return true;
}

bool hitag2_bf_patch_flipper_format_on_success(FlipperFormat* ff, const Hitag2BfState* state) {
    if(!ff || !state) {
        return false;
    }

    flipper_format_rewind(ff);
    const uint8_t recovered_hex = HITAG2_RECOVERED_YES;
    if(!flipper_format_insert_or_update_hex(ff, "Recovered", &recovered_hex, 1)) {
        return false;
    }
    const uint32_t seed = hitag2_seed_from_iv(state->iv);
    if(!hitag2_write_hex_be(ff, "Seed", seed, 4)) {
        return false;
    }

    uint32_t serial = 0U;
    flipper_format_rewind(ff);
    if(!flipper_format_read_uint32(ff, "Serial", &serial, 1) || serial == 0U ||
       serial == UINT32_MAX) {
        uint8_t key_be[8] = {0};
        if(hitag2_read_hex_be(ff, "Key", key_be, sizeof(key_be))) {
            serial = (uint32_t)(hitag2_bytes_to_u64_be(key_be, sizeof(key_be)) >> 32U);
        }
    }
    if(serial != 0U && serial != UINT32_MAX) {
        hitag2_key_db_save_seed(serial, seed);
    }
    return true;
}

bool hitag2_bf_patch_flipper_format_on_miss(FlipperFormat* ff) {
    if(!ff) {
        return false;
    }

    flipper_format_rewind(ff);
    const uint8_t recovered_hex = HITAG2_RECOVERED_BF_MISS;
    return flipper_format_insert_or_update_hex(ff, "Recovered", &recovered_hex, 1);
}

static bool renault_v1_frame_from_flipper(
    FlipperFormat* ff,
    uint32_t* uid,
    uint8_t* btn,
    uint16_t* cnt,
    uint32_t* hop) {
    uint8_t key[8] = {0};
    uint8_t key2[8] = {0};
    if(!hitag2_read_hex_be(ff, "Key", key, sizeof(key))) return false;
    if(!hitag2_read_hex_be(ff, "Key_2", key2, sizeof(key2))) return false;
    const uint64_t data = hitag2_bytes_to_u64_be(key, sizeof(key));
    const uint64_t data_2 = hitag2_bytes_to_u64_be(key2, sizeof(key2));
    hitag2_unpack_frame(data, data_2, uid, btn, cnt, hop, NULL);
    return *uid != 0U && *uid != UINT32_MAX;
}

static void renault_v1_remember_key(uint32_t uid, const uint8_t key[6], uint32_t epoch) {
    Hitag2KeyDbRecord existing = {0};
    if(!hitag2_key_db_find(uid, &existing)) {
        hitag2_key_db_save_key(uid, key, epoch);
    }
}

static void renault_v1_remember_seed(uint32_t uid, uint32_t seed) {
    Hitag2KeyDbRecord existing = {0};
    if(!hitag2_key_db_find(uid, &existing)) {
        hitag2_key_db_save_seed(uid, seed);
    }
}

static bool renault_v1_apply_stored_secret(FlipperFormat* ff) {
    uint32_t uid = 0U;
    uint8_t btn = 0U;
    uint16_t cnt = 0U;
    uint32_t hop = 0U;
    if(!ff || !renault_v1_frame_from_flipper(ff, &uid, &btn, &cnt, &hop)) return false;

    uint8_t key[6] = {0};
    uint32_t epoch = 0U;
    if(hitag2_read_hell_key(ff, key, &epoch) &&
       hitag2_hell_find_epoch(key, uid, btn, cnt, hop, &epoch)) {
        renault_v1_remember_key(uid, key, epoch);
        hitag2_flipper_u32(ff, HITAG2_HELL_EPOCH_FIELD, epoch & 0x3FFFFUL);
        return true;
    }

    uint8_t recovered = 0U;
    uint32_t seed = 0U;
    hitag2_read_recovered_and_seed(ff, &recovered, &seed);
    if(recovered == HITAG2_RECOVERED_YES) {
        renault_v1_remember_seed(uid, seed);
        return true;
    }

    Hitag2KeyDbRecord record = {0};
    if(!hitag2_key_db_find(uid, &record)) return false;

    if(record.has_key) {
        uint32_t db_epoch = record.epoch & 0x3FFFFUL;
        if(hitag2_hell_find_epoch(record.key, uid, btn, cnt, hop, &db_epoch)) {
            flipper_format_rewind(ff);
            if(flipper_format_insert_or_update_hex(ff, HITAG2_HELL_KEY_FIELD, record.key, 6U)) {
                hitag2_flipper_u32(ff, HITAG2_HELL_EPOCH_FIELD, db_epoch);
                return true;
            }
        }
    }
    if(record.has_seed) {
        const uint8_t recovered_hex = HITAG2_RECOVERED_YES;
        flipper_format_rewind(ff);
        if(!flipper_format_insert_or_update_hex(ff, "Recovered", &recovered_hex, 1)) return false;
        return hitag2_write_hex_be(ff, "Seed", record.seed, 4);
    }
    return false;
}

bool renault_v1_flipper_needs_hitag2_key(FlipperFormat* flipper_format) {
    if(!flipper_format) {
        return false;
    }

    FuriString* proto = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_string(flipper_format, "Protocol", proto) ||
       !furi_string_equal_str(proto, SUBGHZ_PROTOCOL_RENAULT_V1_NAME)) {
        furi_string_free(proto);
        return false;
    }
    furi_string_free(proto);

    return !renault_v1_apply_stored_secret(flipper_format);
}

bool renault_v1_flipper_parse_hitag2_key_text(const char* text, uint8_t key[6]) {
    if(!text || !key || strlen(text) != 12U) {
        return false;
    }
    for(size_t i = 0; i < 6U; i++) {
        char byte_str[3] = {text[i * 2U], text[(i * 2U) + 1U], '\0'};
        char* end = NULL;
        const unsigned long val = strtoul(byte_str, &end, 16);
        if(end != byte_str + 2 || val > 0xFFUL) {
            return false;
        }
        key[i] = (uint8_t)val;
    }
    return true;
}

bool renault_v1_flipper_write_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) {
        return false;
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, HITAG2_HELL_KEY_FIELD, key, 6U)) {
        return false;
    }
    uint32_t epoch = 0U;
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_uint32(flipper_format, HITAG2_HELL_EPOCH_FIELD, &epoch, 1U)) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(
            flipper_format, HITAG2_HELL_EPOCH_FIELD, &epoch, 1U);
    }
    return true;
}

bool renault_v1_flipper_accept_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) return false;

    FuriString* proto = furi_string_alloc();
    flipper_format_rewind(flipper_format);
    const bool is_renault = flipper_format_read_string(flipper_format, "Protocol", proto) &&
                            furi_string_equal_str(proto, SUBGHZ_PROTOCOL_RENAULT_V1_NAME);
    furi_string_free(proto);
    if(!is_renault) return false;

    uint32_t uid = 0U;
    uint8_t btn = 0U;
    uint16_t cnt = 0U;
    uint32_t hop = 0U;
    if(!renault_v1_frame_from_flipper(flipper_format, &uid, &btn, &cnt, &hop)) return false;

    uint32_t epoch = 0U;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, HITAG2_HELL_EPOCH_FIELD, &epoch, 1)) {
        epoch &= 0x3FFFFUL;
    }
    if(!hitag2_hell_find_epoch(key, uid, btn, cnt, hop, &epoch)) {
        epoch = 0U;
        if(!hitag2_hell_find_epoch(key, uid, btn, cnt, hop, &epoch)) return false;
    }

    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, HITAG2_HELL_KEY_FIELD, key, 6U)) {
        return false;
    }
    hitag2_flipper_u32(flipper_format, HITAG2_HELL_EPOCH_FIELD, epoch & 0x3FFFFUL);
    hitag2_key_db_save_key(uid, key, epoch);
    return true;
}
