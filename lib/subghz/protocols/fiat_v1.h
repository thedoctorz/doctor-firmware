#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>

#define SUBGHZ_PROTOCOL_FIAT_V1_NAME "Fiat V1"

typedef struct SubGhzProtocolDecoderFiatV1 SubGhzProtocolDecoderFiatV1;
typedef struct SubGhzProtocolEncoderFiatV1 SubGhzProtocolEncoderFiatV1;

extern const SubGhzProtocol subghz_protocol_fiat_v1;

void* subghz_protocol_decoder_fiat_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_fiat_v1_reset(void* context);
void subghz_protocol_decoder_fiat_v1_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_fiat_v1_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_fiat_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_fiat_v1_get_string(void* context, FuriString* output);

bool fiat_v1_flipper_needs_hitag2_key(FlipperFormat* flipper_format);
bool fiat_v1_flipper_parse_hitag2_key_text(const char* text, uint8_t key[6]);
bool fiat_v1_flipper_write_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]);
bool fiat_v1_flipper_accept_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]);

void* subghz_protocol_encoder_fiat_v1_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_v1_deserialize(void* context, FlipperFormat* flipper_format);
