#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_MERCEDES_NAME "MERCEDES"

typedef struct SubGhzProtocolDecoderMERCEDES SubGhzProtocolDecoderMERCEDES;
typedef struct SubGhzProtocolEncoderMERCEDES SubGhzProtocolEncoderMERCEDES;

extern const SubGhzProtocolDecoder subghz_protocol_mercedes_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_mercedes_encoder;
extern const SubGhzProtocol subghz_protocol_mercedes;

void* subghz_protocol_encoder_mercedes_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_mercedes_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_mercedes_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_mercedes_stop(void* context);
LevelDuration subghz_protocol_encoder_mercedes_yield(void* context);

void* subghz_protocol_decoder_mercedes_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mercedes_free(void* context);
void subghz_protocol_decoder_mercedes_reset(void* context);
void subghz_protocol_decoder_mercedes_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mercedes_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mercedes_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_mercedes_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_mercedes_get_string(void* context, FuriString* output);
