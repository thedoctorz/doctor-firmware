/**
 * @file apps_menu.c
 * Browses the apps on the SD card as menus instead of as a file tree.
 */

#include "classic_list.h"

#include <apps_menu_icons.h>
#include <flipper_application/flipper_application.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/modules/loading.h>
#include <gui/view_dispatcher.h>
#include <loader/loader.h>
#include <storage/storage.h>
#include <toolbox/path.h>

#define TAG "AppsMenu"

#define APPS_MENU_ROOT      EXT_PATH("apps")
#define APPS_MENU_SELF_PATH EXT_PATH("apps/Tools/apps_menu.fap")
#define APPS_MENU_JS_RUNNER "JS Runner"

#define APPS_MENU_ICON_SIZE 32

#define APPS_MENU_FAP_EXTENSION ".fap"
#define APPS_MENU_JS_EXTENSION  ".js"

static bool apps_menu_folder_hidden(const char* name) {
    if(!name || !name[0]) return true;

    const char* base = strrchr(name, '/');
    base = (base && base[1]) ? (base + 1) : name;

    const char* hidden = "assets";
    size_t i = 0;
    while(base[i] && hidden[i]) {
        char c = base[i];
        if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if(c != hidden[i]) return false;
        i++;
    }
    return base[i] == '\0' && hidden[i] == '\0';
}

typedef enum {
    AppsMenuViewList,
    AppsMenuViewLoading,
} AppsMenuViewId;

typedef enum {
    AppsMenuCustomEventScan,
} AppsMenuCustomEvent;

typedef struct {
    char* label;
    char* target;
    bool launchable;
    bool script;
    uint8_t* bitmap;
    const Icon* icon;
} AppsMenuEntry;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    ClassicList* list;
    Loading* loading;
    Storage* storage;
    Loader* loader;

    FuriString* self_path;
    FuriString* category;
    FuriString* restore_target;

    AppsMenuEntry* entries;
    size_t entry_count;
    size_t entry_capacity;

    bool launching;
} AppsMenu;

static void apps_menu_entries_clear(AppsMenu* app) {
    for(size_t i = 0; i < app->entry_count; i++) {
        free(app->entries[i].label);
        free(app->entries[i].target);
        free(app->entries[i].bitmap);
    }
    app->entry_count = 0;
}

static void apps_menu_entries_insert(
    AppsMenu* app,
    const char* label,
    const char* target,
    bool launchable,
    bool script,
    uint8_t* bitmap,
    const Icon* icon) {
    if(app->entry_count == app->entry_capacity) {
        const size_t capacity = app->entry_capacity ? app->entry_capacity * 2 : 16;
        app->entries = realloc(app->entries, capacity * sizeof(AppsMenuEntry)); //-V701
        app->entry_capacity = capacity;
    }

    size_t position = app->entry_count;
    while(position > 0) {
        const AppsMenuEntry* previous = &app->entries[position - 1];
        if(previous->launchable != launchable) {
            if(!previous->launchable) break;
        } else if(strcasecmp(previous->label, label) <= 0) {
            break;
        }
        app->entries[position] = app->entries[position - 1];
        position--;
    }

    app->entries[position] = (AppsMenuEntry){
        .label = strdup(label),
        .target = strdup(target),
        .launchable = launchable,
        .script = script,
        .bitmap = bitmap,
        .icon = icon,
    };
    app->entry_count++;
}

static void apps_menu_add_application(AppsMenu* app, const char* path, const char* filename) {
    if(furi_string_cmp_str(app->self_path, path) == 0) return;

    FuriString* label = furi_string_alloc();
    path_extract_filename_no_ext(filename, label);

    uint8_t* bitmap = malloc(APPS_MENU_ICON_SIZE);
    /* An app can have no icon at all, in which case load_name_and_icon leaves the
     * buffer alone - zero it first so that reads as "no icon" rather than junk. */
    memset(bitmap, 0, APPS_MENU_ICON_SIZE);

    FuriString* full_path = furi_string_alloc_set_str(path);
    if(flipper_application_load_name_and_icon(full_path, app->storage, &bitmap, label)) {
        bool has_icon = false;
        for(size_t i = 0; i < APPS_MENU_ICON_SIZE; i++) {
            if(bitmap[i]) {
                has_icon = true;
                break;
            }
        }
        if(!has_icon) {
            free(bitmap);
            bitmap = NULL;
        }
    } else {
        FURI_LOG_W(TAG, "No manifest in %s", path);
        free(bitmap);
        bitmap = NULL;
    }
    furi_string_free(full_path);

    apps_menu_entries_insert(
        app, furi_string_get_cstr(label), path, true, false, bitmap, &I_Unknown_14x14);
    furi_string_free(label);
}

static void apps_menu_add_script(AppsMenu* app, const char* path, const char* filename) {
    FuriString* label = furi_string_alloc();
    path_extract_filename_no_ext(filename, label);
    apps_menu_entries_insert(
        app, furi_string_get_cstr(label), path, true, true, NULL, &I_Script_14x14);
    furi_string_free(label);
}

static void apps_menu_scan_directory(AppsMenu* app, const char* directory, const char* relative) {
    File* dir = storage_file_alloc(app->storage);

    if(!storage_dir_open(dir, directory)) {
        FURI_LOG_E(TAG, "Cannot open %s", directory);
        storage_file_free(dir);
        return;
    }

    FileInfo info;
    char name[256];
    FuriString* path = furi_string_alloc();
    FuriString* nested = furi_string_alloc();

    while(storage_dir_read(dir, &info, name, sizeof(name))) {
        if(name[0] == '.') continue;

        if(file_info_is_dir(&info)) {
            if(!apps_menu_folder_hidden(name)) {
                if(relative && relative[0]) {
                    furi_string_printf(nested, "%s/%s", relative, name);
                } else {
                    furi_string_set_str(nested, name);
                }
                apps_menu_entries_insert(
                    app, name, furi_string_get_cstr(nested), false, false, NULL, &I_Folder_14x14);
            }
            continue;
        }

        furi_string_printf(path, "%s/%s", directory, name);

        if(furi_string_end_with_str(path, APPS_MENU_FAP_EXTENSION)) {
            apps_menu_add_application(app, furi_string_get_cstr(path), name);
        } else if(furi_string_end_with_str(path, APPS_MENU_JS_EXTENSION)) {
            apps_menu_add_script(app, furi_string_get_cstr(path), name);
        }
    }

    furi_string_free(nested);
    furi_string_free(path);
    storage_dir_close(dir);
    storage_file_free(dir);
}

static void apps_menu_scan(AppsMenu* app) {
    apps_menu_entries_clear(app);

    if(furi_string_empty(app->category)) {
        apps_menu_scan_directory(app, APPS_MENU_ROOT, "");
    } else {
        FuriString* directory =
            furi_string_alloc_printf(APPS_MENU_ROOT "/%s", furi_string_get_cstr(app->category));
        apps_menu_scan_directory(
            app, furi_string_get_cstr(directory), furi_string_get_cstr(app->category));
        furi_string_free(directory);
    }
}

static void apps_menu_item_callback(void* context, uint32_t index);

static void apps_menu_populate_list(AppsMenu* app) {
    classic_list_reset(app->list);

    uint32_t selected = 0;

    for(size_t i = 0; i < app->entry_count; i++) {
        const AppsMenuEntry* entry = &app->entries[i];
        if(!entry->launchable && apps_menu_folder_hidden(entry->target)) continue;

        if(entry->bitmap) {
            classic_list_add_item_bitmap(
                app->list, entry->label, entry->bitmap, i, apps_menu_item_callback, app);
        } else {
            classic_list_add_item(
                app->list, entry->label, entry->icon, i, apps_menu_item_callback, app);
        }

        if(!furi_string_empty(app->restore_target) &&
           furi_string_cmp_str(app->restore_target, entry->target) == 0) {
            selected = i;
        }
    }

    classic_list_set_selected_item(app->list, selected);
    furi_string_reset(app->restore_target);
}

static void apps_menu_request_scan(AppsMenu* app) {
    view_dispatcher_switch_to_view(app->view_dispatcher, AppsMenuViewLoading);
    view_dispatcher_send_custom_event(app->view_dispatcher, AppsMenuCustomEventScan);
}

static void apps_menu_launch(AppsMenu* app, const AppsMenuEntry* entry) {
    if(app->launching) return;
    app->launching = true;

    const char* path = entry->target;
    FURI_LOG_I(TAG, "Launching %s", path);

    if(entry->script) {
        loader_enqueue_launch(
            app->loader, APPS_MENU_JS_RUNNER, path, LoaderDeferredLaunchFlagNone);
    } else {
        loader_enqueue_launch(app->loader, path, NULL, LoaderDeferredLaunchFlagNone);
    }

    loader_enqueue_launch(
        app->loader, furi_string_get_cstr(app->self_path), path, LoaderDeferredLaunchFlagNone);

    view_dispatcher_stop(app->view_dispatcher);
}

static void apps_menu_item_callback(void* context, uint32_t index) {
    AppsMenu* app = context;
    if(index >= app->entry_count) return;

    const AppsMenuEntry* entry = &app->entries[index];

    if(entry->launchable) {
        apps_menu_launch(app, entry);
    } else {
        furi_string_set_str(app->category, entry->target);
        furi_string_reset(app->restore_target);
        apps_menu_request_scan(app);
    }
}

static bool apps_menu_custom_event_callback(void* context, uint32_t event) {
    AppsMenu* app = context;

    if(event == AppsMenuCustomEventScan) {
        apps_menu_scan(app);
        apps_menu_populate_list(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, AppsMenuViewList);
        return true;
    }

    return false;
}

static bool apps_menu_back_event_callback(void* context) {
    AppsMenu* app = context;

    if(furi_string_empty(app->category)) return false;

    furi_string_set(app->restore_target, app->category);
    const size_t slash = furi_string_search_rchar(app->category, '/');
    if(slash == FURI_STRING_FAILURE) {
        furi_string_reset(app->category);
    } else {
        furi_string_left(app->category, slash);
    }
    apps_menu_request_scan(app);

    return true;
}

static void apps_menu_resolve_self_path(AppsMenu* app) {
    if(!loader_get_application_launch_path(app->loader, app->self_path) ||
       furi_string_empty(app->self_path)) {
        furi_string_set_str(app->self_path, APPS_MENU_SELF_PATH);
    }
}

/** Reopens the category an app was launched from, with that app selected */
static void apps_menu_restore(AppsMenu* app, const char* launched_path) {
    furi_string_set_str(app->restore_target, launched_path);

    FuriString* directory = furi_string_alloc();
    path_extract_dirname(launched_path, directory);

    const char* root = APPS_MENU_ROOT "/";
    if(furi_string_start_with_str(directory, root)) {
        furi_string_set_str(app->category, furi_string_get_cstr(directory) + strlen(root));
    }

    furi_string_free(directory);
}

static AppsMenu* apps_menu_alloc(void) {
    AppsMenu* app = malloc(sizeof(AppsMenu));

    app->storage = furi_record_open(RECORD_STORAGE);
    app->loader = furi_record_open(RECORD_LOADER);
    app->gui = furi_record_open(RECORD_GUI);

    app->self_path = furi_string_alloc();
    app->category = furi_string_alloc();
    app->restore_target = furi_string_alloc();

    app->entries = NULL;
    app->entry_count = 0;
    app->entry_capacity = 0;
    app->launching = false;

    app->list = classic_list_alloc();
    app->loading = loading_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, apps_menu_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, apps_menu_back_event_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, AppsMenuViewList, classic_list_get_view(app->list));
    view_dispatcher_add_view(
        app->view_dispatcher, AppsMenuViewLoading, loading_get_view(app->loading));

    return app;
}

static void apps_menu_free(AppsMenu* app) {
    view_dispatcher_remove_view(app->view_dispatcher, AppsMenuViewList);
    view_dispatcher_remove_view(app->view_dispatcher, AppsMenuViewLoading);
    view_dispatcher_free(app->view_dispatcher);

    loading_free(app->loading);
    classic_list_free(app->list);

    apps_menu_entries_clear(app);
    free(app->entries);

    furi_string_free(app->restore_target);
    furi_string_free(app->category);
    furi_string_free(app->self_path);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_LOADER);
    furi_record_close(RECORD_STORAGE);

    free(app);
}

int32_t apps_menu_app(char* p) {
    AppsMenu* app = apps_menu_alloc();

    apps_menu_resolve_self_path(app);

    if(p && *p) {
        apps_menu_restore(app, p);
    }

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    apps_menu_request_scan(app);
    view_dispatcher_run(app->view_dispatcher);

    apps_menu_free(app);

    return 0;
}
