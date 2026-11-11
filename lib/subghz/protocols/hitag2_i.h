#pragma once

#include "hitag2.h"
#include "hitag2_crypto.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define FIAT_V1_WIRE_BYTES          13U
#define FIAT_V1_LOGICAL_BITS        102U
#define FIAT_V1_RAW_FIELD           "Raw"
#define FIAT_V1_HOP_FIELD           "Hop"
#define FIAT_V1_HITAG2_KEY_FIELD    "Hitag2 Key"
#define FIAT_V1_HITAG2_EPOCH_FIELD  "Hitag2 Epoch"
#define FIAT_V1_KNOWN_KEY_COUNT     9U
#define HITAG2_RECOVERED_YES        1U
#define HITAG2_RECOVERED_BF_MISS    2U
#define HITAG2_HELL_KEY_FIELD       "Hitag2 Key"
#define HITAG2_HELL_EPOCH_FIELD     "Hitag2 Epoch"
#define HITAG2_HOP_FIELD            "Hop"
#define HITAG2_BF_PROGRESS_INTERVAL 0x400U
#define HITAG2_BF_CANDIDATES        0x40000U

bool fiat_v1_button_valid(uint8_t button);

uint32_t fiat_v1_uid(const uint8_t raw[FIAT_V1_WIRE_BYTES]);

uint32_t fiat_v1_counter(const uint8_t raw[FIAT_V1_WIRE_BYTES]);

uint32_t fiat_v1_hop(const uint8_t raw[FIAT_V1_WIRE_BYTES]);

bool fiat_v1_frame_valid(const uint8_t raw[FIAT_V1_WIRE_BYTES]);

void hitag2_u64_to_bytes_be(uint64_t value, uint8_t* out, size_t nbytes);

uint64_t hitag2_bytes_to_u64_be(const uint8_t* data, size_t nbytes);

uint8_t hitag2_frame_xor(const uint8_t raw[11]);

void hitag2_unpack_frame(
    uint64_t data,
    uint64_t data_2,
    uint32_t* serial,
    uint8_t* btn,
    uint16_t* cnt10,
    uint32_t* hop,
    uint8_t* tail);

bool hitag2_flipper_read_u32(FlipperFormat* ff, const char* name, uint32_t* value);

void hitag2_flipper_u32(FlipperFormat* ff, const char* key, uint32_t value);

SubGhzProtocolStatus fiat_v1_verify_protocol_name(FlipperFormat* ff, const char* expected_name);

void fiat_v1_encoder_read_fields(
    FlipperFormat* ff,
    uint32_t* serial_out,
    uint32_t* btn_out,
    uint32_t* cnt_out,
    uint32_t* type_out);

uint32_t fiat_v1_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat);

SubGhzProtocolStatus
    fiat_v1_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix);

SubGhzProtocolStatus fiat_v1_encoder_read_bit(
    FlipperFormat* ff,
    const uint16_t* allowed_bits,
    size_t allowed_bits_count,
    uint32_t* out_bit);

void fiat_v1_hitag2_key_cipher_to_disk(const uint8_t cipher[6], uint8_t disk[6]);

bool fiat_v1_resolve_hitag2_key(
    FlipperFormat* ff,
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t hop,
    uint8_t key_out[6],
    uint32_t* epoch_out);

bool hitag2_read_hell_key(FlipperFormat* ff, uint8_t key[6], uint32_t* epoch_out);

bool hitag2_hell_find_epoch(
    const uint8_t key[6],
    uint32_t uid,
    uint8_t btn,
    uint16_t cnt10,
    uint32_t hop,
    uint32_t* epoch_out);

bool hitag2_read_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint8_t* data,
    size_t nbytes);

bool hitag2_write_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint64_t value,
    size_t nbytes);

void hitag2_write_named_fields(
    FlipperFormat* ff,
    uint32_t serial,
    uint8_t btn,
    uint32_t cnt,
    uint32_t hop);

void hitag2_read_recovered_and_seed(
    FlipperFormat* flipper_format,
    uint8_t* recovered,
    uint32_t* seed);

SubGhzProtocolStatus hitag2_write_extra_fields(
    FlipperFormat* flipper_format,
    uint64_t key_2,
    uint8_t recovered,
    uint32_t seed);

bool renault_v1_apply_stored_secret(FlipperFormat* ff);
