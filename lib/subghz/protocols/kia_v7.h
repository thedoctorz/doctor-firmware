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
#include <lib/toolbox/level_duration.h>

#define SUBGHZ_PROTOCOL_KIA_V7_NAME "Kia V7"

typedef struct SubGhzProtocolDecoderKiaV7 SubGhzProtocolDecoderKiaV7;
typedef struct SubGhzProtocolEncoderKiaV7 SubGhzProtocolEncoderKiaV7;

extern const SubGhzProtocolDecoder subghz_protocol_kia_v7_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_v7_encoder;
extern const SubGhzProtocol subghz_protocol_kia_v7;

uint8_t subghz_protocol_decoder_kia_v7_get_hash_data(void* context);
void subghz_protocol_encoder_kia_v7_free(void* context);
void subghz_protocol_encoder_kia_v7_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v7_yield(void* context);

void* subghz_protocol_decoder_kia_v7_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v7_free(void* context);
void subghz_protocol_decoder_kia_v7_reset(void* context);
void subghz_protocol_decoder_kia_v7_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v7_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v7_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v7_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v7_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v7_deserialize(void* context, FlipperFormat* flipper_format);
