#pragma once

#include "kia_generic.h"

#define SUBGHZ_PROTOCOL_KIA_V0_NAME "Kia V0"

typedef struct SubGhzProtocolDecoderKIA SubGhzProtocolDecoderKIA;
typedef struct SubGhzProtocolEncoderKIA SubGhzProtocolEncoderKIA;

extern const SubGhzProtocolDecoder subghz_protocol_kia_v0_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_kia_v0_encoder;
extern const SubGhzProtocol subghz_protocol_kia_v0;

// Decoder functions
uint8_t subghz_protocol_decoder_kia_v0_get_hash_data(void* context);
void subghz_protocol_encoder_kia_v0_free(void* context);
void subghz_protocol_encoder_kia_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v0_yield(void* context);
void* subghz_protocol_decoder_kia_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v0_free(void* context);
void subghz_protocol_decoder_kia_v0_reset(void* context);
void subghz_protocol_decoder_kia_v0_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v0_get_string(void* context, FuriString* output);

// Encoder helper functions
void subghz_protocol_encoder_kia_v0_set_button(void* context, uint8_t button);
void subghz_protocol_encoder_kia_v0_set_counter(void* context, uint16_t counter);
void subghz_protocol_encoder_kia_v0_increment_counter(void* context);
uint16_t subghz_protocol_encoder_kia_v0_get_counter(void* context);
uint8_t subghz_protocol_encoder_kia_v0_get_button(void* context);
