#include "hitag2_i.h"
#include <string.h>
#include <stdlib.h>
#include "hitag2_key_db.h"
#include <lib/subghz/registry.h>

static const uint8_t fiat_v1_known_keys[FIAT_V1_KNOWN_KEY_COUNT][6] = {
    {0xB7U, 0x92U, 0x80U, 0xAEU, 0xCCU, 0x37U},
    {0xD4U, 0x24U, 0x28U, 0xF7U, 0xD9U, 0x66U},
    {0x4DU, 0x34U, 0x3FU, 0xD4U, 0xE7U, 0xB6U},
    {0x6DU, 0x6BU, 0xF2U, 0x1DU, 0x3AU, 0x1AU},
    {0xA3U, 0xF3U, 0xACU, 0xF7U, 0xB9U, 0x10U},
    {0x4DU, 0x49U, 0x4BU, 0x52U, 0x4FU, 0x4EU},
    {0xCDU, 0x49U, 0x4BU, 0x52U, 0x4FU, 0x4EU},
    {0x33U, 0xFAU, 0x2FU, 0xCDU, 0xC3U, 0x3BU},
    {0xF6U, 0x1AU, 0xEFU, 0x9CU, 0xD0U, 0x1BU},
};

static bool hitag2_flipper_is_protocol(FlipperFormat* ff, const char* name) {
    if(!ff) return false;
    FuriString* protocol = furi_string_alloc();
    flipper_format_rewind(ff);
    bool matches = flipper_format_read_string(ff, "Protocol", protocol) &&
                   furi_string_equal_str(protocol, name);
    furi_string_free(protocol);
    return matches;
}

bool hitag2_flipper_read_u32(FlipperFormat* ff, const char* name, uint32_t* value) {
    flipper_format_rewind(ff);
    return flipper_format_read_uint32(ff, name, value, 1);
}

static bool hitag2_parse_key_text(const char* text, uint8_t key[6]) {
    if(!text || !key || strlen(text) != 12U) return false;
    for(size_t i = 0; i < 6U; i++) {
        char byte_str[3] = {text[i * 2], text[i * 2 + 1], '\0'};
        char* end = NULL;
        unsigned long val = strtoul(byte_str, &end, 16);
        if(end != byte_str + 2 || val > 0xFFUL) return false;
        key[i] = (uint8_t)val;
    }
    return true;
}

static bool hitag2_flipper_write_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) {
        return false;
    }
    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, "Hitag2 Key", key, 6U)) {
        return false;
    }
    uint32_t epoch = 0U;
    if(!hitag2_flipper_read_u32(flipper_format, "Hitag2 Epoch", &epoch)) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_uint32(flipper_format, "Hitag2 Epoch", &epoch, 1U);
    }
    return true;
}

void hitag2_flipper_u32(FlipperFormat* ff, const char* key, uint32_t value) {
    flipper_format_rewind(ff);
    if(!flipper_format_update_uint32(ff, key, &value, 1)) {
        flipper_format_rewind(ff);
        flipper_format_insert_or_update_uint32(ff, key, &value, 1);
    }
}

SubGhzProtocolStatus fiat_v1_verify_protocol_name(FlipperFormat* ff, const char* expected_name) {
    if(!ff || !expected_name) return SubGhzProtocolStatusError;
    FuriString* tmp = furi_string_alloc();
    if(!tmp) return SubGhzProtocolStatusError;
    SubGhzProtocolStatus result = SubGhzProtocolStatusError;
    flipper_format_rewind(ff);
    if(!flipper_format_read_string(ff, "Protocol", tmp))
        result = SubGhzProtocolStatusErrorParserOthers;
    else if(subghz_protocol_names_match(furi_string_get_cstr(tmp), expected_name))
        result = SubGhzProtocolStatusOk;
    furi_string_free(tmp);
    return result;
}

void fiat_v1_encoder_read_fields(
    FlipperFormat* ff,
    uint32_t* serial_out,
    uint32_t* btn_out,
    uint32_t* cnt_out,
    uint32_t* type_out) {
    if(!ff) return;
    uint32_t tmp = 0;
    if(serial_out) {
        if(hitag2_flipper_read_u32(ff, "Serial", &tmp)) *serial_out = tmp;
    }
    if(btn_out) {
        if(hitag2_flipper_read_u32(ff, "Btn", &tmp)) *btn_out = tmp;
    }
    if(cnt_out) {
        if(hitag2_flipper_read_u32(ff, "Cnt", &tmp)) *cnt_out = tmp;
    }
    if(type_out) {
        if(hitag2_flipper_read_u32(ff, "Type", &tmp)) *type_out = tmp;
    }
}

uint32_t fiat_v1_encoder_read_repeat(FlipperFormat* ff, uint32_t default_repeat) {
    if(!ff) return default_repeat;
    uint32_t tmp = 0;
    return hitag2_flipper_read_u32(ff, "Repeat", &tmp) ? tmp : default_repeat;
}

SubGhzProtocolStatus
    fiat_v1_write_display(FlipperFormat* ff, const char* protocol_name, const char* suffix) {
    if(!ff || !protocol_name || !suffix) return SubGhzProtocolStatusError;
    FuriString* display = furi_string_alloc();
    if(!display) return SubGhzProtocolStatusError;
    furi_string_printf(display, "%s - %s", protocol_name, suffix);
    flipper_format_rewind(ff);
    SubGhzProtocolStatus status =
        flipper_format_insert_or_update_string_cstr(ff, "Disp", furi_string_get_cstr(display)) ?
            SubGhzProtocolStatusOk :
            SubGhzProtocolStatusErrorParserOthers;
    furi_string_free(display);
    return status;
}

SubGhzProtocolStatus fiat_v1_encoder_read_bit(
    FlipperFormat* ff,
    const uint16_t* allowed_bits,
    size_t allowed_bits_count,
    uint32_t* out_bit) {
    if(!ff || !out_bit) return SubGhzProtocolStatusError;
    uint32_t bit = 0;
    if(!hitag2_flipper_read_u32(ff, "Bit", &bit)) return SubGhzProtocolStatusErrorValueBitCount;
    if(allowed_bits && allowed_bits_count) {
        bool ok = false;
        for(size_t i = 0; i < allowed_bits_count; i++)
            if((uint32_t)allowed_bits[i] == bit) {
                ok = true;
                break;
            }
        if(!ok) return SubGhzProtocolStatusError;
    }
    *out_bit = bit;
    return SubGhzProtocolStatusOk;
}

static bool fiat_v1_key_matches(
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t hop,
    const uint8_t key[6],
    uint32_t epoch) {
    return fiat_v1_bcm_generate_authenticator(uid, button, control, key, epoch) == hop;
}

static bool fiat_v1_parse_frame_from_flipper(
    FlipperFormat* ff,
    uint32_t* uid,
    uint8_t* button,
    uint16_t* control,
    uint32_t* hop) {
    if(!ff || !uid || !button || !control || !hop) return false;

    uint8_t raw[FIAT_V1_WIRE_BYTES] = {0};
    flipper_format_rewind(ff);
    if(flipper_format_read_hex(ff, FIAT_V1_RAW_FIELD, raw, sizeof(raw)) &&
       fiat_v1_frame_valid(raw)) {
        *uid = fiat_v1_uid(raw);
        *button = raw[6] >> 4U;
        *control = (uint16_t)fiat_v1_counter(raw);
        *hop = fiat_v1_hop(raw);
        return true;
    }

    uint32_t serial = 0U;
    uint32_t btn = 0U;
    uint32_t cnt = 0U;
    uint32_t hop_val = 0U;
    if(hitag2_flipper_read_u32(ff, "Serial", &serial)) {
        if(hitag2_flipper_read_u32(ff, "Btn", &btn)) {
            if(hitag2_flipper_read_u32(ff, "Cnt", &cnt)) {
                if(hitag2_flipper_read_u32(ff, FIAT_V1_HOP_FIELD, &hop_val)) {
                    *uid = serial;
                    *button = (uint8_t)btn;
                    *control = (uint16_t)(cnt & 0x03FFU);
                    *hop = hop_val;
                    return serial != 0U && serial != UINT32_MAX &&
                           fiat_v1_button_valid((uint8_t)btn);
                }
            }
        }
    }

    SubGhzBlockGeneric generic = {0};
    flipper_format_rewind(ff);
    if(subghz_block_generic_deserialize_check_count_bit(&generic, ff, FIAT_V1_LOGICAL_BITS) !=
       SubGhzProtocolStatusOk) {
        return false;
    }
    *uid = (uint32_t)(generic.data >> 32U);
    *hop = (uint32_t)generic.data;
    *button = generic.btn;
    *control = (uint16_t)(generic.cnt & 0x03FFU);
    return *uid != 0U && *uid != UINT32_MAX && fiat_v1_button_valid(*button);
}

static bool fiat_v1_key_matches_any_button(
    uint32_t uid,
    uint16_t control,
    uint32_t hop,
    const uint8_t key[6],
    uint32_t epoch) {
    static const uint8_t buttons[] = {0x1U, 0x2U, 0x4U, 0x8U};
    for(size_t i = 0; i < COUNT_OF(buttons); i++) {
        if(fiat_v1_key_matches(uid, buttons[i], control, hop, key, epoch)) {
            return true;
        }
    }
    return false;
}

static bool fiat_v1_key_is_builtin(const uint8_t key[6]) {
    for(uint8_t i = 0U; i < FIAT_V1_KNOWN_KEY_COUNT; i++) {
        if(memcmp(key, fiat_v1_known_keys[i], 6U) == 0) return true;
    }
    return false;
}

static void fiat_v1_hitag2_key_disk_to_cipher(const uint8_t disk[6], uint8_t cipher[6]) {
    const uint8_t tmp[6] = {disk[2], disk[3], disk[4], disk[5], disk[0], disk[1]};
    memcpy(cipher, tmp, sizeof(tmp));
}

void fiat_v1_hitag2_key_cipher_to_disk(const uint8_t cipher[6], uint8_t disk[6]) {
    const uint8_t tmp[6] = {cipher[4], cipher[5], cipher[0], cipher[1], cipher[2], cipher[3]};
    memcpy(disk, tmp, sizeof(tmp));
}

static void fiat_v1_store_hitag2_key(FlipperFormat* ff, const uint8_t key[6], uint32_t epoch) {
    if(!ff || !key) return;
    uint8_t disk_key[6];
    fiat_v1_hitag2_key_cipher_to_disk(key, disk_key);
    flipper_format_rewind(ff);
    flipper_format_insert_or_update_hex(ff, FIAT_V1_HITAG2_KEY_FIELD, disk_key, 6U);
    hitag2_flipper_u32(ff, FIAT_V1_HITAG2_EPOCH_FIELD, epoch & 0x3FFFFUL);
}

bool fiat_v1_resolve_hitag2_key(
    FlipperFormat* ff,
    uint32_t uid,
    uint8_t button,
    uint16_t control,
    uint32_t hop,
    uint8_t key_out[6],
    uint32_t* epoch_out) {
    if(!key_out) return false;

    uint8_t key[6] = {0};
    uint32_t epoch = 0U;
    if(ff) {
        flipper_format_rewind(ff);
        uint8_t disk_key[6] = {0};
        if(flipper_format_read_hex(ff, FIAT_V1_HITAG2_KEY_FIELD, disk_key, sizeof(disk_key))) {
            fiat_v1_hitag2_key_disk_to_cipher(disk_key, key);
            if(hitag2_flipper_read_u32(ff, FIAT_V1_HITAG2_EPOCH_FIELD, &epoch)) {
                epoch &= 0x3FFFFUL;
            }
            if(fiat_v1_key_matches(uid, button, control, hop, key, epoch) ||
               fiat_v1_key_matches_any_button(uid, control, hop, key, epoch)) {
                memcpy(key_out, key, sizeof(key));
                if(epoch_out) *epoch_out = epoch;
                if(!fiat_v1_key_is_builtin(key)) {
                    Hitag2KeyDbRecord existing = {0};
                    if(!hitag2_key_db_find(uid, &existing)) {
                        hitag2_key_db_save_key(uid, key, epoch);
                    }
                }
                return true;
            }
        }
    }

    if(ff && uid != 0U && uid != UINT32_MAX) {
        Hitag2KeyDbRecord record = {0};
        if(hitag2_key_db_find(uid, &record) && record.has_key) {
            const uint32_t db_epoch = record.epoch & 0x3FFFFUL;
            if(fiat_v1_key_matches(uid, button, control, hop, record.key, db_epoch) ||
               fiat_v1_key_matches_any_button(uid, control, hop, record.key, db_epoch)) {
                memcpy(key_out, record.key, sizeof(record.key));
                if(epoch_out) *epoch_out = db_epoch;
                fiat_v1_store_hitag2_key(ff, record.key, db_epoch);
                return true;
            }
        }
    }

    for(uint8_t i = 0U; i < FIAT_V1_KNOWN_KEY_COUNT; i++) {
        if(fiat_v1_key_matches(uid, button, control, hop, fiat_v1_known_keys[i], 0U) ||
           fiat_v1_key_matches_any_button(uid, control, hop, fiat_v1_known_keys[i], 0U)) {
            memcpy(key_out, fiat_v1_known_keys[i], sizeof(key));
            if(epoch_out) *epoch_out = 0U;
            return true;
        }
    }
    return false;
}

bool fiat_v1_flipper_needs_hitag2_key(FlipperFormat* flipper_format) {
    if(!flipper_format) return false;
    if(!hitag2_flipper_is_protocol(flipper_format, SUBGHZ_PROTOCOL_FIAT_V1_NAME)) return false;

    uint32_t uid = 0U;
    uint8_t button = 0U;
    uint16_t control = 0U;
    uint32_t hop = 0U;
    uint8_t key[6] = {0};
    uint32_t epoch = 0U;
    if(!fiat_v1_parse_frame_from_flipper(flipper_format, &uid, &button, &control, &hop)) {
        return true;
    }
    return !fiat_v1_resolve_hitag2_key(flipper_format, uid, button, control, hop, key, &epoch);
}

bool fiat_v1_flipper_parse_hitag2_key_text(const char* text, uint8_t key[6]) {
    return hitag2_parse_key_text(text, key);
}

bool fiat_v1_flipper_accept_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) return false;

    if(!hitag2_flipper_is_protocol(flipper_format, SUBGHZ_PROTOCOL_FIAT_V1_NAME)) return false;

    uint32_t uid = 0U;
    uint8_t button = 0U;
    uint16_t control = 0U;
    uint32_t hop = 0U;
    if(!fiat_v1_parse_frame_from_flipper(flipper_format, &uid, &button, &control, &hop)) {
        return false;
    }
    uint8_t cipher_key[6];
    fiat_v1_hitag2_key_disk_to_cipher(key, cipher_key);
    if(!fiat_v1_key_matches(uid, button, control, hop, cipher_key, 0U) &&
       !fiat_v1_key_matches_any_button(uid, control, hop, cipher_key, 0U)) {
        return false;
    }

    fiat_v1_store_hitag2_key(flipper_format, cipher_key, 0U);
    hitag2_key_db_save_key(uid, cipher_key, 0U);
    return true;
}

bool fiat_v1_flipper_write_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) return false;
    uint8_t disk_key[6];
    fiat_v1_hitag2_key_cipher_to_disk(key, disk_key);
    return hitag2_flipper_write_key(flipper_format, disk_key);
}

static bool hitag2_hell_key_nonzero(const uint8_t key[6]) {
    for(size_t i = 0; i < 6U; i++) {
        if(key[i]) return true;
    }
    return false;
}

bool hitag2_read_hell_key(FlipperFormat* ff, uint8_t key[6], uint32_t* epoch_out) {
    memset(key, 0, 6U);
    if(!hitag2_read_hex_be(ff, HITAG2_HELL_KEY_FIELD, key, 6U)) {
        return false;
    }
    uint32_t epoch = 0U;
    if(hitag2_flipper_read_u32(ff, HITAG2_HELL_EPOCH_FIELD, &epoch)) {
        epoch &= 0x3FFFFUL;
    }
    if(epoch_out) {
        *epoch_out = epoch;
    }
    return hitag2_hell_key_nonzero(key);
}

bool hitag2_hell_find_epoch(
    const uint8_t key[6],
    uint32_t uid,
    uint8_t btn,
    uint16_t cnt10,
    uint32_t hop,
    uint32_t* epoch_out) {
    uint32_t start = epoch_out ? (*epoch_out & 0x3FFFFUL) : 0U;
    for(uint32_t i = 0U; i < 8U; i++) {
        const uint32_t epoch = (start + i) & 0x3FFFFUL;
        const uint32_t full = (epoch << 10U) | (cnt10 & 0x3FFU);
        if(hitag2_hell_authenticator(uid, btn, full, key) == hop) {
            if(epoch_out) {
                *epoch_out = epoch;
            }
            return true;
        }
    }
    return false;
}

bool hitag2_read_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint8_t* data,
    size_t nbytes) {
    if(!flipper_format_rewind(flipper_format)) {
        return false;
    }
    return flipper_format_read_hex(flipper_format, name, data, nbytes);
}

bool hitag2_write_hex_be(
    FlipperFormat* flipper_format,
    const char* name,
    uint64_t value,
    size_t nbytes) {
    uint8_t data[8] = {0};
    furi_check(nbytes <= sizeof(data));
    hitag2_u64_to_bytes_be(value, data, nbytes);
    return flipper_format_insert_or_update_hex(flipper_format, name, data, nbytes);
}

void hitag2_write_named_fields(
    FlipperFormat* ff,
    uint32_t serial,
    uint8_t btn,
    uint32_t cnt,
    uint32_t hop) {
    hitag2_flipper_u32(ff, "Serial", serial);
    hitag2_flipper_u32(ff, "Btn", btn);
    hitag2_flipper_u32(ff, "Cnt", cnt);
    hitag2_flipper_u32(ff, HITAG2_HOP_FIELD, hop);
}

void hitag2_read_recovered_and_seed(
    FlipperFormat* flipper_format,
    uint8_t* recovered,
    uint32_t* seed) {
    *recovered = 0;
    *seed = 0;

    uint8_t recovered_hex = 0;
    if(hitag2_read_hex_be(flipper_format, "Recovered", &recovered_hex, 1)) {
        *recovered = recovered_hex;
    } else {
        uint32_t recovered_u32 = 0;
        if(flipper_format_rewind(flipper_format) &&
           flipper_format_read_uint32(flipper_format, "Recovered", &recovered_u32, 1)) {
            *recovered = (uint8_t)recovered_u32;
        }
    }

    uint8_t seed_be[4] = {0};
    if(hitag2_read_hex_be(flipper_format, "Seed", seed_be, 4)) {
        *seed = (uint32_t)hitag2_bytes_to_u64_be(seed_be, 4);
        return;
    }

    if(flipper_format_rewind(flipper_format)) {
        flipper_format_read_uint32(flipper_format, "Seed", seed, 1);
    }
}

SubGhzProtocolStatus hitag2_write_extra_fields(
    FlipperFormat* flipper_format,
    uint64_t key_2,
    uint8_t recovered,
    uint32_t seed) {
    if(!hitag2_write_hex_be(flipper_format, "Key_2", key_2, 8)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    uint8_t recovered_hex = 0;
    if(recovered == HITAG2_RECOVERED_YES) {
        recovered_hex = HITAG2_RECOVERED_YES;
    } else if(recovered == HITAG2_RECOVERED_BF_MISS) {
        recovered_hex = HITAG2_RECOVERED_BF_MISS;
    }
    if(!flipper_format_insert_or_update_hex(flipper_format, "Recovered", &recovered_hex, 1)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    if(!hitag2_write_hex_be(flipper_format, "Seed", seed, 4)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return SubGhzProtocolStatusOk;
}

bool hitag2_bf_state_from_flipper_format(Hitag2BfState* state, FlipperFormat* ff) {
    furi_check(state);
    furi_check(ff);

    uint8_t key1[8] = {0};
    uint8_t key2[8] = {0};
    if(!hitag2_read_hex_be(ff, "Key", key1, sizeof(key1))) {
        return false;
    }
    if(!hitag2_read_hex_be(ff, "Key_2", key2, sizeof(key2))) {
        return false;
    }

    for(size_t i = 0; i < 8; i++) {
        state->frame[i] = key1[i];
    }
    state->frame[8] = key2[5];
    state->frame[9] = key2[6];
    state->frame[10] = key2[7];

    state->cancel = 0;
    state->progress_current = 0;
    state->progress_total = HITAG2_BF_CANDIDATES;
    state->status = HITAG2_BF_STATUS_IDLE;
    state->on_done = NULL;
    state->on_done_ctx = NULL;
    state->iv[0] = 0;
    state->iv[1] = 0;
    state->iv[2] = 0;
    state->iv[3] = 0;
    return true;
}

bool hitag2_bf_needs_bruteforce(FlipperFormat* ff, bool require_hitag2_protocol) {
    if(!ff) {
        return false;
    }

    FuriString* s = furi_string_alloc();
    flipper_format_rewind(ff);
    if(require_hitag2_protocol) {
        if(!flipper_format_read_string(ff, "Protocol", s) ||
           !furi_string_equal_str(s, SUBGHZ_PROTOCOL_RENAULT_V1_NAME)) {
            furi_string_free(s);
            return false;
        }
        flipper_format_rewind(ff);
    }

    bool has_key = flipper_format_read_string(ff, "Key", s);
    furi_string_free(s);
    if(!has_key) {
        return false;
    }

    uint8_t recovered = 0;
    uint32_t seed = 0;
    hitag2_read_recovered_and_seed(ff, &recovered, &seed);
    UNUSED(seed);
    if(recovered != 0) {
        return false;
    }
    uint8_t hell_key[6];
    uint32_t epoch = 0;
    if(hitag2_read_hell_key(ff, hell_key, &epoch)) {
        return false;
    }
    return true;
}

bool hitag2_bf_patch_flipper_format_on_success(FlipperFormat* ff, const Hitag2BfState* state) {
    if(!ff || !state) {
        return false;
    }

    flipper_format_rewind(ff);
    const uint8_t recovered_hex = HITAG2_RECOVERED_YES;
    if(!flipper_format_insert_or_update_hex(ff, "Recovered", &recovered_hex, 1)) {
        return false;
    }
    const uint32_t seed = hitag2_seed_from_iv(state->iv);
    if(!hitag2_write_hex_be(ff, "Seed", seed, 4)) {
        return false;
    }

    uint32_t serial = 0U;
    if(!hitag2_flipper_read_u32(ff, "Serial", &serial) || serial == 0U || serial == UINT32_MAX) {
        uint8_t key_be[8] = {0};
        if(hitag2_read_hex_be(ff, "Key", key_be, sizeof(key_be))) {
            serial = (uint32_t)(hitag2_bytes_to_u64_be(key_be, sizeof(key_be)) >> 32U);
        }
    }
    if(serial != 0U && serial != UINT32_MAX) {
        hitag2_key_db_save_seed(serial, seed);
    }
    return true;
}

bool hitag2_bf_patch_flipper_format_on_miss(FlipperFormat* ff) {
    if(!ff) {
        return false;
    }

    flipper_format_rewind(ff);
    const uint8_t recovered_hex = HITAG2_RECOVERED_BF_MISS;
    return flipper_format_insert_or_update_hex(ff, "Recovered", &recovered_hex, 1);
}

static bool renault_v1_frame_from_flipper(
    FlipperFormat* ff,
    uint32_t* uid,
    uint8_t* btn,
    uint16_t* cnt,
    uint32_t* hop) {
    uint8_t key[8] = {0};
    uint8_t key2[8] = {0};
    if(!hitag2_read_hex_be(ff, "Key", key, sizeof(key))) return false;
    if(!hitag2_read_hex_be(ff, "Key_2", key2, sizeof(key2))) return false;
    const uint64_t data = hitag2_bytes_to_u64_be(key, sizeof(key));
    const uint64_t data_2 = hitag2_bytes_to_u64_be(key2, sizeof(key2));
    hitag2_unpack_frame(data, data_2, uid, btn, cnt, hop, NULL);
    return *uid != 0U && *uid != UINT32_MAX;
}

static void renault_v1_remember_key(uint32_t uid, const uint8_t key[6], uint32_t epoch) {
    Hitag2KeyDbRecord existing = {0};
    if(!hitag2_key_db_find(uid, &existing)) {
        hitag2_key_db_save_key(uid, key, epoch);
    }
}

static void renault_v1_remember_seed(uint32_t uid, uint32_t seed) {
    Hitag2KeyDbRecord existing = {0};
    if(!hitag2_key_db_find(uid, &existing)) {
        hitag2_key_db_save_seed(uid, seed);
    }
}

bool renault_v1_apply_stored_secret(FlipperFormat* ff) {
    uint32_t uid = 0U;
    uint8_t btn = 0U;
    uint16_t cnt = 0U;
    uint32_t hop = 0U;
    if(!ff || !renault_v1_frame_from_flipper(ff, &uid, &btn, &cnt, &hop)) return false;

    uint8_t key[6] = {0};
    uint32_t epoch = 0U;
    if(hitag2_read_hell_key(ff, key, &epoch) &&
       hitag2_hell_find_epoch(key, uid, btn, cnt, hop, &epoch)) {
        renault_v1_remember_key(uid, key, epoch);
        hitag2_flipper_u32(ff, HITAG2_HELL_EPOCH_FIELD, epoch & 0x3FFFFUL);
        return true;
    }

    uint8_t recovered = 0U;
    uint32_t seed = 0U;
    hitag2_read_recovered_and_seed(ff, &recovered, &seed);
    if(recovered == HITAG2_RECOVERED_YES) {
        renault_v1_remember_seed(uid, seed);
        return true;
    }

    Hitag2KeyDbRecord record = {0};
    if(!hitag2_key_db_find(uid, &record)) return false;

    if(record.has_key) {
        uint32_t db_epoch = record.epoch & 0x3FFFFUL;
        if(hitag2_hell_find_epoch(record.key, uid, btn, cnt, hop, &db_epoch)) {
            flipper_format_rewind(ff);
            if(flipper_format_insert_or_update_hex(ff, HITAG2_HELL_KEY_FIELD, record.key, 6U)) {
                hitag2_flipper_u32(ff, HITAG2_HELL_EPOCH_FIELD, db_epoch);
                return true;
            }
        }
    }
    if(record.has_seed) {
        const uint8_t recovered_hex = HITAG2_RECOVERED_YES;
        flipper_format_rewind(ff);
        if(!flipper_format_insert_or_update_hex(ff, "Recovered", &recovered_hex, 1)) return false;
        return hitag2_write_hex_be(ff, "Seed", record.seed, 4);
    }
    return false;
}

bool renault_v1_flipper_needs_hitag2_key(FlipperFormat* flipper_format) {
    if(!flipper_format) {
        return false;
    }

    if(!hitag2_flipper_is_protocol(flipper_format, SUBGHZ_PROTOCOL_RENAULT_V1_NAME)) return false;

    return !renault_v1_apply_stored_secret(flipper_format);
}

bool renault_v1_flipper_parse_hitag2_key_text(const char* text, uint8_t key[6]) {
    return hitag2_parse_key_text(text, key);
}

bool renault_v1_flipper_write_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    return hitag2_flipper_write_key(flipper_format, key);
}

bool renault_v1_flipper_accept_hitag2_key(FlipperFormat* flipper_format, const uint8_t key[6]) {
    if(!flipper_format || !key) return false;

    if(!hitag2_flipper_is_protocol(flipper_format, SUBGHZ_PROTOCOL_RENAULT_V1_NAME)) return false;

    uint32_t uid = 0U;
    uint8_t btn = 0U;
    uint16_t cnt = 0U;
    uint32_t hop = 0U;
    if(!renault_v1_frame_from_flipper(flipper_format, &uid, &btn, &cnt, &hop)) return false;

    uint32_t epoch = 0U;
    if(hitag2_flipper_read_u32(flipper_format, HITAG2_HELL_EPOCH_FIELD, &epoch)) {
        epoch &= 0x3FFFFUL;
    }
    if(!hitag2_hell_find_epoch(key, uid, btn, cnt, hop, &epoch)) {
        epoch = 0U;
        if(!hitag2_hell_find_epoch(key, uid, btn, cnt, hop, &epoch)) return false;
    }

    flipper_format_rewind(flipper_format);
    if(!flipper_format_insert_or_update_hex(flipper_format, HITAG2_HELL_KEY_FIELD, key, 6U)) {
        return false;
    }
    hitag2_flipper_u32(flipper_format, HITAG2_HELL_EPOCH_FIELD, epoch & 0x3FFFFUL);
    hitag2_key_db_save_key(uid, key, epoch);
    return true;
}
