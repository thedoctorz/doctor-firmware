#include "subghz_history.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/receiver.h>

#include <furi.h>
#include <string.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>

#define SUBGHZ_HISTORY_MAX       55
#define SUBGHZ_HISTORY_FREE_HEAP 20480
#define TAG                      "SubGhzHistory"

typedef struct {
    FuriString* item_str;
    FlipperFormat* flipper_string;
    uint8_t type;
    uint8_t source; // dual RX radio source marker: 0 = internal, 1 = external

    const SubGhzProtocol* protocol;
    SubGhzHistoryButtonIcon button_icon;
    SubGhzRadioPreset* preset;
    DateTime datetime;
} SubGhzHistoryItem;

ARRAY_DEF(SubGhzHistoryItemArray, SubGhzHistoryItem, M_POD_OPLIST) //-V658

#define M_OPL_SubGhzHistoryItemArray_t() ARRAY_OPLIST(SubGhzHistoryItemArray, M_POD_OPLIST)

typedef struct {
    SubGhzHistoryItemArray_t data;
} SubGhzHistoryStruct;

struct SubGhzHistory {
    FuriMutex* format_mutex;
    uint16_t last_index_write;
    FuriString* tmp_string;
    SubGhzHistoryStruct* history;

    // GPS geotag applied to newly added items
    bool gps_valid;
    float gps_latitude;
    float gps_longitude;
};

SubGhzHistory* subghz_history_alloc(void) {
    SubGhzHistory* instance = malloc(sizeof(SubGhzHistory));
    instance->format_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->tmp_string = furi_string_alloc();
    instance->history = malloc(sizeof(SubGhzHistoryStruct));
    SubGhzHistoryItemArray_init(instance->history->data);

    SubGhzHistoryItemArray_reserve(instance->history->data, SUBGHZ_HISTORY_MAX);
    instance->last_index_write = 0;
    instance->gps_valid = false;
    instance->gps_latitude = 0;
    instance->gps_longitude = 0;
    return instance;
}

void subghz_history_set_location(
    SubGhzHistory* instance,
    bool valid,
    float latitude,
    float longitude) {
    furi_assert(instance);
    instance->gps_valid = valid;
    instance->gps_latitude = latitude;
    instance->gps_longitude = longitude;
}

void subghz_history_free(SubGhzHistory* instance) {
    furi_assert(instance);
    furi_string_free(instance->tmp_string);
    for
        M_EACH(item, instance->history->data, SubGhzHistoryItemArray_t) {
            furi_string_free(item->item_str);
            furi_string_free(item->preset->name);
            free(item->preset->data);
            free(item->preset);
            flipper_format_free(item->flipper_string);
            item->type = 0;
        }
    SubGhzHistoryItemArray_clear(instance->history->data);
    free(instance->history);
    furi_mutex_free(instance->format_mutex);
    free(instance);
}

uint32_t subghz_history_get_frequency(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->preset->frequency;
}

SubGhzRadioPreset* subghz_history_get_radio_preset(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->preset;
}

const char* subghz_history_get_preset(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return furi_string_get_cstr(item->preset->name);
}

void subghz_history_reset(SubGhzHistory* instance) {
    furi_assert(instance);
    furi_string_reset(instance->tmp_string);
    for
        M_EACH(item, instance->history->data, SubGhzHistoryItemArray_t) {
            furi_string_free(item->item_str);
            furi_string_free(item->preset->name);
            free(item->preset->data);
            free(item->preset);
            flipper_format_free(item->flipper_string);
            item->type = 0;
        }
    SubGhzHistoryItemArray_reset(instance->history->data);
    instance->last_index_write = 0;
}

void subghz_history_delete_item(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    furi_mutex_acquire(instance->format_mutex, FuriWaitForever);

    if(idx < SubGhzHistoryItemArray_size(instance->history->data)) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
        furi_string_free(item->item_str);
        furi_string_free(item->preset->name);
        free(item->preset->data);
        free(item->preset);
        flipper_format_free(item->flipper_string);
        item->type = 0;
        SubGhzHistoryItemArray_remove_v(instance->history->data, idx, idx + 1);
        instance->last_index_write--;
    }
    furi_mutex_release(instance->format_mutex);
}

uint16_t subghz_history_get_item(SubGhzHistory* instance) {
    furi_assert(instance);
    return instance->last_index_write;
}

const SubGhzProtocol* subghz_history_get_protocol(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    if(idx >= instance->last_index_write) return NULL;
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->protocol;
}

uint8_t subghz_history_get_type_protocol(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item->type;
}

SubGhzHistoryButtonIcon subghz_history_get_button_icon(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item ? item->button_icon : SubGhzHistoryButtonIconDefault;
}

uint8_t subghz_history_get_source(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    return item ? item->source : 0;
}

const char* subghz_history_get_protocol_name(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(!item || !item->flipper_string) {
        FURI_LOG_E(TAG, "Missing Item");
        furi_string_reset(instance->tmp_string);
        return furi_string_get_cstr(instance->tmp_string);
    }
    flipper_format_rewind(item->flipper_string);
    if(!flipper_format_read_string(item->flipper_string, "Protocol", instance->tmp_string)) {
        FURI_LOG_E(TAG, "Missing Protocol");
        furi_string_reset(instance->tmp_string);
    }
    return furi_string_get_cstr(instance->tmp_string);
}

DateTime subghz_history_get_datetime(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(item) {
        return item->datetime;
    } else {
        return (DateTime){};
    }
}

FlipperFormat* subghz_history_get_raw_data(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    if(item->flipper_string) {
        return item->flipper_string;
    } else {
        return NULL;
    }
}

FlipperFormat* subghz_history_get_raw_data_copy(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    FlipperFormat* copy = flipper_format_string_alloc();
    bool copied = false;

    furi_mutex_acquire(instance->format_mutex, FuriWaitForever);
    if(idx < instance->last_index_write) {
        FlipperFormat* raw = subghz_history_get_raw_data(instance, idx);
        if(raw) {
            Stream* source = flipper_format_get_raw_stream(raw);
            const size_t position = stream_tell(source);
            const size_t size = stream_size(source);
            copied = size && stream_copy_full(source, flipper_format_get_raw_stream(copy)) == size;
            stream_seek(source, position, StreamOffsetFromStart);
        }
    }
    furi_mutex_release(instance->format_mutex);

    if(!copied) {
        flipper_format_free(copy);
        copy = NULL;
    }
    return copy;
}
bool subghz_history_get_text_space_left(SubGhzHistory* instance, FuriString* output) {
    furi_assert(instance);
    if(memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP) {
        if(output != NULL) furi_string_printf(output, "  RAM almost FULL");
        return true;
    }
    if(instance->last_index_write == SUBGHZ_HISTORY_MAX) {
        if(output != NULL) furi_string_printf(output, "   Memory is FULL");
        return true;
    }
    if(output != NULL)
        furi_string_printf(output, "%02u/%02u", instance->last_index_write, SUBGHZ_HISTORY_MAX);
    return false;
}

uint16_t subghz_history_get_last_index(SubGhzHistory* instance) {
    return instance->last_index_write;
}
void subghz_history_get_text_item_menu(SubGhzHistory* instance, FuriString* output, uint16_t idx) {
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    furi_string_set(output, item->item_str);
}

void subghz_history_get_time_item_menu(SubGhzHistory* instance, FuriString* output, uint16_t idx) {
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
    DateTime* t = &item->datetime;
    furi_string_printf(output, "%.2d:%.2d:%.2d ", t->hour, t->minute, t->second);
}

static bool subghz_history_read_string(FlipperFormat* ff, const char* key, FuriString* value) {
    flipper_format_rewind(ff);
    return flipper_format_read_string(ff, key, value);
}

static bool subghz_history_read_uint32(FlipperFormat* ff, const char* key, uint32_t* value) {
    flipper_format_rewind(ff);
    return flipper_format_read_uint32(ff, key, value, 1);
}

static bool subghz_history_read_key2_u64(FlipperFormat* ff, uint64_t* value) {
    uint8_t key_data[sizeof(uint64_t)] = {0};
    flipper_format_rewind(ff);
    bool have_key = flipper_format_read_hex(ff, "Key_2", key_data, sizeof(key_data));
    if(!have_key) {
        flipper_format_rewind(ff);
        have_key = flipper_format_read_hex(ff, "Key2", key_data, sizeof(key_data));
    }
    if(!have_key) return false;

    *value = 0;
    for(uint8_t i = 0; i < sizeof(key_data); i++) {
        *value = (*value << 8) | key_data[i];
    }
    return true;
}

static bool
    subghz_history_match_optional_uint32(FlipperFormat* a, FlipperFormat* b, const char* key) {
    uint32_t value_a = 0;
    uint32_t value_b = 0;
    const bool have_a = subghz_history_read_uint32(a, key, &value_a);
    const bool have_b = subghz_history_read_uint32(b, key, &value_b);

    return (!have_a && !have_b) || (have_a && have_b && (value_a == value_b));
}

static bool subghz_history_keys_match(FlipperFormat* stored, FlipperFormat* candidate) {
    uint64_t stored_key = 0;
    uint64_t candidate_key = 0;
    if(!subghz_block_generic_read_key_u64(candidate, &candidate_key)) return false;
    if(!subghz_block_generic_read_key_u64(stored, &stored_key)) return false;
    if(stored_key != candidate_key) return false;

    uint64_t stored_key2 = 0;
    uint64_t candidate_key2 = 0;
    bool stored_has_key2 = subghz_history_read_key2_u64(stored, &stored_key2);
    bool candidate_has_key2 = subghz_history_read_key2_u64(candidate, &candidate_key2);
    return (stored_has_key2 == candidate_has_key2) &&
           (!stored_has_key2 || (stored_key2 == candidate_key2));
}

static bool subghz_history_is_duplicate_item(FlipperFormat* stored, FlipperFormat* candidate) {
    FuriString* stored_protocol = furi_string_alloc();
    FuriString* candidate_protocol = furi_string_alloc();
    bool duplicate = false;

    do {
        if(!subghz_history_read_string(stored, "Protocol", stored_protocol)) break;
        if(!subghz_history_read_string(candidate, "Protocol", candidate_protocol)) break;
        if(!furi_string_equal(stored_protocol, candidate_protocol)) break;

        uint32_t stored_cnt = 0;
        uint32_t candidate_cnt = 0;
        bool stored_has_cnt = subghz_history_read_uint32(stored, "Cnt", &stored_cnt);
        bool candidate_has_cnt = subghz_history_read_uint32(candidate, "Cnt", &candidate_cnt);
        if(!candidate_has_cnt) {
            stored_has_cnt = subghz_history_read_uint32(stored, "RawCnt", &stored_cnt);
            candidate_has_cnt = subghz_history_read_uint32(candidate, "RawCnt", &candidate_cnt);
        }

        if(candidate_has_cnt && stored_has_cnt) {
            if(stored_cnt != candidate_cnt) break;
            if(!subghz_history_match_optional_uint32(stored, candidate, "Serial")) break;
            if(!subghz_history_match_optional_uint32(stored, candidate, "Btn")) break;
            if(!subghz_history_match_optional_uint32(stored, candidate, "Type")) break;
            duplicate = stored_cnt == 0 ? subghz_history_keys_match(stored, candidate) : true;
            break;
        }

        duplicate = subghz_history_keys_match(stored, candidate);
    } while(false);

    furi_string_free(candidate_protocol);
    furi_string_free(stored_protocol);
    return duplicate;
}

static bool subghz_history_is_duplicate(SubGhzHistory* instance, FlipperFormat* candidate, uint8_t source) {
    for
        M_EACH(item, instance->history->data, SubGhzHistoryItemArray_t) {
            if(item->source == source && item->flipper_string &&
               subghz_history_is_duplicate_item(item->flipper_string, candidate)) {
                return true;
            }
        }
    return false;
}

static bool
    subghz_history_protocol_shows_counter(const char* protocol_name, SubGhzProtocolType type) {
    if(!protocol_name) return false;
    if(type == SubGhzProtocolTypeStatic) return false;
    if(!strcmp(protocol_name, "Hollarm") || !strcmp(protocol_name, "Nord ICE") ||
       !strcmp(protocol_name, "Dooya")) {
        return false;
    }
    if(!strcmp(protocol_name, "Fiat V0") || !strcmp(protocol_name, "KL Unknown")) {
        return false;
    }
    return true;
}

static void subghz_history_format_item_string(
    FlipperFormat* ff,
    FuriString* output,
    SubGhzProtocolType type) {
    FuriString* protocol = furi_string_alloc();
    FuriString* manufacture = furi_string_alloc();
    uint32_t counter = 0;
    bool have_counter = false;

    do {
        if(!subghz_history_read_string(ff, "Protocol", protocol)) {
            furi_string_set(output, "?");
            break;
        }

        if(!strcmp(furi_string_get_cstr(protocol), "KeeLoq") &&
           subghz_history_read_string(ff, "Manufacture", manufacture)) {
            furi_string_printf(protocol, "KL %s", furi_string_get_cstr(manufacture));
        } else if(!strcmp(furi_string_get_cstr(protocol), "Kia V3/V4")) {
            uint32_t kia_version = 0;
            if(subghz_history_read_uint32(ff, "KIAVersion", &kia_version)) {
                furi_string_set(protocol, kia_version == 0 ? "Kia V4" : "Kia V3");
            }
        } else if(!strcmp(furi_string_get_cstr(protocol), "Urmet")) {
            uint32_t bit = 0;
            if(subghz_history_read_uint32(ff, "Bit", &bit)) {
                furi_string_printf(protocol, "Urmet %lubit", bit);
            }
        }

        if(subghz_history_protocol_shows_counter(furi_string_get_cstr(protocol), type)) {
            have_counter = subghz_history_read_uint32(ff, "Cnt", &counter);
            if(!have_counter) {
                have_counter = subghz_history_read_uint32(ff, "RawCnt", &counter);
            }
        }

        if(have_counter) {
            furi_string_printf(output, "%s - %lX", furi_string_get_cstr(protocol), counter);
        } else {
            furi_string_set(output, protocol);
        }
    } while(false);

    furi_string_free(manufacture);
    furi_string_free(protocol);
}

static SubGhzHistoryButtonIcon subghz_history_button_icon_from_action_text(const char* text) {
    if(!text) return SubGhzHistoryButtonIconDefault;
    if(strstr(text, "Unlock") || strstr(text, "Open")) {
        return SubGhzHistoryButtonIconUnlock;
    } else if(strstr(text, "Lock") || strstr(text, "Close")) {
        return SubGhzHistoryButtonIconLock;
    }

    return SubGhzHistoryButtonIconDefault;
}

static SubGhzHistoryButtonIcon subghz_history_button_icon_from_text(const char* text) {
    const char* button_text = strstr(text, "Btn");
    if(button_text) {
        SubGhzHistoryButtonIcon icon = subghz_history_button_icon_from_action_text(button_text);
        if(icon != SubGhzHistoryButtonIconDefault) return icon;
    }

    return subghz_history_button_icon_from_action_text(text);
}

static SubGhzHistoryButtonIcon
    subghz_history_button_icon_from_lock_unlock(bool lock, bool unlock) {
    if(unlock) return SubGhzHistoryButtonIconUnlock;
    if(lock) return SubGhzHistoryButtonIconLock;
    return SubGhzHistoryButtonIconDefault;
}

static SubGhzHistoryButtonIcon subghz_history_button_icon_from_fields(FlipperFormat* ff) {
    FuriString* protocol = furi_string_alloc();
    FuriString* display = furi_string_alloc();
    uint32_t btn = 0;
    uint32_t type = 0;
    bool have_type = false;
    SubGhzHistoryButtonIcon icon = SubGhzHistoryButtonIconDefault;

    do {
        if(subghz_history_read_string(ff, "Disp", display)) {
            icon = subghz_history_button_icon_from_text(furi_string_get_cstr(display));
            if(icon != SubGhzHistoryButtonIconDefault) break;
        }

        if(!subghz_history_read_string(ff, "Protocol", protocol)) break;
        if(!subghz_history_read_uint32(ff, "Btn", &btn)) break;

        have_type = subghz_history_read_uint32(ff, "Type", &type);
        const char* protocol_name = furi_string_get_cstr(protocol);
        const uint8_t low_nibble = btn & 0x0FU;

        if(strstr(protocol_name, "Chrysler")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x01U, btn == 0x02U);
        } else if(!strcmp(protocol_name, "Fiat V0")) {
            icon = subghz_history_button_icon_from_lock_unlock(
                low_nibble >= 0x04U && low_nibble <= 0x07U,
                low_nibble >= 0x08U && low_nibble <= 0x0BU);
        } else if(!strcmp(protocol_name, "Fiat V1")) {
            icon = subghz_history_button_icon_from_lock_unlock(
                btn == 0x04U || btn == 0x01U, btn == 0x08U);
        } else if(!strcmp(protocol_name, "Fiat V2")) {
            const uint8_t button = btn >> 6U;
            icon = subghz_history_button_icon_from_lock_unlock(button == 0x02U, button == 0x03U);
        } else if(!strcmp(protocol_name, "Ford V0")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x02U, btn == 0x04U);
        } else if(!strcmp(protocol_name, "Ford V1") || !strcmp(protocol_name, "Ford V3")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x01U, btn == 0x02U);
        } else if(!strcmp(protocol_name, "Ford V2")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x10U, btn == 0x11U);
        } else if(!strcmp(protocol_name, "Honda Static")) {
            icon = subghz_history_button_icon_from_lock_unlock(
                btn == 0x01U || btn == 0x09U, btn == 0x02U);
        } else if(!strcmp(protocol_name, "Honda V1")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x08U, btn == 0x00U);
        } else if(!strcmp(protocol_name, "Honda V2")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x02U, btn == 0x04U);
        } else if(strstr(protocol_name, "Kia")) {
            if(!strcmp(protocol_name, "Kia V0") && have_type && type == 2U) {
                icon = subghz_history_button_icon_from_lock_unlock(btn == 0x03U, btn == 0x04U);
            } else {
                icon = subghz_history_button_icon_from_lock_unlock(btn == 0x01U, btn == 0x02U);
            }
        } else if(!strcmp(protocol_name, "Mazda V0")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x01U, btn == 0x02U);
        } else if(!strcmp(protocol_name, "Mazda V2")) {
            const uint8_t mazda_v2_btn = (uint8_t)(btn & 0x7FU);
            icon = subghz_history_button_icon_from_lock_unlock(
                mazda_v2_btn == 0x04U || mazda_v2_btn == 0x06U,
                mazda_v2_btn == 0x08U || mazda_v2_btn == 0x0AU);
        } else if(!strcmp(protocol_name, "Opel V1")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x04U, btn == 0x08U);
        } else if(!strcmp(protocol_name, "PSA")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x04U, btn == 0x01U);
        } else if(!strcmp(protocol_name, "Renault V0")) {
            icon = subghz_history_button_icon_from_lock_unlock(
                btn == 0x06U || (low_nibble >= 0x04U && low_nibble <= 0x07U),
                btn == 0x0AU || (low_nibble >= 0x08U && low_nibble <= 0x0BU));
        } else if(!strcmp(protocol_name, "Renault V1")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x01U, btn == 0x02U);
        } else if(!strcmp(protocol_name, "Mercedes V1")) {
            icon = subghz_history_button_icon_from_lock_unlock(btn == 0x00U, btn == 0x01U);
        } else if(!strcmp(protocol_name, "Subaru")) {
            icon = subghz_history_button_icon_from_lock_unlock(
                (btn & 0x03U) == 0x00U, (btn & 0x03U) == 0x01U);
        } else if(!strcmp(protocol_name, "VAG")) {
            icon = subghz_history_button_icon_from_lock_unlock(
                btn == 0x02U || btn == 0x20U, btn == 0x01U || btn == 0x10U);
        }
    } while(false);

    furi_string_free(display);
    furi_string_free(protocol);
    return icon;
}

bool subghz_history_add_to_history(
    SubGhzHistory* instance,
    void* context,
    SubGhzRadioPreset* preset) {
    return subghz_history_add_to_history_source(instance, context, preset, 0);
}

static bool subghz_history_add_to_history_source_locked(
    SubGhzHistory* instance,
    void* context,
    SubGhzRadioPreset* preset,
    uint8_t source) {
    furi_assert(instance);
    furi_assert(context);

    if(memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP) return false;
    if(instance->last_index_write >= SUBGHZ_HISTORY_MAX) return false;

    SubGhzProtocolDecoderBase* decoder_base = context;

    FlipperFormat* flipper_string = flipper_format_string_alloc();
    if(subghz_protocol_decoder_base_serialize(decoder_base, flipper_string, preset) !=
       SubGhzProtocolStatusOk) {
        flipper_format_free(flipper_string);
        return false;
    }

    if(subghz_history_is_duplicate(instance, flipper_string, source)) {
        flipper_format_free(flipper_string);
        return false;
    }

    SubGhzHistoryItem* item = SubGhzHistoryItemArray_push_raw(instance->history->data);
    item->preset = malloc(sizeof(SubGhzRadioPreset));
    item->type = decoder_base->protocol->type;
    item->protocol = decoder_base->protocol;
    item->source = source;
    item->preset->frequency = preset->frequency;
    item->preset->name = furi_string_alloc();
    furi_string_set(item->preset->name, preset->name);
    item->preset->data = NULL;
    if(preset->data && preset->data_size) {
        item->preset->data = malloc(preset->data_size);
        memcpy(item->preset->data, preset->data, preset->data_size);
    }
    item->preset->data_size = preset->data_size;
    furi_hal_rtc_get_datetime(&item->datetime);

    item->item_str = furi_string_alloc();
    item->flipper_string = flipper_string;
    subghz_history_format_item_string(item->flipper_string, item->item_str, item->type);
    furi_string_reset(instance->tmp_string);
    subghz_protocol_decoder_base_get_string(decoder_base, instance->tmp_string);
    item->button_icon =
        subghz_history_button_icon_from_text(furi_string_get_cstr(instance->tmp_string));
    if(item->button_icon == SubGhzHistoryButtonIconDefault) {
        item->button_icon = subghz_history_button_icon_from_fields(item->flipper_string);
    }

    // Geotag the item so that saving it writes the coordinates into the .sub file.
    // Appended last, after the duplicate check and item-string formatting above.
    if(instance->gps_valid) {
        Stream* stream = flipper_format_get_raw_stream(item->flipper_string);
        stream_seek(stream, 0, StreamOffsetFromEnd);
        flipper_format_write_float(item->flipper_string, "Lat", &instance->gps_latitude, 1);
        flipper_format_write_float(item->flipper_string, "Lon", &instance->gps_longitude, 1);
    }

    instance->last_index_write++;
    return true;
}

bool subghz_history_add_to_history_source(
    SubGhzHistory* instance,
    void* context,
    SubGhzRadioPreset* preset,
    uint8_t source) {
    furi_assert(instance);
    furi_mutex_acquire(instance->format_mutex, FuriWaitForever);
    const bool added =
        subghz_history_add_to_history_source_locked(instance, context, preset, source);
    furi_mutex_release(instance->format_mutex);
    return added;
}
