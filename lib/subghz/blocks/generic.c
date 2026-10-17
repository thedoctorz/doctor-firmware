#include "generic.h"
#include "../subghz_setting.h"
#include <lib/toolbox/stream/stream.h>
#include <lib/flipper_format/flipper_format_i.h>

#define TAG "SubGhzBlockGeneric"

SubGhzBlockGenericGlobal subghz_block_generic_global;

bool subghz_block_generic_read_key_u64(FlipperFormat* flipper_format, uint64_t* out_key) {
    furi_check(out_key);
    uint8_t key_data[sizeof(uint64_t)] = {0};
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
        *out_key = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            *out_key = (*out_key << 8) | key_data[i];
        }
        return true;
    }

    FuriString* value = furi_string_alloc();
    bool ok = false;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_string(flipper_format, "Key", value)) {
        const char* str = furi_string_get_cstr(value);
        uint64_t key = 0;
        uint8_t hex_pos = 0;
        for(size_t i = 0; str[i] != '\0' && hex_pos < 16; i++) {
            if(str[i] == ' ') continue;
            uint8_t nibble = 0;
            if(str[i] >= '0' && str[i] <= '9') {
                nibble = (uint8_t)(str[i] - '0');
            } else if(str[i] >= 'A' && str[i] <= 'F') {
                nibble = (uint8_t)(str[i] - 'A' + 10);
            } else if(str[i] >= 'a' && str[i] <= 'f') {
                nibble = (uint8_t)(str[i] - 'a' + 10);
            } else {
                break;
            }
            key = (key << 4) | nibble;
            hex_pos++;
        }
        if(hex_pos == 16) {
            *out_key = key;
            ok = true;
        }
    }
    furi_string_free(value);
    return ok;
}

static bool subghz_block_generic_write_key_u64(FlipperFormat* flipper_format, uint64_t key) {
    uint8_t key_data[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        key_data[sizeof(uint64_t) - i - 1] = (key >> (i * 8)) & 0xFF;
    }
    return flipper_format_write_hex(flipper_format, "Key", key_data, sizeof(uint64_t));
}

static bool subghz_block_generic_write_key2_u64(FlipperFormat* flipper_format, uint64_t key) {
    if(key == 0) return true;
    uint8_t key_data[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        key_data[sizeof(uint64_t) - i - 1] = (key >> (i * 8)) & 0xFF;
    }
    if(flipper_format_write_hex(flipper_format, "Key_2", key_data, sizeof(uint64_t))) {
        return true;
    }
    return flipper_format_write_hex(flipper_format, "Key2", key_data, sizeof(uint64_t));
}

static bool subghz_block_generic_read_key2_u64(FlipperFormat* flipper_format, uint64_t* out_key) {
    furi_check(out_key);
    uint8_t key_data[sizeof(uint64_t)] = {0};
    flipper_format_rewind(flipper_format);
    bool have_key2 = flipper_format_read_hex(flipper_format, "Key_2", key_data, sizeof(uint64_t));
    if(!have_key2) {
        flipper_format_rewind(flipper_format);
        have_key2 = flipper_format_read_hex(flipper_format, "Key2", key_data, sizeof(uint64_t));
    }
    if(have_key2) {
        *out_key = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            *out_key = (*out_key << 8) | key_data[i];
        }
        return true;
    }
    return false;
}

static bool subghz_block_generic_read_key_u64_named(
    FlipperFormat* flipper_format,
    const char* key_name,
    uint64_t* out_key) {
    furi_check(out_key);
    uint8_t key_data[sizeof(uint64_t)] = {0};
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_hex(flipper_format, key_name, key_data, sizeof(uint64_t))) {
        *out_key = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            *out_key = (*out_key << 8) | key_data[i];
        }
        return true;
    }
    return false;
}

static bool subghz_block_generic_write_key_u64_named(
    FlipperFormat* flipper_format,
    const char* key_name,
    uint64_t key) {
    if(key == 0) return true;
    uint8_t key_data[sizeof(uint64_t)] = {0};
    for(size_t i = 0; i < sizeof(uint64_t); i++) {
        key_data[sizeof(uint64_t) - i - 1] = (key >> (i * 8)) & 0xFF;
    }
    return flipper_format_write_hex(flipper_format, key_name, key_data, sizeof(uint64_t));
}

static void subghz_block_generic_read_optional_fields(
    SubGhzBlockGeneric* instance,
    FlipperFormat* flipper_format) {
    uint32_t temp = 0;

    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Serial", &instance->serial, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Btn", &temp, 1)) {
        instance->btn = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Cnt", &instance->cnt, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Type", &temp, 1)) {
        instance->type = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Seed", &instance->seed, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "CRC", &temp, 1)) {
        instance->crc = (uint16_t)temp;
    } else {
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Checksum", &temp, 1)) {
            instance->crc = (uint16_t)temp;
        }
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "KeyIdx", &temp, 1)) {
        instance->key_idx = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "ValidationField", &temp, 1)) {
        instance->validation_field = (uint16_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Encrypted", &instance->encrypted, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Decrypted", &instance->decrypted, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "KIAVersion", &temp, 1)) {
        instance->kia_version = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "DataHi", &instance->data_hi, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "DataLo", &instance->data_lo, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "YekHi", &instance->yek_hi, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "YekLo", &instance->yek_lo, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "RawCnt", &instance->raw_cnt, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Fx", &temp, 1)) {
        instance->fx = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "TE", &temp, 1)) {
        instance->te = (uint16_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Extra", &instance->extra, 1)) {
        // read ok
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "Extra_bits", &temp, 1)) {
        instance->extra_bits = (uint8_t)temp;
    }
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "EndByte", &temp, 1)) {
        instance->end_byte = (uint8_t)temp;
    }

    subghz_block_generic_read_key2_u64(flipper_format, &instance->data_2);
    subghz_block_generic_read_key_u64_named(flipper_format, "Key_3", &instance->data_3);
    subghz_block_generic_read_key_u64_named(flipper_format, "Key_4", &instance->data_4);

    flipper_format_rewind(flipper_format);
    if(instance->data_count_bit == 72 && (strcmp(instance->protocol_name, "Nice FloR-S") == 0)) {
        if(flipper_format_read_uint32(flipper_format, "Data", &temp, 1)) {
            instance->data_2 = ((uint64_t)temp << 4);
        }
    }
}

static bool subghz_block_generic_write_optional_fields(
    SubGhzBlockGeneric* instance,
    FlipperFormat* flipper_format) {
    uint32_t temp;

    if(instance->serial || instance->btn || instance->cnt) {
        if(!flipper_format_write_uint32(flipper_format, "Serial", &instance->serial, 1)) {
            return false;
        }
        temp = instance->btn;
        if(!flipper_format_write_uint32(flipper_format, "Btn", &temp, 1)) {
            return false;
        }
        if(!flipper_format_write_uint32(flipper_format, "Cnt", &instance->cnt, 1)) {
            return false;
        }
    }
    if(instance->type) {
        temp = instance->type;
        if(!flipper_format_write_uint32(flipper_format, "Type", &temp, 1)) {
            return false;
        }
    }
    if(instance->seed) {
        if(!flipper_format_write_uint32(flipper_format, "Seed", &instance->seed, 1)) {
            return false;
        }
    }
    if(instance->crc) {
        temp = instance->crc;
        if(!flipper_format_write_uint32(flipper_format, "CRC", &temp, 1)) {
            return false;
        }
    }
    if(instance->key_idx) {
        temp = instance->key_idx;
        if(!flipper_format_write_uint32(flipper_format, "KeyIdx", &temp, 1)) {
            return false;
        }
    }
    if(instance->validation_field) {
        temp = instance->validation_field;
        if(!flipper_format_write_uint32(flipper_format, "ValidationField", &temp, 1)) {
            return false;
        }
    }
    if(instance->encrypted) {
        if(!flipper_format_write_uint32(flipper_format, "Encrypted", &instance->encrypted, 1)) {
            return false;
        }
    }
    if(instance->decrypted) {
        if(!flipper_format_write_uint32(flipper_format, "Decrypted", &instance->decrypted, 1)) {
            return false;
        }
    }
    if(instance->kia_version) {
        temp = instance->kia_version;
        if(!flipper_format_write_uint32(flipper_format, "KIAVersion", &temp, 1)) {
            return false;
        }
    }
    if(instance->data_hi) {
        if(!flipper_format_write_uint32(flipper_format, "DataHi", &instance->data_hi, 1)) {
            return false;
        }
    }
    if(instance->data_lo) {
        if(!flipper_format_write_uint32(flipper_format, "DataLo", &instance->data_lo, 1)) {
            return false;
        }
    }
    if(instance->yek_hi) {
        if(!flipper_format_write_uint32(flipper_format, "YekHi", &instance->yek_hi, 1)) {
            return false;
        }
    }
    if(instance->yek_lo) {
        if(!flipper_format_write_uint32(flipper_format, "YekLo", &instance->yek_lo, 1)) {
            return false;
        }
    }
    if(instance->raw_cnt) {
        if(!flipper_format_write_uint32(flipper_format, "RawCnt", &instance->raw_cnt, 1)) {
            return false;
        }
    }
    if(instance->fx) {
        temp = instance->fx;
        if(!flipper_format_write_uint32(flipper_format, "Fx", &temp, 1)) {
            return false;
        }
    }
    if(instance->te) {
        temp = instance->te;
        if(!flipper_format_write_uint32(flipper_format, "TE", &temp, 1)) {
            return false;
        }
    }
    if(instance->extra) {
        if(!flipper_format_write_uint32(flipper_format, "Extra", &instance->extra, 1)) {
            return false;
        }
    }
    if(instance->extra_bits) {
        temp = instance->extra_bits;
        if(!flipper_format_write_uint32(flipper_format, "Extra_bits", &temp, 1)) {
            return false;
        }
    }
    if(instance->end_byte) {
        temp = instance->end_byte;
        if(!flipper_format_write_uint32(flipper_format, "EndByte", &temp, 1)) {
            return false;
        }
    }

    if(!subghz_block_generic_write_key2_u64(flipper_format, instance->data_2)) {
        return false;
    }
    if(!subghz_block_generic_write_key_u64_named(flipper_format, "Key_3", instance->data_3)) {
        return false;
    }
    if(!subghz_block_generic_write_key_u64_named(flipper_format, "Key_4", instance->data_4)) {
        return false;
    }

    if(instance->data_count_bit == 72 && (strcmp(instance->protocol_name, "Nice FloR-S") == 0)) {
        temp = (instance->data_2 >> 4) & 0xFFFFF;
        if(!flipper_format_write_uint32(flipper_format, "Data", &temp, 1)) {
            return false;
        }
    }

    return true;
}

void subghz_block_generic_global_counter_override_set(uint32_t counter) {
    subghz_block_generic_global.new_cnt = counter;
    subghz_block_generic_global.cnt_need_override = true;
}

bool subghz_block_generic_global_counter_override_get(uint32_t* counter) {
    if(subghz_block_generic_global.cnt_need_override) {
        const uint8_t cnt_length_bit = subghz_block_generic_global.cnt_length_bit;
        const uint32_t mask = (cnt_length_bit == 0 || cnt_length_bit >= 32) ?
                                  0xFFFFFFFF :
                                  (0xFFFFFFFF >> (32 - cnt_length_bit));
        *counter = subghz_block_generic_global.new_cnt & mask;
        subghz_block_generic_global.cnt_need_override = false;
        return true;
    } else {
        return false;
    }
}

void subghz_block_generic_global_button_override_set(uint8_t button) {
    subghz_block_generic_global.new_btn = button;
    subghz_block_generic_global.btn_need_override = true;
}

bool subghz_block_generic_global_button_override_get(uint8_t* button) {
    if(subghz_block_generic_global.btn_need_override) {
        const uint8_t btn_length_bit = subghz_block_generic_global.btn_length_bit;
        const uint8_t mask =
            (btn_length_bit == 0 || btn_length_bit >= 8) ? 0xFF : (0xFF >> (8 - btn_length_bit));
        *button = subghz_block_generic_global.new_btn & mask;
        subghz_block_generic_global.btn_need_override = false;
        return true;
    } else {
        return false;
    }
}

void subghz_block_generic_global_reset(void* p) {
    UNUSED(p);
    bool tmp = subghz_block_generic_global.endless_tx;
    bool replay = subghz_block_generic_global.hitag2_replay_only;
    memset(&subghz_block_generic_global, 0, sizeof(subghz_block_generic_global));
    subghz_block_generic_global.endless_tx = tmp;
    subghz_block_generic_global.hitag2_replay_only = replay;
}

void subghz_block_generic_get_preset_name(const char* preset_name, FuriString* preset_str) {
    const char* preset_name_temp;
    preset_name = subghz_setting_radio_counterpart(preset_name, false);
    if(!strcmp(preset_name, "AM270") || !strcmp(preset_name, "FuriHalSubGhzPresetOok270Async")) {
        preset_name_temp = "FuriHalSubGhzPresetOok270Async";
    } else if(
        !strcmp(preset_name, "AM650") || !strcmp(preset_name, "FuriHalSubGhzPresetOok650Async")) {
        preset_name_temp = "FuriHalSubGhzPresetOok650Async";
    } else if(
        !strcmp(preset_name, "FM238") ||
        !strcmp(preset_name, "FuriHalSubGhzPreset2FSKDev238Async")) {
        preset_name_temp = "FuriHalSubGhzPreset2FSKDev238Async";
    } else if(
        !strcmp(preset_name, "FM12K") ||
        !strcmp(preset_name, "FuriHalSubGhzPreset2FSKDev12KAsync")) {
        preset_name_temp = "FuriHalSubGhzPreset2FSKDev12KAsync";
    } else if(!strcmp(preset_name, "FM20K")) {
        preset_name_temp = "FM20K";
    } else if(!strcmp(preset_name, "FM14K")) {
        preset_name_temp = "FM14K";
    } else if(!strcmp(preset_name, "AM101")) {
        preset_name_temp = "AM101";
    } else if(
        !strcmp(preset_name, "FM476") ||
        !strcmp(preset_name, "FuriHalSubGhzPreset2FSKDev476Async")) {
        preset_name_temp = "FuriHalSubGhzPreset2FSKDev476Async";
    } else if(!strcmp(preset_name, "CUSTOM") || !strcmp(preset_name, "FuriHalSubGhzPresetCustom")) {
        preset_name_temp = "FuriHalSubGhzPresetCustom";
    } else {
        preset_name_temp = "FuriHalSubGhzPresetCustom";
    }
    furi_string_set(preset_str, preset_name_temp);
}

bool subghz_block_generic_serialize_preset(
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(flipper_format);
    furi_check(preset);

    stream_clean(flipper_format_get_raw_stream(flipper_format));
    if(!flipper_format_write_header_cstr(
           flipper_format, SUBGHZ_KEY_FILE_TYPE, SUBGHZ_KEY_FILE_VERSION)) {
        FURI_LOG_E(TAG, "Unable to add header");
        return false;
    }

    if(!flipper_format_write_uint32(flipper_format, "Frequency", &preset->frequency, 1)) {
        FURI_LOG_E(TAG, "Unable to add Frequency");
        return false;
    }

    FuriString* preset_name = furi_string_alloc();
    subghz_block_generic_get_preset_name(furi_string_get_cstr(preset->name), preset_name);
    bool success = flipper_format_write_string_cstr(
        flipper_format, "Preset", furi_string_get_cstr(preset_name));
    if(success && !strcmp(furi_string_get_cstr(preset_name), "FuriHalSubGhzPresetCustom")) {
        success =
            flipper_format_write_string_cstr(flipper_format, "Custom_preset_module", "CC1101") &&
            flipper_format_write_hex(
                flipper_format, "Custom_preset_data", preset->data, preset->data_size);
    }
    furi_string_free(preset_name);
    if(!success) {
        FURI_LOG_E(TAG, "Unable to add Preset");
    }
    return success;
}

SubGhzProtocolStatus subghz_block_generic_serialize(
    SubGhzBlockGeneric* instance,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(instance);
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    FuriString* temp_str;
    temp_str = furi_string_alloc();
    do {
        stream_clean(flipper_format_get_raw_stream(flipper_format));
        if(!flipper_format_write_header_cstr(
               flipper_format, SUBGHZ_KEY_FILE_TYPE, SUBGHZ_KEY_FILE_VERSION)) {
            FURI_LOG_E(TAG, "Unable to add header");
            res = SubGhzProtocolStatusErrorParserHeader;
            break;
        }

        if(!flipper_format_write_uint32(flipper_format, "Frequency", &preset->frequency, 1)) {
            FURI_LOG_E(TAG, "Unable to add Frequency");
            res = SubGhzProtocolStatusErrorParserFrequency;
            break;
        }

        subghz_block_generic_get_preset_name(furi_string_get_cstr(preset->name), temp_str);
        if(!flipper_format_write_string_cstr(
               flipper_format, "Preset", furi_string_get_cstr(temp_str))) {
            FURI_LOG_E(TAG, "Unable to add Preset");
            res = SubGhzProtocolStatusErrorParserPreset;
            break;
        }
        if(!strcmp(furi_string_get_cstr(temp_str), "FuriHalSubGhzPresetCustom")) {
            if(!flipper_format_write_string_cstr(
                   flipper_format, "Custom_preset_module", "CC1101")) {
                FURI_LOG_E(TAG, "Unable to add Custom_preset_module");
                res = SubGhzProtocolStatusErrorParserCustomPreset;
                break;
            }
            if(!flipper_format_write_hex(
                   flipper_format, "Custom_preset_data", preset->data, preset->data_size)) {
                FURI_LOG_E(TAG, "Unable to add Custom_preset_data");
                res = SubGhzProtocolStatusErrorParserCustomPreset;
                break;
            }
        }
        if(!flipper_format_write_string_cstr(flipper_format, "Protocol", instance->protocol_name)) {
            FURI_LOG_E(TAG, "Unable to add Protocol");
            res = SubGhzProtocolStatusErrorParserProtocolName;
            break;
        }
        uint32_t temp = instance->data_count_bit;
        if(!flipper_format_write_uint32(flipper_format, "Bit", &temp, 1)) {
            FURI_LOG_E(TAG, "Unable to add Bit");
            res = SubGhzProtocolStatusErrorParserBitCount;
            break;
        }

        if(!subghz_block_generic_write_key_u64(flipper_format, instance->data)) {
            FURI_LOG_E(TAG, "Unable to add Key");
            res = SubGhzProtocolStatusErrorParserKey;
            break;
        }

        if(!subghz_block_generic_write_optional_fields(instance, flipper_format)) {
            FURI_LOG_E(TAG, "Unable to add optional fields");
            res = SubGhzProtocolStatusErrorParserOthers;
            break;
        }

        res = SubGhzProtocolStatusOk;
    } while(false);
    furi_string_free(temp_str);
    return res;
}

SubGhzProtocolStatus
    subghz_block_generic_deserialize(SubGhzBlockGeneric* instance, FlipperFormat* flipper_format) {
    furi_check(instance);

    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    uint32_t temp_data = 0;

    do {
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            res = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        if(!flipper_format_read_uint32(flipper_format, "Bit", (uint32_t*)&temp_data, 1)) {
            FURI_LOG_E(TAG, "Missing Bit");
            res = SubGhzProtocolStatusErrorParserBitCount;
            break;
        }
        instance->data_count_bit = (uint16_t)temp_data;

        if(!subghz_block_generic_read_key_u64(flipper_format, &instance->data)) {
            FURI_LOG_E(TAG, "Missing Key");
            res = SubGhzProtocolStatusErrorParserKey;
            break;
        }

        subghz_block_generic_read_optional_fields(instance, flipper_format);

        res = SubGhzProtocolStatusOk;
    } while(0);

    return res;
}

SubGhzProtocolStatus subghz_block_generic_deserialize_check_count_bit(
    SubGhzBlockGeneric* instance,
    FlipperFormat* flipper_format,
    uint16_t count_bit) {
    furi_check(instance);
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize(instance, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        if(instance->data_count_bit != count_bit) {
            FURI_LOG_D(TAG, "Wrong number of bits in key");
            ret = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }
    } while(false);
    return ret;
}
