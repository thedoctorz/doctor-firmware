#pragma once

#include "hitag2.h"

uint32_t fiat_v1_bcm_generate_authenticator(
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    const uint8_t key[6],
    uint32_t epoch);

uint32_t
    hitag2_hell_authenticator(uint32_t uid, uint8_t button, uint32_t counter, const uint8_t key[6]);

void hitag2_encrypt_frame(
    uint32_t serial,
    uint32_t cnt,
    uint8_t btn,
    uint32_t seed,
    uint8_t out[11],
    uint8_t iv[4]);

uint32_t hitag2_seed_from_iv(const uint8_t iv[4]);
