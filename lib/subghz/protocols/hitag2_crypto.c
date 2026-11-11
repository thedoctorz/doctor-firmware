#include "hitag2_i.h"
#include <string.h>
#include <stdlib.h>

static uint8_t hitag2_truth(uint32_t table, uint8_t index) {
    return (uint8_t)((table >> index) & 1U);
}

static uint8_t hitag2_filter_index(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint8_t)((a << 3U) | (b << 2U) | (c << 1U) | d);
}

static uint8_t hitag2_byte_bit(uint8_t byte, uint8_t bit) {
    return (uint8_t)((byte >> bit) & 1U);
}

static uint8_t hitag2_filter(const uint8_t state[6]) {
    uint8_t group = 0U;
    group |= hitag2_truth(
        0x2c79U,
        hitag2_filter_index(
            hitag2_byte_bit(state[0], 1U),
            hitag2_byte_bit(state[0], 2U),
            hitag2_byte_bit(state[0], 4U),
            hitag2_byte_bit(state[0], 5U)));
    group |= (uint8_t)(hitag2_truth(
                           0x6671U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[1], 0U),
                               hitag2_byte_bit(state[1], 1U),
                               hitag2_byte_bit(state[1], 3U),
                               hitag2_byte_bit(state[1], 7U)))
                       << 1U);
    group |= (uint8_t)(hitag2_truth(
                           0x6671U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[3], 5U),
                               hitag2_byte_bit(state[2], 0U),
                               hitag2_byte_bit(state[2], 2U),
                               hitag2_byte_bit(state[2], 6U)))
                       << 2U);
    group |= (uint8_t)(hitag2_truth(
                           0x6671U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[4], 6U),
                               hitag2_byte_bit(state[3], 0U),
                               hitag2_byte_bit(state[3], 2U),
                               hitag2_byte_bit(state[3], 3U)))
                       << 3U);
    group |= (uint8_t)(hitag2_truth(
                           0x2c79U,
                           hitag2_filter_index(
                               hitag2_byte_bit(state[5], 1U),
                               hitag2_byte_bit(state[5], 3U),
                               hitag2_byte_bit(state[5], 4U),
                               hitag2_byte_bit(state[4], 5U)))
                       << 4U);
    return hitag2_truth(0x7907287bUL, group);
}

static uint8_t hitag2_parity8(uint8_t value) {
    value ^= (uint8_t)(value >> 4U);
    value ^= (uint8_t)(value >> 2U);
    value ^= (uint8_t)(value >> 1U);
    return value & 1U;
}

static uint8_t hitag2_feedback(const uint8_t state[6]) {
    static const uint8_t masks[6] = {0xb3U, 0x80U, 0x83U, 0x22U, 0x00U, 0x73U};
    uint8_t feedback = 0U;
    for(uint8_t i = 0U; i < 6U; i++) {
        feedback ^= hitag2_parity8((uint8_t)(state[i] & masks[i]));
    }
    return feedback & 1U;
}

static void hitag2_shift_state(uint8_t state[6], uint8_t input) {
    for(uint8_t i = 0U; i < 5U; i++) {
        state[i] = (uint8_t)((state[i] << 1U) | (state[i + 1U] >> 7U));
    }
    state[5] = (uint8_t)((state[5] << 1U) | (input & 1U));
}

static uint8_t fiat_v1_input_bit_u32_be(uint32_t value, uint8_t index) {
    return (uint8_t)((value >> (31U - index)) & 1U);
}

static uint8_t fiat_v1_input_bit_bytes_be(const uint8_t* bytes, uint8_t index) {
    return (uint8_t)((bytes[index >> 3U] >> (7U - (index & 7U))) & 1U);
}

uint32_t fiat_v1_bcm_generate_authenticator(
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    const uint8_t key[6],
    uint32_t epoch) {
    uint8_t state[6] = {
        (uint8_t)(uid >> 24U),
        (uint8_t)(uid >> 16U),
        (uint8_t)(uid >> 8U),
        (uint8_t)uid,
        key[4],
        key[5],
    };

    const uint32_t iv = ((epoch & 0x3FFFFUL) << 14U) | (((uint32_t)control & 0x03FFUL) << 4U) |
                        ((uint32_t)button & 0x0FUL);

    for(uint8_t i = 0U; i < 32U; i++) {
        const uint8_t input = fiat_v1_input_bit_u32_be(iv, i) ^
                              fiat_v1_input_bit_bytes_be(key, i) ^ hitag2_filter(state);
        hitag2_shift_state(state, input);
    }

    uint32_t authenticator = 0U;
    for(uint8_t i = 0U; i < 32U; i++) {
        authenticator = (authenticator << 1U) | hitag2_filter(state);
        hitag2_shift_state(state, hitag2_feedback(state));
    }
    return authenticator;
}

static uint8_t hitag2_hell_i4(uint64_t x, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint8_t)((((x >> a) & 1U) << 3U) | (((x >> b) & 1U) << 2U) | (((x >> c) & 1U) << 1U) |
                     ((x >> d) & 1U));
}

static uint8_t hitag2_hell_f20(uint64_t state) {
    const uint8_t s0 = (uint8_t)((0x3C65U >> hitag2_hell_i4(state, 2U, 3U, 5U, 6U)) & 1U);
    const uint8_t s1 = (uint8_t)((0x0EE5U >> hitag2_hell_i4(state, 8U, 12U, 14U, 15U)) & 1U);
    const uint8_t s2 = (uint8_t)((0x0EE5U >> hitag2_hell_i4(state, 17U, 21U, 23U, 26U)) & 1U);
    const uint8_t s3 = (uint8_t)((0x0EE5U >> hitag2_hell_i4(state, 28U, 29U, 31U, 33U)) & 1U);
    const uint8_t s4 = (uint8_t)((0x3C65U >> hitag2_hell_i4(state, 34U, 43U, 44U, 46U)) & 1U);
    return (uint8_t)((0x0DD3929BUL >> ((s0 << 4U) | (s1 << 3U) | (s2 << 2U) | (s3 << 1U) | s4)) &
                     1U);
}

static uint64_t hitag2_hell_lfsr(uint64_t state) {
    const uint64_t fb =
        (state ^ (state >> 2U) ^ (state >> 3U) ^ (state >> 6U) ^ (state >> 7U) ^ (state >> 8U) ^
         (state >> 16U) ^ (state >> 22U) ^ (state >> 23U) ^ (state >> 26U) ^ (state >> 30U) ^
         (state >> 41U) ^ (state >> 42U) ^ (state >> 43U) ^ (state >> 46U) ^ (state >> 47U)) &
        1ULL;
    return (state >> 1U) | (fb << 47U);
}

static uint64_t hitag2_hell_key_u64(const uint8_t key[6]) {
    uint64_t key64 = 0ULL;
    for(size_t i = 0; i < 6U; i++) {
        key64 = (key64 << 8U) | key[i];
    }
    return key64;
}

uint32_t hitag2_hell_authenticator(
    uint32_t uid,
    uint8_t button,
    uint32_t counter,
    const uint8_t key[6]) {
    const uint64_t key64 = hitag2_hell_key_u64(key);
    const uint32_t nonce = (counter << 4U) | ((uint32_t)button & 0x0FU);
    uint64_t state = 0ULL;
    for(uint8_t i = 32U; i < 48U; i++) {
        state = (state << 1U) | ((key64 >> i) & 1ULL);
    }
    for(uint8_t i = 0U; i < 32U; i++) {
        state = (state << 1U) | ((uint64_t)((uid >> i) & 1U));
    }
    for(uint8_t i = 0U; i < 32U; i++) {
        const uint64_t nonce_bit = (uint64_t)hitag2_hell_f20(state) ^ ((nonce >> (31U - i)) & 1U);
        state = (state >> 1U) | (((nonce_bit ^ ((key64 >> (31U - i)) & 1ULL)) & 1ULL) << 47U);
    }
    uint32_t hop = 0U;
    for(uint8_t i = 0U; i < 32U; i++) {
        hop = (hop << 1U) | hitag2_hell_f20(state);
        state = hitag2_hell_lfsr(state);
    }
    return hop;
}

static uint8_t hitag2_ubfx(uint32_t value, uint8_t lsb, uint8_t width) {
    return (uint8_t)((value >> lsb) & ((1U << width) - 1U));
}

static void hitag2_serial_permute(const uint8_t serial[4], uint8_t perm[6]) {
    const uint8_t sn0 = serial[0];
    const uint8_t sn1 = serial[1];
    const uint8_t sn2 = serial[2];
    const uint8_t sn3 = serial[3];

    uint8_t acc =
        (uint8_t)(((sn0 >> 6) & 2U) | ((sn1 >> 4) & 8U) | hitag2_ubfx(sn0 ^ 0x10U, 4, 1));
    acc |= (uint8_t)((~(uint32_t)(sn0 << 2)) & 0x20U);
    acc |= (uint8_t)((sn0 << 5) & 0x40U);
    acc |= (uint8_t)((sn2 << 1) & 0x80U);
    acc |= (uint8_t)((~(uint32_t)(sn2 >> 5)) & 4U);
    acc |= (uint8_t)((~(uint32_t)(sn0 >> 2)) & 0x10U);
    perm[0] = acc;

    const uint8_t sn1_inv_shr3 = (uint8_t) ~(sn1 >> 3);
    const uint8_t sn3_inv_shl3 = (uint8_t) ~(sn3 << 3);
    acc = (uint8_t)(hitag2_ubfx(sn1, 5, 1) | (sn1_inv_shr3 & 4U) | (sn0 & 0x20U));
    acc |= (uint8_t)(sn3_inv_shl3 & 8U);
    acc |= (uint8_t)((~(uint32_t)(sn2 << 2)) & 0x10U);
    acc |= (uint8_t)((~(uint32_t)(sn2 << 3)) & 0x40U);
    acc |= 0x80U;
    perm[1] = acc;

    const uint8_t sn0_shr3 = (uint8_t)(sn0 >> 3);
    acc = (uint8_t)(hitag2_ubfx(sn0, 2, 1) | (sn0_shr3 & 2U) | ((~(uint32_t)(sn3 >> 2)) & 4U) |
                    (sn1 & 0x10U));
    acc |= (uint8_t)((~(uint32_t)(sn1 << 4)) & 0x20U);
    acc |= (uint8_t)(sn3_inv_shl3 & 0x40U);
    acc |= 0x80U;
    perm[2] = acc;

    const uint8_t sn2_inv_shl6 = (uint8_t) ~(sn2 << 6);
    acc = (uint8_t)(((sn0 >> 2) & 2U) | ((sn1 >> 3) & 8U) | hitag2_ubfx(sn2 ^ 0x20U, 5, 1));
    acc |= (uint8_t)((sn1 << 5) & 0x20U);
    acc |= (uint8_t)((sn3 << 1) & 0x40U);
    acc |= (uint8_t)(((uint8_t)~sn0_shr3) & 4U);
    acc |= (uint8_t)(sn1_inv_shr3 & 0x10U);
    acc |= (uint8_t)(sn2_inv_shl6 & 0x80U);
    perm[3] = acc;

    uint8_t perm4_lo =
        (uint8_t)(((sn3 << 2) & 8U) | ((sn0 << 4) & 0x10U) | hitag2_ubfx(sn0 ^ 4U, 2, 1));
    perm4_lo |= (uint8_t)((~(uint32_t)(sn3 >> 1)) & 2U);
    perm4_lo |= (uint8_t)((~(uint32_t)(sn1 >> 4)) & 4U);
    uint8_t perm4_hi = (uint8_t)((~(uint32_t)(sn0 << 4)) & 0x20U);
    perm4_hi |= (uint8_t)(sn2_inv_shl6 & 0x40U);
    const uint8_t sn1_inv_shl3 = (uint8_t) ~(sn1 << 3);
    perm4_hi |= (uint8_t)(sn1_inv_shl3 & 0x80U);
    perm[4] = (uint8_t)(perm4_lo | perm4_hi);

    uint8_t perm5 =
        (uint8_t)(((sn3 >> 2) & 0x10U) | ((sn2 >> 3) & 2U) | (((uint8_t)~sn0) & 0x80U));
    perm5 |= (uint8_t)((~(uint32_t)(sn0 << 3)) & 8U);
    perm5 |= (uint8_t)((sn0 >> 2) & 0x10U);
    perm5 |= (uint8_t)(sn1_inv_shl3 & 0x20U);
    perm5 |= (uint8_t)((sn3 >> 1) & 0x40U);
    perm5 |= (uint8_t)((~(uint32_t)(sn1 >> 1)) & 4U);
    perm[5] = perm5;
}

static void hitag2_shift_u32(uint8_t buf[4], uint8_t inject) {
    const uint8_t b0 = buf[0];
    const uint8_t b1 = buf[1];
    const uint8_t b2 = buf[2];
    const uint8_t b3 = buf[3];
    buf[3] = (uint8_t)((b2 >> 7U) | (b3 << 1U));
    buf[2] = (uint8_t)((b1 >> 7U) | (b2 << 1U));
    buf[1] = (uint8_t)((b0 >> 7U) | (b1 << 1U));
    buf[0] = (uint8_t)((b0 << 1U) | (inject & 1U));
}

static void hitag2_clock_cipher(uint8_t state[6], uint8_t iv_work[4], uint8_t iv_orig[4]) {
    for(uint8_t i = 0; i < 32; i++) {
        const uint8_t filter = hitag2_filter(state);
        uint8_t mix = (iv_work[3] & 0x80U) ? (filter ? 0U : 1U) : (filter ? 1U : 0U);
        if(iv_orig[3] & 0x80U) {
            mix ^= 5U;
        }
        hitag2_shift_state(state, (uint8_t)(mix & 1U));
        hitag2_shift_u32(iv_work, 0);
        hitag2_shift_u32(iv_orig, (uint8_t)((mix >> 2U) & 1U));
    }

    for(uint8_t i = 0; i < 32; i++) {
        hitag2_shift_u32(iv_work, 0);
        if(hitag2_filter(state)) {
            iv_work[0] |= 1U;
        }
        hitag2_shift_state(state, hitag2_feedback(state));
    }
}

static void hitag2_build_iv(uint32_t cnt, uint8_t btn, uint32_t seed, uint8_t iv[4]) {
    iv[0] = (uint8_t)(((cnt << 4U) & 0xF0U) | (btn & 0x0FU));
    iv[1] = (uint8_t)((cnt >> 4U) & 0xFFU);
    iv[2] = (uint8_t)(((seed >> 8U) & 0xF0U) | ((cnt >> 12U) & 0x0FU));
    iv[3] = (uint8_t)(seed & 0xFFU);
}

static void
    hitag2_encrypt_from_iv(const uint8_t serial_be[4], const uint8_t iv[4], uint8_t out[11]) {
    uint8_t perm[6];
    uint8_t state[6];
    uint8_t iv_work[4];
    uint8_t iv_orig[4];

    hitag2_serial_permute(serial_be, perm);
    state[0] = serial_be[0];
    state[1] = serial_be[1];
    state[2] = serial_be[2];
    state[3] = serial_be[3];
    state[4] = perm[4];
    state[5] = perm[5];
    iv_work[0] = perm[0];
    iv_work[1] = perm[1];
    iv_work[2] = perm[2];
    iv_work[3] = perm[3];
    iv_orig[0] = iv[0];
    iv_orig[1] = iv[1];
    iv_orig[2] = iv[2];
    iv_orig[3] = iv[3];
    hitag2_clock_cipher(state, iv_work, iv_orig);

    const uint8_t hop0 = iv_work[0];
    uint8_t hop1 = (uint8_t)((hop0 >> 7U) | (iv_work[1] << 1U));
    uint8_t hop2 = (uint8_t)((iv_work[1] >> 7U) | (iv_work[2] << 1U));
    const uint8_t hop_ext = (uint8_t)(((hop0 >> 6U) & 1U) | (hop1 << 1U));
    uint8_t hop3 = (uint8_t)((iv_work[2] >> 7U) | (iv_work[3] << 1U));
    uint8_t hop4 = (uint8_t)((iv_work[3] >> 7U) | (state[5] << 1U));
    hop1 = (uint8_t)((hop1 >> 7U) | (hop2 << 1U));
    hop2 = (uint8_t)((hop2 >> 7U) | (hop3 << 1U));
    hop3 = (uint8_t)((hop3 >> 7U) | (hop4 << 1U));

    uint32_t mix = ((uint32_t)iv[1] << 4U) | ((uint32_t)iv[0] >> 4U);
    mix = (mix | (((uint32_t)iv[2] << 12U) & 0xFFFFU)) & 0xFFFFU;

    out[0] = serial_be[0];
    out[1] = serial_be[1];
    out[2] = serial_be[2];
    out[3] = serial_be[3];
    out[4] = (uint8_t)(((mix >> 6U) & 0x0FU) | ((iv[0] << 4U) & 0xF0U));
    out[5] = (uint8_t)((hop3 & 3U) | ((mix << 2U) & 0xFCU));
    out[6] = hop2;
    out[7] = hop1;
    out[8] = hop_ext;
    out[9] = (uint8_t)((hop0 << 2U) | 2U);
    out[10] = hitag2_frame_xor(out);
}

void hitag2_encrypt_frame(
    uint32_t serial,
    uint32_t cnt,
    uint8_t btn,
    uint32_t seed,
    uint8_t out[11],
    uint8_t iv[4]) {
    const uint8_t serial_be[4] = {
        (uint8_t)(serial >> 24U),
        (uint8_t)(serial >> 16U),
        (uint8_t)(serial >> 8U),
        (uint8_t)serial,
    };
    hitag2_build_iv(cnt, btn, seed, iv);
    hitag2_encrypt_from_iv(serial_be, iv, out);
}

uint32_t hitag2_seed_from_iv(const uint8_t iv[4]) {
    return ((uint32_t)iv[0] << 24U) | ((uint32_t)iv[1] << 16U) | ((uint32_t)iv[2] << 8U) | iv[3];
}

static void hitag2_bf_rearrange_dest(uint8_t dest[11]) {
    const uint8_t frame4 = dest[6];
    uint8_t frame5 = dest[5];
    uint8_t frame6 = dest[4];
    uint8_t frame7 = dest[3];
    const uint8_t frame8 = dest[2];
    const uint8_t frame9 = dest[1];

    uint8_t hop0 = (uint8_t)(((frame4 & 1U) << 7U) | (frame5 >> 1U));
    frame5 = (uint8_t)(((frame5 & 1U) << 7U) | (frame6 >> 1U));
    const uint8_t hop0_hi = (uint8_t)(hop0 >> 1U);
    hop0 = (uint8_t)(hop0 & 1U);
    hop0 = (uint8_t)((hop0 << 7U) | (frame5 >> 1U));
    frame6 = (uint8_t)(((frame6 & 1U) << 7U) | (frame7 >> 1U));
    frame5 = (uint8_t)(((frame5 & 1U) << 7U) | (frame6 >> 1U));
    dest[3] = frame5;
    frame7 = (uint8_t)(((frame7 & 1U) << 7U) | (frame8 >> 1U));
    const uint8_t hop2 = (uint8_t)(((frame6 & 1U) << 7U) | (frame7 >> 1U));
    frame6 = (uint8_t)(((frame8 & 1U) << 7U) | (frame9 >> 1U));
    dest[6] = (uint8_t)(((frame9 & 1U) << 7U) | (frame4 >> 2U));
    dest[1] = (uint8_t)(((frame7 & 1U) << 7U) | (frame6 >> 1U));
    dest[5] = (uint8_t)(hop0_hi | (((frame4 >> 1U) & 1U) << 7U));
    dest[2] = hop2;
    dest[4] = hop0;
}

static void hitag2_bf_prepare(
    const uint8_t frame[11],
    uint8_t serial_be[4],
    uint8_t perm[6],
    uint8_t hop_target[4],
    uint8_t* iv0,
    uint8_t* fp) {
    uint8_t dest[11];
    for(size_t i = 0; i < 11; i++) {
        dest[i] = frame[10U - i];
    }
    hitag2_bf_rearrange_dest(dest);

    hop_target[0] = dest[1];
    hop_target[1] = dest[2];
    hop_target[2] = dest[3];
    hop_target[3] = dest[4];
    *iv0 = (uint8_t)((frame[4] >> 4U) | (dest[5] << 4U));
    *fp = (uint8_t)((dest[5] >> 4U) | ((dest[6] & 3U) << 4U));

    serial_be[0] = frame[0];
    serial_be[1] = frame[1];
    serial_be[2] = frame[2];
    serial_be[3] = frame[3];
    hitag2_serial_permute(serial_be, perm);
}

static bool hitag2_bf_hop_matches(
    const uint8_t serial_be[4],
    const uint8_t perm[6],
    const uint8_t iv[4],
    const uint8_t hop_target[4]) {
    uint8_t state[6];
    uint8_t iv_work[4];
    uint8_t iv_orig[4];

    state[0] = serial_be[0];
    state[1] = serial_be[1];
    state[2] = serial_be[2];
    state[3] = serial_be[3];
    state[4] = perm[4];
    state[5] = perm[5];
    iv_work[0] = perm[0];
    iv_work[1] = perm[1];
    iv_work[2] = perm[2];
    iv_work[3] = perm[3];
    iv_orig[0] = iv[0];
    iv_orig[1] = iv[1];
    iv_orig[2] = iv[2];
    iv_orig[3] = iv[3];
    hitag2_clock_cipher(state, iv_work, iv_orig);

    return (iv_work[0] == hop_target[0]) && (iv_work[1] == hop_target[1]) &&
           (iv_work[2] == hop_target[2]) && (iv_work[3] == hop_target[3]);
}

void hitag2_brute_force_run(Hitag2BfState* state) {
    furi_check(state);

    uint8_t serial_be[4];
    uint8_t perm[6];
    uint8_t hop_target[4];
    uint8_t iv0 = 0;
    uint8_t fp = 0;
    hitag2_bf_prepare(state->frame, serial_be, perm, hop_target, &iv0, &fp);

    state->progress_current = 0;
    state->progress_total = HITAG2_BF_CANDIDATES;
    state->status = HITAG2_BF_STATUS_RUNNING;

    uint8_t iv[4];
    for(uint32_t cand = 0; cand < HITAG2_BF_CANDIDATES; cand++) {
        if((cand & (HITAG2_BF_PROGRESS_INTERVAL - 1U)) == 0U) {
            state->progress_current = cand;
            if(state->cancel) {
                state->status = (uint8_t)(state->cancel + 3U);
                return;
            }
        }

        iv[0] = iv0;
        iv[1] = (uint8_t)(fp | ((cand & 3U) << 6U));
        iv[2] = (uint8_t)(cand >> 2U);
        iv[3] = (uint8_t)(cand >> 10U);
        if(hitag2_bf_hop_matches(serial_be, perm, iv, hop_target)) {
            state->iv[0] = iv[0];
            state->iv[1] = iv[1];
            state->iv[2] = iv[2];
            state->iv[3] = iv[3];
            state->progress_current = HITAG2_BF_CANDIDATES;
            state->status = HITAG2_BF_STATUS_FOUND;
            return;
        }
    }

    state->progress_current = HITAG2_BF_CANDIDATES;
    state->status = (uint8_t)(state->cancel + 3U);
}

int32_t hitag2_brute_force_thread_entry(void* arg) {
    Hitag2BfState* state = arg;
    hitag2_brute_force_run(state);
    if(state->on_done) {
        state->on_done(state->on_done_ctx);
    }
    return 0;
}
