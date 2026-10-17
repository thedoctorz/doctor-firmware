
#pragma once

#include <furi.h>
#include <gui/icon.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAIN_MENU_APPS_DIR  "/ext/apps_data/loader"
#define MAIN_MENU_APPS_PATH MAIN_MENU_APPS_DIR "/mainmenu.txt"

#define MAIN_MENU_APPS_MAX 32

typedef struct MainMenuApps MainMenuApps;

MainMenuApps* main_menu_apps_alloc(void);

void main_menu_apps_free(MainMenuApps* instance);

size_t main_menu_apps_get_count(const MainMenuApps* instance);

const char* main_menu_apps_get_name(const MainMenuApps* instance, size_t index);

const char* main_menu_apps_get_path(const MainMenuApps* instance, size_t index);

const Icon* main_menu_apps_get_icon(const MainMenuApps* instance, size_t index);

bool main_menu_apps_is_available(const MainMenuApps* instance, size_t index);

size_t main_menu_apps_find(const MainMenuApps* instance, const char* path);

bool main_menu_apps_equal(const MainMenuApps* instance, const MainMenuApps* other);

bool main_menu_apps_add(MainMenuApps* instance, const char* path);

void main_menu_apps_remove(MainMenuApps* instance, size_t index);

bool main_menu_apps_move(MainMenuApps* instance, size_t index, bool up);

void main_menu_apps_reset(MainMenuApps* instance);

bool main_menu_apps_save(const MainMenuApps* instance);

#ifdef __cplusplus
}
#endif
