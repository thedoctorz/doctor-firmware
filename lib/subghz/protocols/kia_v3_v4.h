#pragma once

#include "kia_generic.h"

#define SUBGHZ_PROTOCOL_KIA_V3_V4_NAME "Kia V3/V4"

extern const SubGhzProtocol subghz_protocol_kia_v3_v4;

// Decoder functions
uint8_t subghz_protocol_decoder_kia_v3_v4_get_hash_data(void* context);
void subghz_protocol_encoder_kia_v3_v4_free(void* context);
void* subghz_protocol_decoder_kia_v3_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_kia_v3_v4_free(void* context);
void subghz_protocol_decoder_kia_v3_v4_reset(void* context);
void subghz_protocol_decoder_kia_v3_v4_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_kia_v3_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_kia_v3_v4_get_string(void* context, FuriString* output);

// Encoder functions
void* subghz_protocol_encoder_kia_v3_v4_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_kia_v3_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_kia_v3_v4_stop(void* context);
LevelDuration subghz_protocol_encoder_kia_v3_v4_yield(void* context);

// Encoder helper functions for UI
void subghz_protocol_encoder_kia_v3_v4_set_button(void* context, uint8_t button);
void subghz_protocol_encoder_kia_v3_v4_set_counter(void* context, uint16_t counter);
void subghz_protocol_encoder_kia_v3_v4_increment_counter(void* context);
uint16_t subghz_protocol_encoder_kia_v3_v4_get_counter(void* context);
uint8_t subghz_protocol_encoder_kia_v3_v4_get_button(void* context);
