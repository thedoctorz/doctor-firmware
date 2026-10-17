#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_SUBARU_NAME "Subaru"

typedef struct SubGhzProtocolDecoderSubaru SubGhzProtocolDecoderSubaru;
typedef struct SubGhzProtocolEncoderSubaru SubGhzProtocolEncoderSubaru;

extern const SubGhzProtocolDecoder subghz_protocol_subaru_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_subaru_encoder;
extern const SubGhzProtocol subghz_protocol_subaru;

void* subghz_protocol_encoder_subaru_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_subaru_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_subaru_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_subaru_stop(void* context);
LevelDuration subghz_protocol_encoder_subaru_yield(void* context);

void* subghz_protocol_decoder_subaru_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_subaru_free(void* context);
void subghz_protocol_decoder_subaru_reset(void* context);
void subghz_protocol_decoder_subaru_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_subaru_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_subaru_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_subaru_get_string(void* context, FuriString* output);
