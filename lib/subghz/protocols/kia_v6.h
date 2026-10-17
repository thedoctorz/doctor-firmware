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
#include "kia_generic.h"

#define SUBGHZ_PROTOCOL_KIA_V6_NAME "Kia V6"

typedef struct SubGhzProtocolDecoderKiaV6 SubGhzProtocolDecoderKiaV6;
typedef struct SubGhzProtocolEncoderKiaV6 SubGhzProtocolEncoderKiaV6;

extern const SubGhzProtocolDecoder subghz_protocol_kia_v6_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_v6_encoder;
extern const SubGhzProtocol subghz_protocol_kia_v6;

// Decoder functions
void subghz_protocol_encoder_kia_v6_free(void* context);
void subghz_protocol_encoder_kia_v6_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v6_yield(void* context);
void* subghz_protocol_decoder_kia_v6_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v6_free(void* context);
void subghz_protocol_decoder_kia_v6_reset(void* context);
void subghz_protocol_decoder_kia_v6_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_kia_v6_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v6_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v6_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v6_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v6_deserialize(void* context, FlipperFormat* flipper_format);
