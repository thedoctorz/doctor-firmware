#pragma once

#include "base.h"
#include <flipper_format/flipper_format.h>

#define SUBGHZ_PROTOCOL_RENAULT_V1_NAME "Renault V1"
#define SUBGHZ_PROTOCOL_HITAG2_NAME     SUBGHZ_PROTOCOL_RENAULT_V1_NAME

#define HITAG2_BF_STATUS_IDLE      0
#define HITAG2_BF_STATUS_RUNNING   1
#define HITAG2_BF_STATUS_FOUND     2
#define HITAG2_BF_STATUS_NOT_FOUND 3
#define HITAG2_BF_STATUS_CANCELLED 4

typedef struct Hitag2BfState Hitag2BfState;
struct Hitag2BfState {
    volatile uint8_t cancel;
    volatile uint32_t progress_current;
    volatile uint8_t status;
    volatile uint32_t progress_total;
    uint8_t frame[11];
    uint8_t iv[4];
    void (*on_done)(void* context);
    void* on_done_ctx;
};

void hitag2_brute_force_run(Hitag2BfState* state);
int32_t hitag2_brute_force_thread_entry(void* arg);
bool hitag2_bf_state_from_flipper_format(Hitag2BfState* state, FlipperFormat* ff);
bool hitag2_bf_needs_bruteforce(FlipperFormat* ff, bool require_hitag2_protocol);
bool hitag2_bf_patch_flipper_format_on_success(FlipperFormat* ff, const Hitag2BfState* state);
bool hitag2_bf_patch_flipper_format_on_miss(FlipperFormat* ff);

bool renault_v1_flipper_needs_hitag2_key(FlipperFormat* flipper_format);
bool renault_v1_flipper_parse_hitag2_key_text(const char* text, uint8_t key[6]);
bool renault_v1_flipper_write_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]);
bool renault_v1_flipper_accept_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]);

typedef struct SubGhzProtocolDecoderHitag2 SubGhzProtocolDecoderHitag2;
typedef struct SubGhzProtocolEncoderHitag2 SubGhzProtocolEncoderHitag2;

extern const SubGhzProtocolDecoder subghz_protocol_hitag2_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_hitag2_encoder;
extern const SubGhzProtocol subghz_protocol_renault_v1;

void* subghz_protocol_encoder_hitag2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_hitag2_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_hitag2_stop(void* context);
LevelDuration subghz_protocol_encoder_hitag2_yield(void* context);

void* subghz_protocol_decoder_hitag2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_hitag2_free(void* context);
void subghz_protocol_decoder_hitag2_reset(void* context);
void subghz_protocol_decoder_hitag2_feed(void* context, bool level, uint32_t duration);
SubGhzProtocolStatus subghz_protocol_decoder_hitag2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_hitag2_get_string(void* context, FuriString* output);
