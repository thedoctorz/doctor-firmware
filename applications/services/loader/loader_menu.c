#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/menu.h>
#include <gui/modules/submenu.h>
#include <assets_icons.h>
#include <applications.h>
#include <archive/helpers/archive_favorites.h>
#include <string.h>

#include "loader.h"
#include "loader_menu.h"
#include "main_menu_apps.h"

#define TAG "LoaderMenu"

struct LoaderMenu {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;
    LoaderMenuStartView start_view;
    Menu* primary_menu;
    MainMenuApps* apps;
    ViewDispatcher* view_dispatcher;
    Gui* gui;
    bool statusbar_hidden;
};

static LoaderMenu* loader_menu_active = NULL;

static void loader_menu_statusbar_set_hidden(LoaderMenu* loader_menu, bool hidden) {
    if(!loader_menu->gui || loader_menu->statusbar_hidden == hidden) return;
    gui_set_hide_statusbar(loader_menu->gui, hidden);
    loader_menu->statusbar_hidden = hidden;
}

typedef enum {
    LoaderMenuViewPrimary,
    LoaderMenuViewSettings,
} LoaderMenuView;

static void loader_menu_start(const char* name) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_with_gui_error(loader, name, NULL);
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_apps_callback(void* context, uint32_t index) {
    UNUSED(context);
    const char* name = FLIPPER_APPS[index].name;
    loader_menu_start(name);
}

static void loader_menu_external_apps_callback(void* context, uint32_t index) {
    LoaderMenu* loader_menu = context;
    loader_menu_start(main_menu_apps_get_path(loader_menu->apps, index));
}

static void loader_menu_applications_callback(void* context, uint32_t index) {
    UNUSED(index);
    UNUSED(context);
    loader_menu_start(LOADER_APPLICATIONS_NAME);
}

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    if(input_type == InputTypeShort) {
        loader_menu_start((const char*)index);
    } else if(input_type == InputTypeLong) {
        archive_favorites_handle_setting_pin_unpin((const char*)index, NULL);
    }
}

static void loader_menu_switch_to_settings(void* context, uint32_t index) {
    UNUSED(index);
    LoaderMenu* loader_menu = context;
    loader_menu_statusbar_set_hidden(loader_menu, true);
    view_dispatcher_switch_to_view(loader_menu->view_dispatcher, LoaderMenuViewSettings);
}

static uint32_t loader_menu_switch_to_primary(void* context) {
    UNUSED(context);
    if(loader_menu_active) {
        loader_menu_statusbar_set_hidden(loader_menu_active, false);
    }
    return LoaderMenuViewPrimary;
}

static uint32_t loader_menu_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static void loader_menu_build_menu(LoaderMenu* loader_menu) {
    Menu* menu = loader_menu->primary_menu;
    size_t position = 0;
    size_t subghz_position = 0;
    bool found_subghz = false;

    menu_add_item(
        menu,
        LOADER_APPLICATIONS_NAME,
        &A_Plugins_14,
        position,
        loader_menu_applications_callback,
        loader_menu);
    position++;

    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        if(!found_subghz && strcmp(FLIPPER_APPS[i].name, "Sub-GHz") == 0) {
            subghz_position = position;
            found_subghz = true;
        }
        menu_add_item(
            menu,
            FLIPPER_APPS[i].name,
            FLIPPER_APPS[i].icon,
            i,
            loader_menu_apps_callback,
            loader_menu);
        position++;
    }

    const size_t apps_count = main_menu_apps_get_count(loader_menu->apps);
    for(size_t i = 0; i < apps_count; i++) {
        if(!main_menu_apps_is_available(loader_menu->apps, i)) continue;
        menu_add_item(
            menu,
            main_menu_apps_get_name(loader_menu->apps, i),
            main_menu_apps_get_icon(loader_menu->apps, i),
            i,
            loader_menu_external_apps_callback,
            loader_menu);
        position++;
    }

    menu_add_item(
        menu, "Settings", &A_Settings_14, position, loader_menu_switch_to_settings, loader_menu);

    if(found_subghz) {
        menu_set_selected_item(menu, subghz_position);
    }
}

static void loader_menu_build_submenu(Submenu* submenu, LoaderMenu* loader_menu) {
    for(size_t i = 0; i < FLIPPER_EXTSETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            submenu,
            FLIPPER_EXTSETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_EXTSETTINGS_APPS[i].name,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            submenu,
            FLIPPER_SETTINGS_APPS[i].name,
            (uint32_t)FLIPPER_SETTINGS_APPS[i].name,
            loader_menu_settings_menu_callback,
            loader_menu);
    }
}

static int32_t loader_menu_thread(void* p) {
    LoaderMenu* loader_menu = p;
    furi_assert(loader_menu);

    loader_menu->gui = furi_record_open(RECORD_GUI);
    loader_menu->statusbar_hidden = false;
    loader_menu_active = loader_menu;
    Submenu* settings_menu = submenu_alloc();
    loader_menu_build_submenu(settings_menu, loader_menu);

    loader_menu->view_dispatcher = view_dispatcher_alloc();

    View* primary_view = menu_get_view(loader_menu->primary_menu);
    view_set_previous_callback(primary_view, loader_menu_exit);
    view_dispatcher_add_view(loader_menu->view_dispatcher, LoaderMenuViewPrimary, primary_view);

    View* settings_view = submenu_get_view(settings_menu);
    if(loader_menu->start_view == LoaderMenuStartViewSettings) {
        view_set_previous_callback(settings_view, loader_menu_exit);
    } else {
        view_set_previous_callback(settings_view, loader_menu_switch_to_primary);
    }
    view_dispatcher_add_view(loader_menu->view_dispatcher, LoaderMenuViewSettings, settings_view);
    const bool open_settings = loader_menu->start_view == LoaderMenuStartViewSettings;
    if(open_settings) {
        loader_menu_statusbar_set_hidden(loader_menu, true);
    }
    view_dispatcher_switch_to_view(
        loader_menu->view_dispatcher,
        open_settings ? LoaderMenuViewSettings : LoaderMenuViewPrimary);

    view_dispatcher_attach_to_gui(
        loader_menu->view_dispatcher, loader_menu->gui, ViewDispatcherTypeDesktop);
    view_dispatcher_run(loader_menu->view_dispatcher);

    loader_menu_statusbar_set_hidden(loader_menu, false);

    if(loader_menu->closed_cb) {
        loader_menu->closed_cb(loader_menu->context);
    }

    view_dispatcher_remove_view(loader_menu->view_dispatcher, LoaderMenuViewPrimary);
    view_dispatcher_remove_view(loader_menu->view_dispatcher, LoaderMenuViewSettings);
    view_dispatcher_free(loader_menu->view_dispatcher);
    submenu_free(settings_menu);
    loader_menu_active = NULL;
    furi_record_close(RECORD_GUI);
    loader_menu->gui = NULL;

    return 0;
}

LoaderMenu* loader_menu_alloc(
    void (*closed_cb)(void*),
    void* context,
    LoaderMenuStartView start_view,
    const MenuStyle* style) {
    LoaderMenu* loader_menu = malloc(sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->start_view = start_view;
    loader_menu->apps = main_menu_apps_alloc();
    loader_menu->primary_menu = menu_alloc();
    menu_set_style(loader_menu->primary_menu, style);
    loader_menu_build_menu(loader_menu);
    loader_menu->thread = furi_thread_alloc_ex(TAG, 1024, loader_menu_thread, loader_menu);
    furi_thread_start(loader_menu->thread);
    return loader_menu;
}

void loader_menu_free(LoaderMenu* loader_menu) {
    furi_assert(loader_menu);
    furi_thread_join(loader_menu->thread);
    furi_thread_free(loader_menu->thread);
    menu_free(loader_menu->primary_menu);
    main_menu_apps_free(loader_menu->apps);
    free(loader_menu);
}

void loader_menu_set_style(LoaderMenu* loader_menu, const MenuStyle* style) {
    furi_assert(loader_menu);
    menu_set_style(loader_menu->primary_menu, style);
}
