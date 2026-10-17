#pragma once

#include "kia_generic.h"

#define SUBGHZ_PROTOCOL_KIA_V2_NAME "Kia V2"

typedef struct SubGhzProtocolDecoderKiaV2 SubGhzProtocolDecoderKiaV2;
typedef struct SubGhzProtocolEncoderKiaV2 SubGhzProtocolEncoderKiaV2;

extern const SubGhzProtocolDecoder subghz_protocol_kia_v2_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_v2_encoder;
extern const SubGhzProtocol subghz_protocol_kia_v2;

uint8_t subghz_protocol_decoder_kia_v2_get_hash_data(void* context);
void subghz_protocol_encoder_kia_v2_free(void* context);
void subghz_protocol_encoder_kia_v2_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v2_yield(void* context);

void* subghz_protocol_decoder_kia_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v2_free(void* context);
void subghz_protocol_decoder_kia_v2_reset(void* context);
void subghz_protocol_decoder_kia_v2_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v2_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v2_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v2_deserialize(void* context, FlipperFormat* flipper_format);
