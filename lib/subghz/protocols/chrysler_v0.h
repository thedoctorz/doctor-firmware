#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_CHRYSLER_V0_NAME "Chrysler V0"

typedef struct SubGhzProtocolDecoderChrysler_V0 SubGhzProtocolDecoderChrysler_V0;
typedef struct SubGhzProtocolEncoderChrysler_V0 SubGhzProtocolEncoderChrysler_V0;

extern const SubGhzProtocolDecoder subghz_protocol_chrysler_v0_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_chrysler_v0_encoder;
extern const SubGhzProtocol subghz_protocol_chrysler_v0;

void* subghz_protocol_encoder_chrysler_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_chrysler_v0_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_chrysler_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_chrysler_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_chrysler_v0_yield(void* context);

void* subghz_protocol_decoder_chrysler_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_chrysler_v0_free(void* context);
void subghz_protocol_decoder_chrysler_v0_reset(void* context);
void subghz_protocol_decoder_chrysler_v0_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_chrysler_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_chrysler_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_chrysler_v0_get_string(void* context, FuriString* output);
