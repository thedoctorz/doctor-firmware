#include "main_menu_apps.h"

#include <applications.h>
#include <gui/icon_i.h>
#include <flipper_application/flipper_application.h>
#include <flipper_application/application_manifest.h>
#include <storage/storage.h>
#include <toolbox/path.h>
#include <toolbox/stream/buffered_file_stream.h>
#include <toolbox/stream/stream.h>

#include <string.h>

#define TAG "MainMenuApps"

#define MAIN_MENU_APPS_ICON_SIZE  10
#define MAIN_MENU_APPS_PARENT_DIR "/ext/apps_data"

typedef struct {
    Icon icon;
    const uint8_t* frame;
    uint8_t data[FAP_MANIFEST_MAX_ICON_SIZE];
} MainMenuAppIcon;

typedef struct {
    FuriString* name;
    FuriString* path;
    const Icon* icon;
    MainMenuAppIcon* own_icon;
    bool available;
} MainMenuAppItem;

struct MainMenuApps {
    MainMenuAppItem* items;
    size_t count;
};

static MainMenuAppIcon* main_menu_app_icon_alloc(const uint8_t* manifest_icon) {
    MainMenuAppIcon* instance = malloc(sizeof(MainMenuAppIcon));

    memcpy(instance->data, manifest_icon, FAP_MANIFEST_MAX_ICON_SIZE);
    instance->frame = instance->data;

    const Icon icon = {
        .width = MAIN_MENU_APPS_ICON_SIZE,
        .height = MAIN_MENU_APPS_ICON_SIZE,
        .frame_count = 1,
        .frame_rate = 1,
        .frames = (const uint8_t* const*)&instance->frame,
    };
    memcpy(&instance->icon, &icon, sizeof(Icon));

    return instance;
}

static bool main_menu_app_icon_is_blank(const uint8_t* manifest_icon) {
    for(size_t i = 0; i < FAP_MANIFEST_MAX_ICON_SIZE; i++) {
        if(manifest_icon[i]) return false;
    }

    return true;
}

static const FlipperExternalApplication* main_menu_apps_find_builtin(const char* path) {
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(strcmp(FLIPPER_EXTERNAL_APPS[i].path, path) == 0) {
            return &FLIPPER_EXTERNAL_APPS[i];
        }
    }

    return NULL;
}

static bool main_menu_apps_contains(const MainMenuApps* instance, const char* path) {
    return main_menu_apps_find(instance, path) < instance->count;
}

static MainMenuAppItem* main_menu_apps_push(MainMenuApps* instance) {
    instance->items = realloc(instance->items, (instance->count + 1) * sizeof(MainMenuAppItem));
    MainMenuAppItem* item = &instance->items[instance->count++];
    item->name = furi_string_alloc();
    item->path = furi_string_alloc();
    item->icon = NULL;
    item->own_icon = NULL;
    item->available = false;
    return item;
}

static void main_menu_apps_item_clear(MainMenuAppItem* item) {
    furi_string_free(item->name);
    furi_string_free(item->path);
    free(item->own_icon);
}

static void main_menu_apps_resolve(MainMenuAppItem* item, Storage* storage) {
    const char* path = furi_string_get_cstr(item->path);

    const FlipperExternalApplication* builtin = main_menu_apps_find_builtin(path);
    if(builtin) {
        furi_string_set_str(item->name, builtin->name);
        item->icon = builtin->icon;
        item->available = true;
        return;
    }

    uint8_t icon_data[FAP_MANIFEST_MAX_ICON_SIZE] = {0};
    uint8_t* icon_ptr = icon_data;

    if(flipper_application_load_name_and_icon(item->path, storage, &icon_ptr, item->name)) {
        if(!main_menu_app_icon_is_blank(icon_data)) {
            item->own_icon = main_menu_app_icon_alloc(icon_data);
            item->icon = &item->own_icon->icon;
        }
        item->available = true;
        return;
    }

    FURI_LOG_W(TAG, "Cannot read %s", path);
    path_extract_filename_no_ext(path, item->name);
}

static void main_menu_apps_load_defaults(MainMenuApps* instance) {
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT && i < MAIN_MENU_APPS_MAX; i++) {
        MainMenuAppItem* item = main_menu_apps_push(instance);
        furi_string_set_str(item->name, FLIPPER_EXTERNAL_APPS[i].name);
        furi_string_set_str(item->path, FLIPPER_EXTERNAL_APPS[i].path);
        item->icon = FLIPPER_EXTERNAL_APPS[i].icon;
        item->available = true;
    }
}

static bool main_menu_apps_load_file(MainMenuApps* instance, Storage* storage) {
    Stream* stream = buffered_file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    bool loaded = false;

    if(buffered_file_stream_open(stream, MAIN_MENU_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        loaded = true;

        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(furi_string_empty(line) || furi_string_get_char(line, 0) == '#') continue;

            if(instance->count >= MAIN_MENU_APPS_MAX) {
                FURI_LOG_W(TAG, "More than %u apps listed, ignoring the rest", MAIN_MENU_APPS_MAX);
                break;
            }

            if(main_menu_apps_contains(instance, furi_string_get_cstr(line))) continue;

            MainMenuAppItem* item = main_menu_apps_push(instance);
            furi_string_set(item->path, line);
            main_menu_apps_resolve(item, storage);
        }
    }

    furi_string_free(line);
    buffered_file_stream_close(stream);
    stream_free(stream);

    return loaded;
}

MainMenuApps* main_menu_apps_alloc(void) {
    MainMenuApps* instance = malloc(sizeof(MainMenuApps));
    instance->items = NULL;
    instance->count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!main_menu_apps_load_file(instance, storage)) {
        main_menu_apps_load_defaults(instance);
    }
    furi_record_close(RECORD_STORAGE);

    return instance;
}

void main_menu_apps_free(MainMenuApps* instance) {
    furi_check(instance);

    for(size_t i = 0; i < instance->count; i++) {
        main_menu_apps_item_clear(&instance->items[i]);
    }

    free(instance->items);
    free(instance);
}

size_t main_menu_apps_get_count(const MainMenuApps* instance) {
    furi_check(instance);
    return instance->count;
}

const char* main_menu_apps_get_name(const MainMenuApps* instance, size_t index) {
    furi_check(instance);
    furi_check(index < instance->count);
    return furi_string_get_cstr(instance->items[index].name);
}

const char* main_menu_apps_get_path(const MainMenuApps* instance, size_t index) {
    furi_check(instance);
    furi_check(index < instance->count);
    return furi_string_get_cstr(instance->items[index].path);
}

const Icon* main_menu_apps_get_icon(const MainMenuApps* instance, size_t index) {
    furi_check(instance);
    furi_check(index < instance->count);
    return instance->items[index].icon;
}

bool main_menu_apps_is_available(const MainMenuApps* instance, size_t index) {
    furi_check(instance);
    furi_check(index < instance->count);
    return instance->items[index].available;
}

size_t main_menu_apps_find(const MainMenuApps* instance, const char* path) {
    furi_check(instance);
    furi_check(path);

    size_t index = 0;
    while(index < instance->count && !furi_string_equal_str(instance->items[index].path, path)) {
        index++;
    }

    return index;
}

bool main_menu_apps_equal(const MainMenuApps* instance, const MainMenuApps* other) {
    furi_check(instance);
    furi_check(other);

    if(instance->count != other->count) return false;

    for(size_t i = 0; i < instance->count; i++) {
        if(instance->items[i].available != other->items[i].available) return false;
        if(!furi_string_equal(instance->items[i].path, other->items[i].path)) return false;
    }

    return true;
}

bool main_menu_apps_add(MainMenuApps* instance, const char* path) {
    furi_check(instance);
    furi_check(path);

    if(instance->count >= MAIN_MENU_APPS_MAX) return false;
    if(main_menu_apps_contains(instance, path)) return false;

    MainMenuAppItem* item = main_menu_apps_push(instance);
    furi_string_set_str(item->path, path);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    main_menu_apps_resolve(item, storage);
    furi_record_close(RECORD_STORAGE);

    if(!item->available) {
        main_menu_apps_remove(instance, instance->count - 1);
        return false;
    }

    return true;
}

void main_menu_apps_remove(MainMenuApps* instance, size_t index) {
    furi_check(instance);
    furi_check(index < instance->count);

    main_menu_apps_item_clear(&instance->items[index]);
    memmove(
        &instance->items[index],
        &instance->items[index + 1],
        (instance->count - index - 1) * sizeof(MainMenuAppItem));
    instance->count--;
}

bool main_menu_apps_move(MainMenuApps* instance, size_t index, bool up) {
    furi_check(instance);
    furi_check(index < instance->count);

    if(up ? (index == 0) : (index + 1 >= instance->count)) return false;

    const size_t other = up ? index - 1 : index + 1;
    const MainMenuAppItem item = instance->items[index];
    instance->items[index] = instance->items[other];
    instance->items[other] = item;

    return true;
}

void main_menu_apps_reset(MainMenuApps* instance) {
    furi_check(instance);

    for(size_t i = 0; i < instance->count; i++) {
        main_menu_apps_item_clear(&instance->items[i]);
    }
    free(instance->items);
    instance->items = NULL;
    instance->count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_remove(storage, MAIN_MENU_APPS_PATH);
    furi_record_close(RECORD_STORAGE);

    main_menu_apps_load_defaults(instance);
}

bool main_menu_apps_save(const MainMenuApps* instance) {
    furi_check(instance);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, MAIN_MENU_APPS_PARENT_DIR);
    storage_simply_mkdir(storage, MAIN_MENU_APPS_DIR);

    Stream* stream = buffered_file_stream_alloc(storage);
    bool success = false;

    if(buffered_file_stream_open(stream, MAIN_MENU_APPS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        success = true;
        for(size_t i = 0; i < instance->count && success; i++) {
            success = stream_write_format(
                          stream, "%s\n", furi_string_get_cstr(instance->items[i].path)) > 0;
        }
    }

    success = buffered_file_stream_close(stream) && success;
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    if(!success) FURI_LOG_E(TAG, "Failed to save " MAIN_MENU_APPS_PATH);

    return success;
}
