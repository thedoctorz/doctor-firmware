#include "../desktop_settings_app.h"
#include "desktop_settings_scene.h"

typedef enum {
    DesktopSettingsMainMenuItemMoveUp,
    DesktopSettingsMainMenuItemMoveDown,
    DesktopSettingsMainMenuItemRemove,
} DesktopSettingsMainMenuItemEntry;

static void desktop_settings_scene_main_menu_item_submenu_callback(void* context, uint32_t index) {
    DesktopSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void desktop_settings_scene_main_menu_item_on_enter(void* context) {
    DesktopSettingsApp* app = context;
    Submenu* submenu = app->submenu;
    const size_t index =
        scene_manager_get_scene_state(app->scene_manager, DesktopSettingsAppSceneMainMenuItem);

    submenu_reset(submenu);

    if(index > 0) {
        submenu_add_item(
            submenu,
            "Move Up",
            DesktopSettingsMainMenuItemMoveUp,
            desktop_settings_scene_main_menu_item_submenu_callback,
            app);
    }
    if(index + 1 < main_menu_apps_get_count(app->main_menu_apps)) {
        submenu_add_item(
            submenu,
            "Move Down",
            DesktopSettingsMainMenuItemMoveDown,
            desktop_settings_scene_main_menu_item_submenu_callback,
            app);
    }
    submenu_add_item(
        submenu,
        "Remove",
        DesktopSettingsMainMenuItemRemove,
        desktop_settings_scene_main_menu_item_submenu_callback,
        app);

    submenu_set_header(submenu, main_menu_apps_get_name(app->main_menu_apps, index));

    view_dispatcher_switch_to_view(app->view_dispatcher, DesktopSettingsAppViewMenu);
}

bool desktop_settings_scene_main_menu_item_on_event(void* context, SceneManagerEvent event) {
    DesktopSettingsApp* app = context;
    size_t index =
        scene_manager_get_scene_state(app->scene_manager, DesktopSettingsAppSceneMainMenuItem);

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case DesktopSettingsMainMenuItemMoveUp:
        if(main_menu_apps_move(app->main_menu_apps, index, true)) index--;
        break;

    case DesktopSettingsMainMenuItemMoveDown:
        if(main_menu_apps_move(app->main_menu_apps, index, false)) index++;
        break;

    case DesktopSettingsMainMenuItemRemove: {
        main_menu_apps_remove(app->main_menu_apps, index);
        const size_t count = main_menu_apps_get_count(app->main_menu_apps);
        if(index >= count && count) index = count - 1;
    } break;

    default:
        return false;
    }

    main_menu_apps_save(app->main_menu_apps);

    scene_manager_set_scene_state(app->scene_manager, DesktopSettingsAppSceneMainMenu, index);
    scene_manager_previous_scene(app->scene_manager);

    return true;
}

void desktop_settings_scene_main_menu_item_on_exit(void* context) {
    DesktopSettingsApp* app = context;
    submenu_reset(app->submenu);
}
