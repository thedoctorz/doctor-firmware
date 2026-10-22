#include "subghz_setting.h"
#include "types.h" // IWYU pragma: keep

#include <furi.h>
#include <m-list.h>
#include <lib/subghz/devices/cc1101_configs.h>
#include <cc1101_regs.h>

#define TAG "SubGhzSetting"

#define SUBGHZ_SETTING_FILE_TYPE    "Flipper SubGhz Setting File"
#define SUBGHZ_SETTING_FILE_VERSION 1

#define FREQUENCY_FLAG_DEFAULT (1 << 31)
#define FREQUENCY_MASK         (0xFFFFFFFF ^ FREQUENCY_FLAG_DEFAULT)

/* Default */
static const uint32_t subghz_frequency_list[] = {
    /* 300 - 348 */
    300000000,
    302757000,
    303000000,
    303875000,
    303900000,
    304250000,
    307000000,
    307500000,
    307800000,
    309000000,
    310000000,
    312000000,
    312100000,
    312200000,
    313000000,
    313850000,
    314000000,
    314350000,
    314980000,
    315000000,
    318000000,
    320000000,
    320150000,
    330000000,
    345000000,
    348000000,
    350000000,

    /* 387 - 464 */
    387000000,
    390000000,
    418000000,
    430000000,
    430500000,
    431000000,
    431500000,
    433075000, /* LPD433 first */
    433140000,
    433220000,
    433420000,
    433470000,
    433657070,
    433889000,
    433920000 | FREQUENCY_FLAG_DEFAULT, /* LPD433 mid */
    434075000,
    434176948,
    434190000,
    434370000,
    434390000,
    434420000,
    434620000,
    434775000, /* LPD433 last channels */
    438900000,
    440175000,
    462750000,
    464000000,
    467750000,

    /* 779 - 928 */
    779000000,
    868350000,
    868400000,
    868460000,
    868800000,
    868950000,
    906400000,
    915000000,
    925000000,
    928000000,
    0,
};

static const uint32_t subghz_hopper_frequency_list[] = {
    315000000,
    390000000,
    430500000,
    433140000,
    433470000,
    433920000,
    434370000,
    434420000,
    868350000,
    0,
};

typedef struct {
    FuriString* custom_preset_name;
    uint8_t* custom_preset_data;
    size_t custom_preset_data_size;
} SubGhzSettingCustomPresetItem;

ARRAY_DEF(SubGhzSettingCustomPresetItemArray, SubGhzSettingCustomPresetItem, M_POD_OPLIST) //-V658

#define M_OPL_SubGhzSettingCustomPresetItemArray_t() \
    ARRAY_OPLIST(SubGhzSettingCustomPresetItemArray, M_POD_OPLIST)

LIST_DEF(FrequencyList, uint32_t)

#define M_OPL_FrequencyList_t() LIST_OPLIST(FrequencyList)

typedef struct {
    SubGhzSettingCustomPresetItemArray_t data;
} SubGhzSettingCustomPresetStruct;

struct SubGhzSetting {
    FrequencyList_t frequencies;
    FrequencyList_t hopper_frequencies;
    FrequencyList_t user_frequencies;
    FrequencyList_t user_hopper_frequencies;
    bool add_standard_frequencies;
    SubGhzSettingCustomPresetStruct* preset;
    size_t builtin_preset_count;
};

SubGhzSetting* subghz_setting_alloc(void) {
    SubGhzSetting* instance = malloc(sizeof(SubGhzSetting));
    FrequencyList_init(instance->frequencies);
    FrequencyList_init(instance->hopper_frequencies);
    FrequencyList_init(instance->user_frequencies);
    FrequencyList_init(instance->user_hopper_frequencies);
    instance->add_standard_frequencies = true;
    instance->preset = malloc(sizeof(SubGhzSettingCustomPresetStruct));
    SubGhzSettingCustomPresetItemArray_init(instance->preset->data);
    instance->builtin_preset_count = 0;
    return instance;
}

static void subghz_setting_preset_reset(SubGhzSetting* instance) {
    for
        M_EACH(item, instance->preset->data, SubGhzSettingCustomPresetItemArray_t) {
            furi_string_free(item->custom_preset_name);
            free(item->custom_preset_data);
        }
    SubGhzSettingCustomPresetItemArray_reset(instance->preset->data);
}

void subghz_setting_free(SubGhzSetting* instance) {
    furi_check(instance);
    FrequencyList_clear(instance->frequencies);
    FrequencyList_clear(instance->hopper_frequencies);
    FrequencyList_clear(instance->user_frequencies);
    FrequencyList_clear(instance->user_hopper_frequencies);
    for
        M_EACH(item, instance->preset->data, SubGhzSettingCustomPresetItemArray_t) {
            furi_string_free(item->custom_preset_name);
            free(item->custom_preset_data);
        }
    SubGhzSettingCustomPresetItemArray_clear(instance->preset->data);
    free(instance->preset);
    free(instance);
}

static void subghz_setting_load_default_preset(
    SubGhzSetting* instance,
    const char* preset_name,
    const uint8_t* preset_data) {
    furi_assert(instance);
    furi_assert(preset_data);
    uint32_t preset_data_count = 0;
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_push_raw(instance->preset->data);

    item->custom_preset_name = furi_string_alloc();
    furi_string_set(item->custom_preset_name, preset_name);

    while(preset_data[preset_data_count]) {
        preset_data_count += 2;
    }
    preset_data_count += 2;
    item->custom_preset_data_size = sizeof(uint8_t) * preset_data_count + sizeof(uint8_t) * 8;
    item->custom_preset_data = malloc(item->custom_preset_data_size);
    //load preset register + pa table
    memcpy(&item->custom_preset_data[0], &preset_data[0], item->custom_preset_data_size);
}

typedef struct {
    const char* si_name;
    const char* cc_name;
    const uint8_t* regs;
    uint8_t profile;
} SubGhzSettingSi4463Preset;

static const SubGhzSettingSi4463Preset subghz_setting_si4463_presets[] = {
    {"AM800", "AM650", subghz_device_cc1101_preset_ook_650khz_async_regs, 0},
    {"AM660", "AM270", subghz_device_cc1101_preset_ook_270khz_async_regs, 1},
    {"FM476A", "FM476", subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs, 2},
    {"FM12KA", "FM12K", subghz_device_cc1101_preset_2fsk_dev12khz_async_regs, 3},
    {"FM20KA", "FM20K", subghz_device_cc1101_preset_mercedes_async_regs, 4},
    {"FM14KA", "FM14K", subghz_device_cc1101_preset_2fsk_dev14khz_38kbaud_async_regs, 5},
};

static void
    subghz_setting_load_si4463_preset(SubGhzSetting* instance, const SubGhzSettingSi4463Preset* p) {
    size_t regs = 0;
    while(p->regs[regs] || p->regs[regs + 1])
        regs += 2;
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_push_raw(instance->preset->data);
    item->custom_preset_name = furi_string_alloc_set(p->si_name);
    item->custom_preset_data_size = regs + 2 + 2 + 8;
    item->custom_preset_data = malloc(item->custom_preset_data_size);
    memcpy(item->custom_preset_data, p->regs, regs);
    item->custom_preset_data[regs] = 0xFC;
    item->custom_preset_data[regs + 1] = p->profile;
    item->custom_preset_data[regs + 2] = 0x00;
    item->custom_preset_data[regs + 3] = 0x00;
    memcpy(&item->custom_preset_data[regs + 4], &p->regs[regs + 2], 8);
}

const char* subghz_setting_radio_counterpart(const char* preset_name, bool si4463) {
    if(!preset_name) return preset_name;
    for(size_t i = 0; i < COUNT_OF(subghz_setting_si4463_presets); i++) {
        const SubGhzSettingSi4463Preset* p = &subghz_setting_si4463_presets[i];
        if(si4463 && !strcmp(preset_name, p->cc_name)) return p->si_name;
        if(!si4463 && !strcmp(preset_name, p->si_name)) return p->cc_name;
    }
    if(si4463 && !strcmp(preset_name, "AM101")) return "AM660";
    if(si4463 && !strcmp(preset_name, "FM238")) return "FM12KA";
    return preset_name;
}

static void subghz_setting_load_frequency_table(
    FrequencyList_t list,
    const uint32_t* frequencies,
    size_t max_count) {
    while(*frequencies) {
        if(FrequencyList_size(list) >= max_count) break;
        FrequencyList_push_back(list, *frequencies);
        frequencies++;
    }
}

static void subghz_setting_append_frequency_list(
    FrequencyList_t dest,
    FrequencyList_t src,
    size_t max_count) {
    for(size_t i = 0; i < FrequencyList_size(src); i++) {
        if(FrequencyList_size(dest) >= max_count) break;
        FrequencyList_push_back(dest, *FrequencyList_get(src, i));
    }
}

static void subghz_setting_rebuild_from_user(SubGhzSetting* instance) {
    uint32_t default_frequency = 0;
    if(FrequencyList_size(instance->frequencies)) {
        default_frequency = subghz_setting_get_default_frequency(instance);
    }

    FrequencyList_reset(instance->frequencies);
    FrequencyList_reset(instance->hopper_frequencies);

    if(instance->add_standard_frequencies) {
        subghz_setting_load_frequency_table(
            instance->frequencies, subghz_frequency_list, SUBGHZ_SETTING_MAX_FREQUENCY_COUNT);
        subghz_setting_load_frequency_table(
            instance->hopper_frequencies,
            subghz_hopper_frequency_list,
            SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT);
    }

    subghz_setting_append_frequency_list(
        instance->frequencies, instance->user_frequencies, SUBGHZ_SETTING_MAX_FREQUENCY_COUNT);
    subghz_setting_append_frequency_list(
        instance->hopper_frequencies,
        instance->user_hopper_frequencies,
        SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT);

    if(!FrequencyList_size(instance->frequencies)) {
        subghz_setting_load_frequency_table(
            instance->frequencies, subghz_frequency_list, SUBGHZ_SETTING_MAX_FREQUENCY_COUNT);
    }
    if(!FrequencyList_size(instance->hopper_frequencies)) {
        subghz_setting_load_frequency_table(
            instance->hopper_frequencies,
            subghz_hopper_frequency_list,
            SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT);
    }

    if(default_frequency) {
        subghz_setting_set_default_frequency(instance, default_frequency);
    }
}

static void subghz_setting_load_default_region(
    SubGhzSetting* instance,
    const uint32_t frequencies[],
    const uint32_t hopper_frequencies[]) {
    furi_assert(instance);

    FrequencyList_reset(instance->frequencies);
    FrequencyList_reset(instance->hopper_frequencies);
    FrequencyList_reset(instance->user_frequencies);
    FrequencyList_reset(instance->user_hopper_frequencies);
    instance->add_standard_frequencies = true;
    subghz_setting_preset_reset(instance);

    while(*frequencies) {
        FrequencyList_push_back(instance->frequencies, *frequencies);
        frequencies++;
    }

    while(*hopper_frequencies) {
        FrequencyList_push_back(instance->hopper_frequencies, *hopper_frequencies);
        hopper_frequencies++;
    }

    subghz_setting_load_default_preset(
        instance, "AM270", subghz_device_cc1101_preset_ook_270khz_async_regs);
    subghz_setting_load_default_preset(
        instance, "AM650", subghz_device_cc1101_preset_ook_650khz_async_regs);
    subghz_setting_load_default_preset(
        instance, "AM101", subghz_device_cc1101_preset_ook_101khz_16kbaud_async_regs);
    subghz_setting_load_default_preset(
        instance, "FM238", subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs);
    subghz_setting_load_default_preset(
        instance, "FM476", subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs);
    subghz_setting_load_default_preset(
        instance, "FM12K", subghz_device_cc1101_preset_2fsk_dev12khz_async_regs);
    subghz_setting_load_default_preset(
        instance, "FM20K", subghz_device_cc1101_preset_mercedes_async_regs);
    subghz_setting_load_default_preset(
        instance, "FM20KR", subghz_device_cc1101_preset_mercedes_rx_async_regs);
    subghz_setting_load_default_preset(
        instance, "FM14K", subghz_device_cc1101_preset_2fsk_dev14khz_38kbaud_async_regs);
    for(size_t i = 0; i < COUNT_OF(subghz_setting_si4463_presets); i++)
        subghz_setting_load_si4463_preset(instance, &subghz_setting_si4463_presets[i]);

    instance->builtin_preset_count =
        SubGhzSettingCustomPresetItemArray_size(instance->preset->data);
}

// Region check removed
void subghz_setting_load_default(SubGhzSetting* instance) {
    subghz_setting_load_default_region(
        instance, subghz_frequency_list, subghz_hopper_frequency_list);
}

void subghz_setting_load(SubGhzSetting* instance, const char* file_path) {
    furi_check(instance);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* fff_data_file = flipper_format_file_alloc(storage);

    FuriString* temp_str;
    temp_str = furi_string_alloc();
    uint32_t temp_data32;
    bool temp_bool;

    subghz_setting_load_default(instance);

    if(file_path) {
        if(!storage_file_exists(storage, file_path)) {
            FuriString* example_path = furi_string_alloc();
            furi_string_printf(example_path, "%s.example", file_path);
            if(storage_file_exists(storage, furi_string_get_cstr(example_path))) {
                storage_common_copy(storage, furi_string_get_cstr(example_path), file_path);
            }
            furi_string_free(example_path);
        }
        do {
            if(!flipper_format_file_open_existing(fff_data_file, file_path)) {
                FURI_LOG_I(TAG, "File is not used %s", file_path);
                break;
            }

            if(!flipper_format_read_header(fff_data_file, temp_str, &temp_data32)) {
                FURI_LOG_E(TAG, "Missing or incorrect header");
                break;
            }

            if((!strcmp(furi_string_get_cstr(temp_str), SUBGHZ_SETTING_FILE_TYPE)) &&
               temp_data32 == SUBGHZ_SETTING_FILE_VERSION) {
            } else {
                FURI_LOG_E(TAG, "Type or version mismatch");
                break;
            }

            // Standard frequencies (optional)
            temp_bool = true;
            flipper_format_read_bool(fff_data_file, "Add_standard_frequencies", &temp_bool, 1);
            instance->add_standard_frequencies = temp_bool;
            FURI_LOG_I(TAG, "%s standard frequencies", temp_bool ? "Keeping" : "Skipping");

            // Load frequencies
            if(!flipper_format_rewind(fff_data_file)) {
                FURI_LOG_E(TAG, "Rewind error");
                break;
            }
            while(flipper_format_read_uint32(
                fff_data_file, "Frequency", (uint32_t*)&temp_data32, 1)) {
                temp_data32 &= FREQUENCY_MASK;
                if(FrequencyList_size(instance->user_frequencies) >=
                   SUBGHZ_SETTING_MAX_FREQUENCY_COUNT) {
                    FURI_LOG_W(
                        TAG,
                        "Frequency list full (%u), ignoring further entries",
                        SUBGHZ_SETTING_MAX_FREQUENCY_COUNT);
                    break;
                }
                if(furi_hal_subghz_is_frequency_valid(temp_data32)) {
                    FURI_LOG_I(TAG, "Frequency loaded %lu", temp_data32);
                    FrequencyList_push_back(instance->user_frequencies, temp_data32);
                } else {
                    FURI_LOG_E(TAG, "Frequency not supported %lu", temp_data32);
                }
            }

            // Load hopper frequencies
            if(!flipper_format_rewind(fff_data_file)) {
                FURI_LOG_E(TAG, "Rewind error");
                break;
            }
            while(flipper_format_read_uint32(
                fff_data_file, "Hopper_frequency", (uint32_t*)&temp_data32, 1)) {
                temp_data32 &= FREQUENCY_MASK;
                if(FrequencyList_size(instance->user_hopper_frequencies) >=
                   SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT) {
                    FURI_LOG_W(
                        TAG,
                        "Hopper frequency list full (%u), ignoring further entries",
                        SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT);
                    break;
                }
                if(furi_hal_subghz_is_frequency_valid(temp_data32)) {
                    FURI_LOG_I(TAG, "Hopper frequency loaded %lu", temp_data32);
                    FrequencyList_push_back(instance->user_hopper_frequencies, temp_data32);
                } else {
                    FURI_LOG_E(TAG, "Hopper frequency not supported %lu", temp_data32);
                }
            }

            subghz_setting_rebuild_from_user(instance);

            // Default frequency (optional)
            if(!flipper_format_rewind(fff_data_file)) {
                FURI_LOG_E(TAG, "Rewind error");
                break;
            }
            if(flipper_format_read_uint32(fff_data_file, "Default_frequency", &temp_data32, 1)) {
                subghz_setting_set_default_frequency(instance, temp_data32);
            }

            // custom preset (optional)
            if(!flipper_format_rewind(fff_data_file)) {
                FURI_LOG_E(TAG, "Rewind error");
                break;
            }
            furi_string_reset(temp_str);
            while(flipper_format_read_string(fff_data_file, "Custom_preset_name", temp_str)) {
                if(SubGhzSettingCustomPresetItemArray_size(instance->preset->data) >=
                   SUBGHZ_SETTING_MAX_PRESET_COUNT) {
                    FURI_LOG_W(
                        TAG,
                        "Preset list full (%u), ignoring further custom presets",
                        SUBGHZ_SETTING_MAX_PRESET_COUNT);
                    break;
                }
                FURI_LOG_I(TAG, "Custom preset loaded %s", furi_string_get_cstr(temp_str));
                if(!subghz_setting_load_custom_preset(
                       instance, furi_string_get_cstr(temp_str), fff_data_file)) {
                    FURI_LOG_E(
                        TAG, "Failed to load custom preset %s", furi_string_get_cstr(temp_str));
                }
            }

        } while(false);
    }

    furi_string_free(temp_str);
    flipper_format_free(fff_data_file);
    furi_record_close(RECORD_STORAGE);

    if(!FrequencyList_size(instance->frequencies)) {
        FURI_LOG_E(TAG, "Empty static frequency list, loading default ones");
        subghz_setting_load_frequency_table(
            instance->frequencies, subghz_frequency_list, SUBGHZ_SETTING_MAX_FREQUENCY_COUNT);
    }
    if(!FrequencyList_size(instance->hopper_frequencies)) {
        FURI_LOG_E(TAG, "Empty hopper frequency list, loading default ones");
        subghz_setting_load_frequency_table(
            instance->hopper_frequencies,
            subghz_hopper_frequency_list,
            SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT);
    }
}

void subghz_setting_set_default_frequency(SubGhzSetting* instance, uint32_t frequency_to_setup) {
    for
        M_EACH(frequency, instance->frequencies, FrequencyList_t) {
            *frequency &= FREQUENCY_MASK;
            if(*frequency == frequency_to_setup) {
                *frequency |= FREQUENCY_FLAG_DEFAULT;
            }
        }
}

size_t subghz_setting_get_frequency_count(SubGhzSetting* instance) {
    furi_check(instance);
    return FrequencyList_size(instance->frequencies);
}

size_t subghz_setting_get_hopper_frequency_count(SubGhzSetting* instance) {
    furi_check(instance);
    return FrequencyList_size(instance->hopper_frequencies);
}

size_t subghz_setting_get_preset_count(SubGhzSetting* instance) {
    furi_check(instance);
    return SubGhzSettingCustomPresetItemArray_size(instance->preset->data);
}

const char* subghz_setting_get_preset_name(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    if(idx >= SubGhzSettingCustomPresetItemArray_size(instance->preset->data)) {
        idx = 0;
    }
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_get(instance->preset->data, idx);
    return furi_string_get_cstr(item->custom_preset_name);
}

int subghz_setting_get_inx_preset_by_name(SubGhzSetting* instance, const char* preset_name) {
    furi_check(instance);
    size_t idx = 0;
    for
        M_EACH(item, instance->preset->data, SubGhzSettingCustomPresetItemArray_t) {
            if(strcmp(furi_string_get_cstr(item->custom_preset_name), preset_name) == 0) {
                return idx;
            }
            idx++;
        }
    FURI_LOG_E(TAG, "No preset with name \"%s\"", preset_name);
    return -1;
}

bool subghz_setting_load_custom_preset(
    SubGhzSetting* instance,
    const char* preset_name,
    FlipperFormat* fff_data_file) {
    furi_check(instance);
    furi_check(preset_name);
    furi_check(fff_data_file);

    uint32_t temp_data32 = 0;
    if(!flipper_format_get_value_count(fff_data_file, "Custom_preset_data", &temp_data32)) {
        FURI_LOG_E(TAG, "Missing Custom_preset_data");
        return false;
    }
    if(!temp_data32 || (temp_data32 % 2)) {
        FURI_LOG_E(TAG, "Integrity error Custom_preset_data");
        return false;
    }
    if(temp_data32 > SUBGHZ_SETTING_MAX_PRESET_DATA_SIZE) {
        FURI_LOG_E(
            TAG,
            "Custom_preset_data too large (%lu > %u)",
            temp_data32,
            SUBGHZ_SETTING_MAX_PRESET_DATA_SIZE);
        return false;
    }

    uint8_t* data = malloc(temp_data32);
    if(!data) {
        FURI_LOG_E(TAG, "Out of memory for Custom_preset_data (%lu)", temp_data32);
        return false;
    }
    if(!flipper_format_read_hex(fff_data_file, "Custom_preset_data", data, temp_data32)) {
        FURI_LOG_E(TAG, "Missing Custom_preset_data");
        free(data);
        return false;
    }

    size_t idx = 0;
    for
        M_EACH(item, instance->preset->data, SubGhzSettingCustomPresetItemArray_t) {
            if(strcmp(furi_string_get_cstr(item->custom_preset_name), preset_name) == 0) {
                if(idx < instance->builtin_preset_count) {
                    FURI_LOG_W(TAG, "Keeping builtin preset %s, ignoring file copy", preset_name);
                    free(data);
                    return true;
                }
                free(item->custom_preset_data);
                item->custom_preset_data = data;
                item->custom_preset_data_size = temp_data32;
                return true;
            }
            idx++;
        }

    if(SubGhzSettingCustomPresetItemArray_size(instance->preset->data) >=
       SUBGHZ_SETTING_MAX_PRESET_COUNT) {
        FURI_LOG_E(TAG, "Preset list full, cannot load %s", preset_name);
        free(data);
        return false;
    }

    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_push_raw(instance->preset->data);
    item->custom_preset_name = furi_string_alloc_set(preset_name);
    item->custom_preset_data = data;
    item->custom_preset_data_size = temp_data32;
    return true;
}

bool subghz_setting_delete_custom_preset(SubGhzSetting* instance, const char* preset_name) {
    furi_check(instance);
    furi_check(preset_name);

    size_t idx = SubGhzSettingCustomPresetItemArray_size(instance->preset->data);
    SubGhzSettingCustomPresetItemArray_it_t it;
    SubGhzSettingCustomPresetItemArray_it_last(it, instance->preset->data);
    while(!SubGhzSettingCustomPresetItemArray_end_p(it)) {
        if(idx-- <= instance->builtin_preset_count) break;

        SubGhzSettingCustomPresetItem* item = SubGhzSettingCustomPresetItemArray_ref(it);
        if(strcmp(furi_string_get_cstr(item->custom_preset_name), preset_name) == 0) {
            furi_string_free(item->custom_preset_name);
            free(item->custom_preset_data);
            SubGhzSettingCustomPresetItemArray_remove(instance->preset->data, it);
            return true;
        }
        SubGhzSettingCustomPresetItemArray_previous(it);
    }
    return false;
}

uint8_t* subghz_setting_get_preset_data(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_get(instance->preset->data, idx);
    return item->custom_preset_data;
}

size_t subghz_setting_get_preset_data_size(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_get(instance->preset->data, idx);
    return item->custom_preset_data_size;
}

uint8_t* subghz_setting_get_preset_data_by_name(SubGhzSetting* instance, const char* preset_name) {
    furi_check(instance);
    int idx = subghz_setting_get_inx_preset_by_name(instance, preset_name);
    if(idx < 0) {
        return NULL;
    }
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_get(instance->preset->data, idx);
    return item->custom_preset_data;
}

uint32_t subghz_setting_get_frequency(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    if(idx < FrequencyList_size(instance->frequencies)) {
        return (*FrequencyList_get(instance->frequencies, idx)) & FREQUENCY_MASK;
    } else {
        return 0;
    }
}

uint32_t subghz_setting_get_hopper_frequency(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    if(idx < FrequencyList_size(instance->hopper_frequencies)) {
        return *FrequencyList_get(instance->hopper_frequencies, idx);
    } else {
        return 0;
    }
}

uint32_t subghz_setting_get_frequency_default_index(SubGhzSetting* instance) {
    furi_check(instance);
    for(size_t i = 0; i < FrequencyList_size(instance->frequencies); i++) {
        uint32_t frequency = *FrequencyList_get(instance->frequencies, i);
        if(frequency & FREQUENCY_FLAG_DEFAULT) {
            return i;
        }
    }
    return 0;
}

uint32_t subghz_setting_get_default_frequency(SubGhzSetting* instance) {
    furi_check(instance);
    return subghz_setting_get_frequency(
        instance, subghz_setting_get_frequency_default_index(instance));
}

uint8_t subghz_setting_customs_presets_to_log(SubGhzSetting* instance) {
    furi_assert(instance);
#ifndef FURI_DEBUG
    FURI_LOG_I(TAG, "Logging loaded presets allow only Debug build");
#else
    uint8_t count = 0;
    FuriString* temp = furi_string_alloc();

    FURI_LOG_I(TAG, "Loaded presets");
    for
        M_EACH(item, instance->preset->data, SubGhzSettingCustomPresetItemArray_t) {
            furi_string_reset(temp);

            for(uint8_t i = 0; i < item->custom_preset_data_size; i++) {
                furi_string_cat_printf(temp, "%02u ", item->custom_preset_data[i]);
            }

            FURI_LOG_I(
                TAG, "%u  -  %s", count + 1, furi_string_get_cstr(item->custom_preset_name));
            FURI_LOG_I(TAG, "  Size: %u", item->custom_preset_data_size);
            FURI_LOG_I(TAG, "  Data: %s", furi_string_get_cstr(temp));

            count++;
        }

    furi_string_free(temp);

    return count;
#endif
    return 0;
}

size_t subghz_setting_get_builtin_preset_count(SubGhzSetting* instance) {
    furi_check(instance);
    return instance->builtin_preset_count;
}

static bool subghz_setting_frequency_list_contains(FrequencyList_t list, uint32_t frequency) {
    for
        M_EACH(f, list, FrequencyList_t) {
            if((*f & FREQUENCY_MASK) == frequency) return true;
        }
    return false;
}

static bool
    subghz_setting_user_list_push(FrequencyList_t list, uint32_t frequency, size_t max_count) {
    if(!furi_hal_subghz_is_frequency_valid(frequency)) return false;
    if(FrequencyList_size(list) >= max_count) return false;
    if(subghz_setting_frequency_list_contains(list, frequency)) return false;
    FrequencyList_push_back(list, frequency);
    return true;
}

static size_t subghz_setting_user_list_remove_value(FrequencyList_t list, uint32_t frequency) {
    size_t removed = 0;
    FrequencyList_it_t it;
    FrequencyList_it(it, list);
    while(!FrequencyList_end_p(it)) {
        if((*FrequencyList_ref(it) & FREQUENCY_MASK) == frequency) {
            FrequencyList_remove(list, it);
            removed++;
        } else {
            FrequencyList_next(it);
        }
    }
    return removed;
}

static uint32_t subghz_setting_frequency_list_get(FrequencyList_t list, size_t idx) {
    if(idx < FrequencyList_size(list)) {
        return (*FrequencyList_get(list, idx)) & FREQUENCY_MASK;
    }
    return 0;
}

static bool subghz_setting_frequency_list_delete(FrequencyList_t list, size_t idx) {
    size_t i = 0;
    FrequencyList_it_t it;
    for(FrequencyList_it(it, list); !FrequencyList_end_p(it); FrequencyList_next(it)) {
        if(i == idx) {
            FrequencyList_remove(list, it);
            return true;
        }
        i++;
    }
    return false;
}

bool subghz_setting_get_add_standard_frequencies(SubGhzSetting* instance) {
    furi_check(instance);
    return instance->add_standard_frequencies;
}

void subghz_setting_set_add_standard_frequencies(SubGhzSetting* instance, bool add) {
    furi_check(instance);
    instance->add_standard_frequencies = add;
    subghz_setting_rebuild_from_user(instance);
}

size_t subghz_setting_get_user_frequency_count(SubGhzSetting* instance) {
    furi_check(instance);
    return FrequencyList_size(instance->user_frequencies);
}

uint32_t subghz_setting_get_user_frequency(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    return subghz_setting_frequency_list_get(instance->user_frequencies, idx);
}

bool subghz_setting_add_user_frequency(SubGhzSetting* instance, uint32_t frequency) {
    furi_check(instance);
    if(!subghz_setting_user_list_push(
           instance->user_frequencies, frequency, SUBGHZ_SETTING_MAX_FREQUENCY_COUNT)) {
        return false;
    }
    subghz_setting_rebuild_from_user(instance);
    return true;
}

size_t subghz_setting_delete_user_frequency_value(SubGhzSetting* instance, uint32_t frequency) {
    furi_check(instance);
    size_t removed = subghz_setting_user_list_remove_value(instance->user_frequencies, frequency);
    if(removed) subghz_setting_rebuild_from_user(instance);
    return removed;
}

size_t subghz_setting_get_user_hopper_frequency_count(SubGhzSetting* instance) {
    furi_check(instance);
    return FrequencyList_size(instance->user_hopper_frequencies);
}

uint32_t subghz_setting_get_user_hopper_frequency(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    return subghz_setting_frequency_list_get(instance->user_hopper_frequencies, idx);
}

bool subghz_setting_add_user_hopper_frequency(SubGhzSetting* instance, uint32_t frequency) {
    furi_check(instance);
    if(!subghz_setting_user_list_push(
           instance->user_hopper_frequencies,
           frequency,
           SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT)) {
        return false;
    }
    subghz_setting_rebuild_from_user(instance);
    return true;
}

size_t
    subghz_setting_delete_user_hopper_frequency_value(SubGhzSetting* instance, uint32_t frequency) {
    furi_check(instance);
    size_t removed =
        subghz_setting_user_list_remove_value(instance->user_hopper_frequencies, frequency);
    if(removed) subghz_setting_rebuild_from_user(instance);
    return removed;
}

bool subghz_setting_add_frequency(SubGhzSetting* instance, uint32_t frequency) {
    return subghz_setting_add_user_frequency(instance, frequency);
}

bool subghz_setting_delete_frequency(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    uint32_t frequency = subghz_setting_get_frequency(instance, idx);
    if(!frequency) return false;
    if(subghz_setting_delete_user_frequency_value(instance, frequency)) return true;
    return subghz_setting_frequency_list_delete(instance->frequencies, idx);
}

bool subghz_setting_add_hopper_frequency(SubGhzSetting* instance, uint32_t frequency) {
    return subghz_setting_add_user_hopper_frequency(instance, frequency);
}

bool subghz_setting_delete_hopper_frequency(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    uint32_t frequency = subghz_setting_get_hopper_frequency(instance, idx);
    if(!frequency) return false;
    if(subghz_setting_delete_user_hopper_frequency_value(instance, frequency)) return true;
    return subghz_setting_frequency_list_delete(instance->hopper_frequencies, idx);
}

bool subghz_setting_preset_is_async_serial(SubGhzSetting* instance, size_t preset_idx) {
    furi_check(instance);
    if(preset_idx >= subghz_setting_get_preset_count(instance)) return false;

    const uint8_t* data = subghz_setting_get_preset_data(instance, preset_idx);
    bool gd0_serial = false;
    bool async_format = false;

    for(size_t i = 0; data[i]; i += 2) {
        if(data[i] == CC1101_IOCFG0) {
            gd0_serial = (data[i + 1] == CC1101IocfgSerialDataOutput);
        } else if(data[i] == CC1101_PKTCTRL0) {
            async_format = ((data[i + 1] & 0x30) == 0x30);
        }
    }

    return gd0_serial && async_format;
}

bool subghz_setting_preset_is_si4463(SubGhzSetting* instance, size_t preset_idx) {
    if(preset_idx >= subghz_setting_get_preset_count(instance)) return false;
    const uint8_t* data = subghz_setting_get_preset_data(instance, preset_idx);
    for(size_t i = 0; data[i]; i += 2) {
        if(data[i] == 0xFC) return true;
    }
    return false;
}

bool subghz_setting_has_si4463_preset(SubGhzSetting* instance) {
    for(size_t i = 0; i < subghz_setting_get_preset_count(instance); i++) {
        if(subghz_setting_preset_is_si4463(instance, i)) return true;
    }
    return false;
}

bool subghz_setting_is_hopper_frequency(SubGhzSetting* instance, uint32_t frequency) {
    furi_check(instance);
    for
        M_EACH(f, instance->hopper_frequencies, FrequencyList_t) {
            if((*f & FREQUENCY_MASK) == frequency) return true;
        }
    return false;
}

bool subghz_setting_rename_preset(SubGhzSetting* instance, size_t idx, const char* new_name) {
    furi_check(instance);
    furi_check(new_name);
    if(idx < instance->builtin_preset_count ||
       idx >= SubGhzSettingCustomPresetItemArray_size(instance->preset->data)) {
        return false;
    }
    SubGhzSettingCustomPresetItem* item =
        SubGhzSettingCustomPresetItemArray_get(instance->preset->data, idx);
    furi_string_set(item->custom_preset_name, new_name);
    return true;
}

bool subghz_setting_delete_preset(SubGhzSetting* instance, size_t idx) {
    furi_check(instance);
    if(idx < instance->builtin_preset_count ||
       idx >= SubGhzSettingCustomPresetItemArray_size(instance->preset->data)) {
        return false;
    }
    size_t i = 0;
    SubGhzSettingCustomPresetItemArray_it_t it;
    for(SubGhzSettingCustomPresetItemArray_it(it, instance->preset->data);
        !SubGhzSettingCustomPresetItemArray_end_p(it);
        SubGhzSettingCustomPresetItemArray_next(it)) {
        if(i == idx) {
            SubGhzSettingCustomPresetItem* item = SubGhzSettingCustomPresetItemArray_ref(it);
            furi_string_free(item->custom_preset_name);
            free(item->custom_preset_data);
            SubGhzSettingCustomPresetItemArray_remove(instance->preset->data, it);
            return true;
        }
        i++;
    }
    return false;
}

void subghz_setting_reset_to_default(SubGhzSetting* instance) {
    furi_check(instance);
    subghz_setting_load_default(instance);
}

bool subghz_setting_save(SubGhzSetting* instance, const char* file_path) {
    furi_check(instance);
    furi_check(file_path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool saved = false;

    do {
        if(!flipper_format_file_open_always(file, file_path)) break;
        if(!flipper_format_write_header_cstr(
               file, SUBGHZ_SETTING_FILE_TYPE, SUBGHZ_SETTING_FILE_VERSION))
            break;

        if(!flipper_format_write_bool(
               file, "Add_standard_frequencies", &instance->add_standard_frequencies, 1))
            break;

        uint32_t default_frequency = subghz_setting_get_default_frequency(instance);
        if(default_frequency) {
            flipper_format_write_uint32(file, "Default_frequency", &default_frequency, 1);
        }

        for(size_t i = 0; i < FrequencyList_size(instance->user_frequencies); i++) {
            uint32_t frequency = (*FrequencyList_get(instance->user_frequencies, i)) &
                                 FREQUENCY_MASK;
            flipper_format_write_uint32(file, "Frequency", &frequency, 1);
        }

        for(size_t i = 0; i < FrequencyList_size(instance->user_hopper_frequencies); i++) {
            uint32_t frequency = (*FrequencyList_get(instance->user_hopper_frequencies, i)) &
                                 FREQUENCY_MASK;
            flipper_format_write_uint32(file, "Hopper_frequency", &frequency, 1);
        }

        for(size_t i = instance->builtin_preset_count;
            i < SubGhzSettingCustomPresetItemArray_size(instance->preset->data);
            i++) {
            SubGhzSettingCustomPresetItem* item =
                SubGhzSettingCustomPresetItemArray_get(instance->preset->data, i);
            flipper_format_write_string_cstr(
                file, "Custom_preset_name", furi_string_get_cstr(item->custom_preset_name));
            flipper_format_write_string_cstr(file, "Custom_preset_module", "CC1101");
            flipper_format_write_hex(
                file,
                "Custom_preset_data",
                item->custom_preset_data,
                item->custom_preset_data_size);
        }

        saved = true;
    } while(false);

    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return saved;
}
