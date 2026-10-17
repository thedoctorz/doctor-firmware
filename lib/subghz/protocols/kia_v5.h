#pragma once

#include "kia_generic.h"

#define SUBGHZ_PROTOCOL_KIA_V5_NAME "Kia V5"

typedef struct SubGhzProtocolDecoderKiaV5 SubGhzProtocolDecoderKiaV5;
typedef struct SubGhzProtocolEncoderKiaV5 SubGhzProtocolEncoderKiaV5;

extern const SubGhzProtocolDecoder subghz_protocol_kia_v5_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_v5_encoder;
extern const SubGhzProtocol subghz_protocol_kia_v5;

uint8_t subghz_protocol_decoder_kia_v5_get_hash_data(void* context);
void subghz_protocol_encoder_kia_v5_free(void* context);
void subghz_protocol_encoder_kia_v5_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v5_yield(void* context);

void* subghz_protocol_decoder_kia_v5_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v5_free(void* context);
void subghz_protocol_decoder_kia_v5_reset(void* context);
void subghz_protocol_decoder_kia_v5_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v5_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_kia_v5_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v5_deserialize(void* context, FlipperFormat* flipper_format);
