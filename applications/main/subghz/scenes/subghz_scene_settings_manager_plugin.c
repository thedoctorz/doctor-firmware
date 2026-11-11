#include "../subghz_i.h"
#include "../helpers/subghz_settings_manager_plugin.h"

static const SubGhzScenePluginInfo subghz_settings_manager_plugin_info = {
    .app_id = SUBGHZ_SETTINGS_MANAGER_PLUGIN_APP_ID,
    .path = SUBGHZ_SETTINGS_MANAGER_PLUGIN_PATH,
    .missing_message = "Sub-GHz Settings\nplugin is missing\nor outdated. Update\nresources.",
};

#define SUBGHZ_SCENE_PLUGIN_SLOT(subghz) ((subghz)->settings_manager_plugin)
#define SUBGHZ_SCENE_PLUGIN_INFO         subghz_settings_manager_plugin_info

SUBGHZ_SCENE_PLUGIN_STUBS(SUBGHZ_SETTINGS_MANAGER_SCENES)
