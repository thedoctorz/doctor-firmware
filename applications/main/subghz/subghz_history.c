#include "subghz_history.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/receiver.h>

#include <furi.h>
#include <string.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/string_stream.h>

#define SUBGHZ_HISTORY_MAX           255
#define SUBGHZ_HISTORY_CHUNK_SIZE    8
#define SUBGHZ_HISTORY_FREE_HEAP     12288
#define SUBGHZ_HISTORY_FREE_BLOCK    1032
#define SUBGHZ_HISTORY_TRIM_HEADROOM 2048
#define TAG                          "SubGhzHistory"
typedef struct {
    SubGhzRadioPreset preset;
    size_t references;
} SubGhzHistoryPreset;

static SubGhzRadioPreset* subghz_history_preset_copy(const SubGhzRadioPreset* preset) {
    SubGhzHistoryPreset* shared = malloc(sizeof(SubGhzHistoryPreset));
    shared->references = 1;
    shared->preset.frequency = preset->frequency;
    shared->preset.name = furi_string_alloc_set(preset->name);
    shared->preset.data_size = preset->data_size;
    shared->preset.data = NULL;
    if(preset->data && preset->data_size) {
        shared->preset.data = malloc(preset->data_size);
        memcpy(shared->preset.data, preset->data, preset->data_size);
    }
    return &shared->preset;
}

static void subghz_history_preset_release(SubGhzRadioPreset* preset) {
    SubGhzHistoryPreset* shared = (SubGhzHistoryPreset*)preset;
    furi_assert(shared->references);
    if(--shared->references) return;
    furi_string_free(preset->name);
    free(preset->data);
    free(shared);
}

typedef struct {
    uint64_t key;
    uint64_t key2;
    uint32_t cnt;
    uint32_t raw_cnt;
    uint32_t serial;
    uint32_t btn;
    uint32_t type;
    uint8_t present;
} SubGhzHistoryIdentity;

enum {
    HistoryHasKey = 1 << 0,
    HistoryHasKey2 = 1 << 1,
    HistoryHasCnt = 1 << 2,
    HistoryHasRawCnt = 1 << 3,
    HistoryHasSerial = 1 << 4,
    HistoryHasBtn = 1 << 5,
    HistoryHasType = 1 << 6,
};

typedef struct {
    FuriString* item_str;
    FlipperFormat* flipper_string;
    uint8_t* serialized;
    size_t serialized_size;
    uint8_t type;
    uint8_t source; // dual RX radio source marker: 0 = internal, 1 = external

    const SubGhzProtocol* protocol;
    SubGhzHistoryButtonIcon button_icon;
    SubGhzRadioPreset* preset;
    SubGhzHistoryIdentity identity;
    bool identity_valid;
    DateTime datetime;
} SubGhzHistoryItem;

static void subghz_history_item_free_format(SubGhzHistoryItem* item) {
    if(item->flipper_string) flipper_format_free(item->flipper_string);
    free(item->serialized);
}

static FlipperFormat* subghz_history_item_format(SubGhzHistoryItem* item) {
    if(!item->flipper_string && item->serialized) {
        item->flipper_string = flipper_format_string_alloc();
        stream_write(
            flipper_format_get_raw_stream(item->flipper_string),
            item->serialized,
            item->serialized_size);
        flipper_format_rewind(item->flipper_string);
        free(item->serialized);
        item->serialized = NULL;
        item->serialized_size = 0;
    }
    return item->flipper_string;
}

typedef struct {
    SubGhzHistoryItem*
        chunks[(SUBGHZ_HISTORY_MAX + SUBGHZ_HISTORY_CHUNK_SIZE - 1) / SUBGHZ_HISTORY_CHUNK_SIZE];
    size_t size;
} SubGhzHistoryItemArray[1];
typedef SubGhzHistoryItemArray SubGhzHistoryItemArray_t;

static void SubGhzHistoryItemArray_init(SubGhzHistoryItemArray_t data) {
    memset(data, 0, sizeof(SubGhzHistoryItemArray));
}
static size_t SubGhzHistoryItemArray_size(SubGhzHistoryItemArray_t data) {
    return data->size;
}
static SubGhzHistoryItem* SubGhzHistoryItemArray_get(SubGhzHistoryItemArray_t data, size_t index) {
    furi_check(index < data->size);
    return &data->chunks[index / SUBGHZ_HISTORY_CHUNK_SIZE][index % SUBGHZ_HISTORY_CHUNK_SIZE];
}
static SubGhzHistoryItem* SubGhzHistoryItemArray_push_raw(SubGhzHistoryItemArray_t data) {
    furi_check(data->size < SUBGHZ_HISTORY_MAX);
    size_t chunk = data->size / SUBGHZ_HISTORY_CHUNK_SIZE;
    if(!data->chunks[chunk])
        data->chunks[chunk] = malloc(sizeof(SubGhzHistoryItem) * SUBGHZ_HISTORY_CHUNK_SIZE);
    data->size++;
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(data, data->size - 1);
    memset(item, 0, sizeof(*item));
    return item;
}
static void SubGhzHistoryItemArray_clear(SubGhzHistoryItemArray_t data) {
    for(size_t i = 0; i < COUNT_OF(data->chunks); i++)
        free(data->chunks[i]);
    SubGhzHistoryItemArray_init(data);
}
static void SubGhzHistoryItemArray_reset(SubGhzHistoryItemArray_t data) {
    SubGhzHistoryItemArray_clear(data);
}
static void
    SubGhzHistoryItemArray_remove_v(SubGhzHistoryItemArray_t data, size_t first, size_t last) {
    furi_check(last == first + 1 && last <= data->size);
    for(size_t i = first; i + 1 < data->size; i++)
        *SubGhzHistoryItemArray_get(data, i) = *SubGhzHistoryItemArray_get(data, i + 1);
    memset(SubGhzHistoryItemArray_get(data, data->size - 1), 0, sizeof(SubGhzHistoryItem));
    data->size--;
    if(data->size % SUBGHZ_HISTORY_CHUNK_SIZE == 0) {
        size_t empty = data->size / SUBGHZ_HISTORY_CHUNK_SIZE;
        free(data->chunks[empty]);
        data->chunks[empty] = NULL;
    }
}

typedef struct {
    SubGhzHistoryItemArray_t data;
} SubGhzHistoryStruct;

struct SubGhzHistory {
    FuriMutex* format_mutex;
    uint16_t last_index_write;
    FuriString* tmp_string;
    SubGhzHistoryStruct* history;
    void* tx_reserve;
    size_t tx_reserved;
    size_t tx_wanted;

    // GPS geotag applied to newly added items
    bool gps_valid;
    float gps_latitude;
    float gps_longitude;
};

size_t subghz_history_tx_budget(const char* protocol) {
    if(!protocol) return 24576;
    if(!strcmp(protocol, "Toyota Lexus")) return 24576;
    if(!strcmp(protocol, "Mercedes V1") || !strcmp(protocol, "Datsun Lada")) return 16384;
    if(!strcmp(protocol, "Princeton") || !strcmp(protocol, "Holtek_HT12X")) return 2048;
    if(!strcmp(protocol, "Security+ 1.0") || !strcmp(protocol, "Security+ 2.0")) return 4096;
    if(!strcmp(protocol, "Nice FloR-S") || !strcmp(protocol, "RAW") || !strcmp(protocol, "KeeLoq"))
        return 16384;
    return 12288;
}

static bool subghz_history_reserve_locked(SubGhzHistory* instance, size_t wanted) {
    instance->tx_wanted = MAX(instance->tx_wanted, wanted);
    if(instance->tx_reserved >= wanted) return true;
    const size_t previous = instance->tx_reserved;
    free(instance->tx_reserve);
    instance->tx_reserve = NULL;
    instance->tx_reserved = 0;
    if(memmgr_heap_get_max_free_block() >= wanted + 8 &&
       memmgr_get_free_heap() >= wanted + SUBGHZ_HISTORY_FREE_HEAP + 2048) {
        instance->tx_reserve = malloc(wanted);
        instance->tx_reserved = wanted;
        return true;
    }
    if(previous && memmgr_heap_get_max_free_block() >= previous + 8) {
        instance->tx_reserve = malloc(previous);
        instance->tx_reserved = previous;
    }
    return false;
}

void subghz_history_tx_release(SubGhzHistory* instance) {
    if(!instance) return;
    furi_mutex_acquire(instance->format_mutex, FuriWaitForever);
    free(instance->tx_reserve);
    instance->tx_reserve = NULL;
    instance->tx_reserved = 0;
    furi_mutex_release(instance->format_mutex);
}

void subghz_history_tx_restore(SubGhzHistory* instance) {
    if(!instance || !instance->tx_wanted) return;
    furi_mutex_acquire(instance->format_mutex, FuriWaitForever);
    subghz_history_reserve_locked(instance, instance->tx_wanted);
    furi_mutex_release(instance->format_mutex);
}

SubGhzHistory* subghz_history_alloc(void) {
    SubGhzHistory* instance = malloc(sizeof(SubGhzHistory));
    instance->format_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->tmp_string = furi_string_alloc();
    instance->history = malloc(sizeof(SubGhzHistoryStruct));
    SubGhzHistoryItemArray_init(instance->history->data);

    instance->last_index_write = 0;
    instance->tx_reserve = NULL;
    instance->tx_reserved = instance->tx_wanted = 0;
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
    for(size_t item_index = 0; item_index < SubGhzHistoryItemArray_size(instance->history->data);
        item_index++) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, item_index);
        furi_string_free(item->item_str);
        subghz_history_preset_release(item->preset);
        subghz_history_item_free_format(item);
        item->type = 0;
    }
    SubGhzHistoryItemArray_clear(instance->history->data);
    free(instance->history);
    free(instance->tx_reserve);
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
    SubGhzHistoryPreset* shared = (SubGhzHistoryPreset*)item->preset;
    if(shared->references > 1) {
        SubGhzRadioPreset* copy = subghz_history_preset_copy(item->preset);
        subghz_history_preset_release(item->preset);
        item->preset = copy;
    }
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
    for(size_t item_index = 0; item_index < SubGhzHistoryItemArray_size(instance->history->data);
        item_index++) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, item_index);
        furi_string_free(item->item_str);
        subghz_history_preset_release(item->preset);
        subghz_history_item_free_format(item);
        item->type = 0;
    }
    SubGhzHistoryItemArray_reset(instance->history->data);
    free(instance->tx_reserve);
    instance->tx_reserve = NULL;
    instance->tx_reserved = instance->tx_wanted = 0;
    instance->last_index_write = 0;
}

void subghz_history_delete_item(SubGhzHistory* instance, uint16_t idx) {
    furi_assert(instance);
    furi_mutex_acquire(instance->format_mutex, FuriWaitForever);

    if(idx < SubGhzHistoryItemArray_size(instance->history->data)) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
        furi_string_free(item->item_str);
        subghz_history_preset_release(item->preset);
        subghz_history_item_free_format(item);
        item->type = 0;
        SubGhzHistoryItemArray_remove_v(instance->history->data, idx, idx + 1);
        instance->last_index_write--;
        size_t wanted = 0;
        for(size_t i = 0; i < instance->last_index_write; i++) {
            const SubGhzProtocol* protocol =
                SubGhzHistoryItemArray_get(instance->history->data, i)->protocol;
            if(protocol->flag & SubGhzProtocolFlag_Send)
                wanted = MAX(wanted, subghz_history_tx_budget(protocol->name));
        }
        if(wanted < instance->tx_wanted) {
            free(instance->tx_reserve);
            instance->tx_reserve = NULL;
            instance->tx_reserved = 0;
            instance->tx_wanted = wanted;
            if(wanted) subghz_history_reserve_locked(instance, wanted);
        }
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
    if(!item) return "";
    if(item->flipper_string) {
        flipper_format_rewind(item->flipper_string);
        if(!flipper_format_read_string(item->flipper_string, "Protocol", instance->tmp_string)) {
            furi_string_reset(instance->tmp_string);
        }
        return furi_string_get_cstr(instance->tmp_string);
    }
    return item->protocol ? item->protocol->name : "";
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
    if(subghz_history_item_format(item)) {
        item->identity_valid = false;
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
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, idx);
        if(item->serialized) {
            copied = stream_write(
                         flipper_format_get_raw_stream(copy),
                         item->serialized,
                         item->serialized_size) == item->serialized_size;
        } else if(item->flipper_string) {
            Stream* source = flipper_format_get_raw_stream(item->flipper_string);
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
    if(memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP ||
       memmgr_heap_get_max_free_block() < SUBGHZ_HISTORY_FREE_BLOCK) {
        if(output != NULL) furi_string_printf(output, "  RAM almost FULL");
        return true;
    }
    if(instance->last_index_write == SUBGHZ_HISTORY_MAX) {
        if(output != NULL) furi_string_printf(output, "   Memory is FULL");
        return true;
    }
    if(output != NULL) furi_string_printf(output, "#%u", instance->last_index_write);
    return false;
}

bool subghz_history_needs_memory_trim(SubGhzHistory* instance) {
    furi_assert(instance);
    return memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP + SUBGHZ_HISTORY_TRIM_HEADROOM ||
           instance->last_index_write >= SUBGHZ_HISTORY_MAX;
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
    static const char* const names[] = {"Key_2", "Key2", "Secplus_packet_1"};
    for(size_t field = 0; field < COUNT_OF(names); field++) {
        FuriString* text = furi_string_alloc();
        flipper_format_rewind(ff);
        bool valid = flipper_format_read_string(ff, names[field], text);
        const char* str = furi_string_get_cstr(text);
        uint64_t key = 0;
        unsigned digits = 0;
        for(size_t i = 0; valid && str[i]; i++) {
            if(str[i] == ' ' || str[i] == '\t') continue;
            unsigned nibble;
            if(str[i] >= '0' && str[i] <= '9')
                nibble = str[i] - '0';
            else if(str[i] >= 'A' && str[i] <= 'F')
                nibble = str[i] - 'A' + 10;
            else if(str[i] >= 'a' && str[i] <= 'f')
                nibble = str[i] - 'a' + 10;
            else {
                valid = false;
                break;
            }
            if(++digits > 16) {
                valid = false;
                break;
            }
            key = (key << 4) | nibble;
        }
        furi_string_free(text);
        if(valid && digits == 16) {
            *value = key;
            return true;
        }
    }
    return false;
}

static SubGhzHistoryIdentity subghz_history_identity_read(FlipperFormat* ff, bool keys_only) {
    SubGhzHistoryIdentity value = {0};
    if(subghz_block_generic_read_key_u64(ff, &value.key)) value.present |= HistoryHasKey;
    if(subghz_history_read_key2_u64(ff, &value.key2)) value.present |= HistoryHasKey2;
    if(keys_only) return value;
    if(subghz_history_read_uint32(ff, "Cnt", &value.cnt)) value.present |= HistoryHasCnt;
    if(subghz_history_read_uint32(ff, "RawCnt", &value.raw_cnt)) value.present |= HistoryHasRawCnt;
    if(!value.cnt && !value.raw_cnt) return value;
    if(subghz_history_read_uint32(ff, "Serial", &value.serial)) value.present |= HistoryHasSerial;
    if(subghz_history_read_uint32(ff, "Btn", &value.btn)) value.present |= HistoryHasBtn;
    if(subghz_history_read_uint32(ff, "Type", &value.type)) value.present |= HistoryHasType;
    return value;
}

static bool subghz_history_uses_exact_keys(const SubGhzProtocol* protocol) {
    return protocol->type == SubGhzProtocolTypeStatic ||
           strcmp(protocol->name, "Security+ 2.0") == 0;
}

static bool subghz_history_identity_keys_match(
    const SubGhzHistoryIdentity* stored,
    const SubGhzHistoryIdentity* candidate) {
    if(!(stored->present & candidate->present & HistoryHasKey)) return false;
    if(stored->key != candidate->key) return false;
    if((stored->present ^ candidate->present) & HistoryHasKey2) return false;
    return !(stored->present & HistoryHasKey2) || stored->key2 == candidate->key2;
}

static bool subghz_history_identity_match(
    const SubGhzHistoryIdentity* stored,
    const SubGhzHistoryIdentity* candidate) {
    const uint8_t counter = (candidate->present & HistoryHasCnt) ? HistoryHasCnt :
                                                                   HistoryHasRawCnt;
    if(stored->present & candidate->present & counter) {
        const uint32_t a = counter == HistoryHasCnt ? stored->cnt : stored->raw_cnt;
        const uint32_t b = counter == HistoryHasCnt ? candidate->cnt : candidate->raw_cnt;
        if(a != b) return false;
        if(!a) return subghz_history_identity_keys_match(stored, candidate);
        const uint8_t optional = HistoryHasSerial | HistoryHasBtn | HistoryHasType;
        if((stored->present ^ candidate->present) & optional) return false;
        return (!(stored->present & HistoryHasSerial) || stored->serial == candidate->serial) &&
               (!(stored->present & HistoryHasBtn) || stored->btn == candidate->btn) &&
               (!(stored->present & HistoryHasType) || stored->type == candidate->type);
    }
    return subghz_history_identity_keys_match(stored, candidate);
}

static bool subghz_history_is_duplicate(
    SubGhzHistory* instance,
    const SubGhzProtocol* protocol,
    const SubGhzHistoryIdentity* candidate,
    uint8_t source) {
    const bool keys_only = subghz_history_uses_exact_keys(protocol);
    for(size_t item_index = 0; item_index < SubGhzHistoryItemArray_size(instance->history->data);
        item_index++) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, item_index);
        if(item->source != source || item->protocol != protocol) continue;
        if(!item->identity_valid) {
            item->identity = subghz_history_identity_read(item->flipper_string, keys_only);
            item->identity_valid = true;
        }
        const bool duplicate = keys_only ?
                                   subghz_history_identity_keys_match(&item->identity, candidate) :
                                   subghz_history_identity_match(&item->identity, candidate);
        if(duplicate) return true;
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

        if(!strcmp(furi_string_get_cstr(protocol), "PSA")) {
            if(have_counter) {
                furi_string_printf(output, "PSA 80bit Cnt:%lX", counter);
            } else {
                furi_string_set(output, "PSA 80bit");
            }
        } else if(have_counter) {
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

static SubGhzRadioPreset*
    subghz_history_preset_acquire(SubGhzHistory* instance, const SubGhzRadioPreset* preset) {
    for(size_t item_index = 0; item_index < SubGhzHistoryItemArray_size(instance->history->data);
        item_index++) {
        SubGhzHistoryItem* item = SubGhzHistoryItemArray_get(instance->history->data, item_index);
        const SubGhzRadioPreset* stored = item->preset;
        if(stored->frequency == preset->frequency && stored->data_size == preset->data_size &&
           furi_string_equal(stored->name, preset->name) &&
           ((!stored->data && !preset->data) ||
            (stored->data && preset->data &&
             !memcmp(stored->data, preset->data, preset->data_size)))) {
            ((SubGhzHistoryPreset*)item->preset)->references++;
            return item->preset;
        }
    }
    return subghz_history_preset_copy(preset);
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

    if(memmgr_get_free_heap() < SUBGHZ_HISTORY_FREE_HEAP ||
       memmgr_heap_get_max_free_block() < SUBGHZ_HISTORY_FREE_BLOCK)
        return false;
    if(instance->last_index_write >= SUBGHZ_HISTORY_MAX) return false;

    SubGhzProtocolDecoderBase* decoder_base = context;
    furi_string_reset(instance->tmp_string);
    subghz_protocol_decoder_base_get_string(decoder_base, instance->tmp_string);

    FlipperFormat* flipper_string = flipper_format_string_alloc();
    if(subghz_protocol_decoder_base_serialize(decoder_base, flipper_string, preset) !=
       SubGhzProtocolStatusOk) {
        flipper_format_free(flipper_string);
        return false;
    }

    const SubGhzHistoryIdentity identity = subghz_history_identity_read(
        flipper_string, subghz_history_uses_exact_keys(decoder_base->protocol));
    if(subghz_history_is_duplicate(instance, decoder_base->protocol, &identity, source)) {
        flipper_format_free(flipper_string);
        return false;
    }

    if(decoder_base->protocol->flag & SubGhzProtocolFlag_Send)
        subghz_history_reserve_locked(
            instance, subghz_history_tx_budget(decoder_base->protocol->name));
    SubGhzRadioPreset* snapshot = subghz_history_preset_acquire(instance, preset);
    SubGhzHistoryItem* item = SubGhzHistoryItemArray_push_raw(instance->history->data);
    item->preset = snapshot;
    item->type = decoder_base->protocol->type;
    item->protocol = decoder_base->protocol;
    item->source = source;
    item->identity = identity;
    item->identity_valid = true;
    furi_hal_rtc_get_datetime(&item->datetime);

    item->item_str = furi_string_alloc();
    item->flipper_string = flipper_string;
    item->serialized = NULL;
    item->serialized_size = 0;
    subghz_history_format_item_string(item->flipper_string, item->item_str, item->type);
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

    Stream* capture = flipper_format_get_raw_stream(item->flipper_string);
    item->serialized_size = stream_size(capture);
    item->serialized = malloc(item->serialized_size);
    stream_rewind(capture);
    furi_check(
        stream_read(capture, item->serialized, item->serialized_size) == item->serialized_size);
    flipper_format_free(item->flipper_string);
    item->flipper_string = NULL;
    furi_string_reserve(item->item_str, 0);

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
