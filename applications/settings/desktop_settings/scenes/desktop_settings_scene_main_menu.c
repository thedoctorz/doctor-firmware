#include "../desktop_settings_app.h"
#include "desktop_settings_scene.h"
#include "desktop_settings_scene_i.h"

#include <dialogs/dialogs.h>
#include <flipper_application/flipper_application.h>
#include <storage/storage.h>

static void desktop_settings_scene_main_menu_submenu_callback(void* context, uint32_t index) {
    DesktopSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static bool desktop_settings_scene_main_menu_item_callback(
    FuriString* file_path,
    void* context,
    uint8_t** icon_ptr,
    FuriString* item_name) {
    UNUSED(context);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool success = flipper_application_load_name_and_icon(file_path, storage, icon_ptr, item_name);
    furi_record_close(RECORD_STORAGE);
    return success;
}

static void desktop_settings_scene_main_menu_alert(DesktopSettingsApp* app, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

static void desktop_settings_scene_main_menu_show(DesktopSettingsApp* app) {
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);

    FuriString* label = furi_string_alloc();
    const size_t count = main_menu_apps_get_count(app->main_menu_apps);

    for(size_t i = 0; i < count; i++) {
        const char* name = main_menu_apps_get_name(app->main_menu_apps, i);
        if(main_menu_apps_is_available(app->main_menu_apps, i)) {
            furi_string_set_str(label, name);
        } else {
            furi_string_printf(label, "%s (missing)", name);
        }
        submenu_add_item(
            submenu,
            furi_string_get_cstr(label),
            i,
            desktop_settings_scene_main_menu_submenu_callback,
            app);
    }

    furi_string_free(label);

    submenu_add_item(
        submenu,
        "Add App",
        SCENE_STATE_MAIN_MENU_ADD,
        desktop_settings_scene_main_menu_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Reset to Default",
        SCENE_STATE_MAIN_MENU_RESET,
        desktop_settings_scene_main_menu_submenu_callback,
        app);

    submenu_set_header(submenu, "Main Menu Apps");
    submenu_set_selected_item(
        submenu,
        scene_manager_get_scene_state(app->scene_manager, DesktopSettingsAppSceneMainMenu));

    view_dispatcher_switch_to_view(app->view_dispatcher, DesktopSettingsAppViewMenu);
}

void desktop_settings_scene_main_menu_on_enter(void* context) {
    DesktopSettingsApp* app = context;

    if(!app->main_menu_apps) {
        app->main_menu_apps = main_menu_apps_alloc();
    }

    desktop_settings_scene_main_menu_show(app);
}

static void desktop_settings_scene_main_menu_add(DesktopSettingsApp* app) {
    const DialogsFileBrowserOptions browser_options = {
        .extension = ".fap",
        .icon = &I_unknown_10px,
        .skip_assets = true,
        .hide_ext = true,
        .item_loader_callback = desktop_settings_scene_main_menu_item_callback,
        .item_loader_context = app,
        .base_path = EXT_PATH("apps"),
    };

    FuriString* path = furi_string_alloc_set_str(EXT_PATH("apps"));

    if(dialog_file_browser_show(app->dialogs, path, path, &browser_options)) {
        const char* path_str = furi_string_get_cstr(path);
        const size_t count = main_menu_apps_get_count(app->main_menu_apps);
        const size_t existing = main_menu_apps_find(app->main_menu_apps, path_str);

        if(existing < count) {
            scene_manager_set_scene_state(
                app->scene_manager, DesktopSettingsAppSceneMainMenu, existing);
        } else if(main_menu_apps_add(app->main_menu_apps, path_str)) {
            main_menu_apps_save(app->main_menu_apps);
            scene_manager_set_scene_state(
                app->scene_manager, DesktopSettingsAppSceneMainMenu, count);
        } else if(count >= MAIN_MENU_APPS_MAX) {
            desktop_settings_scene_main_menu_alert(app, "The main menu is full");
        } else {
            desktop_settings_scene_main_menu_alert(app, "Cannot read this app");
        }
    }

    furi_string_free(path);
}

static void desktop_settings_scene_main_menu_reset(DesktopSettingsApp* app) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Reset Main Menu Apps?", 64, 0, AlignCenter, AlignTop);
    dialog_message_set_text(
        message,
        "The menu goes back to the\napps this firmware ships with",
        64,
        32,
        AlignCenter,
        AlignCenter);
    dialog_message_set_buttons(message, "Cancel", NULL, "Reset");

    if(dialog_message_show(app->dialogs, message) == DialogMessageButtonRight) {
        main_menu_apps_reset(app->main_menu_apps);
        scene_manager_set_scene_state(app->scene_manager, DesktopSettingsAppSceneMainMenu, 0);
    }

    dialog_message_free(message);
}

bool desktop_settings_scene_main_menu_on_event(void* context, SceneManagerEvent event) {
    DesktopSettingsApp* app = context;

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == SCENE_STATE_MAIN_MENU_ADD) {
        desktop_settings_scene_main_menu_add(app);
    } else if(event.event == SCENE_STATE_MAIN_MENU_RESET) {
        desktop_settings_scene_main_menu_reset(app);
    } else {
        scene_manager_set_scene_state(
            app->scene_manager, DesktopSettingsAppSceneMainMenuItem, event.event);
        scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneMainMenuItem);
        return true;
    }

    desktop_settings_scene_main_menu_show(app);
    return true;
}

void desktop_settings_scene_main_menu_on_exit(void* context) {
    DesktopSettingsApp* app = context;
    submenu_reset(app->submenu);
}
