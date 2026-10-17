#include "../subghz_i.h" // IWYU pragma: keep
#include "../helpers/subghz_clean_dupes_plugin.h"

static const SubGhzScenePluginInfo subghz_clean_dupes_plugin_info = {
    .app_id = SUBGHZ_CLEAN_DUPES_PLUGIN_APP_ID,
    .path = SUBGHZ_CLEAN_DUPES_PLUGIN_PATH,
    .missing_message = "Clean Duplicates\nplugin is missing\nor outdated. Update\nresources.",
};

#define SUBGHZ_SCENE_PLUGIN_SLOT(subghz) ((subghz)->clean_dupes_plugin)
#define SUBGHZ_SCENE_PLUGIN_INFO         subghz_clean_dupes_plugin_info

SUBGHZ_SCENE_PLUGIN_STUBS(SUBGHZ_CLEAN_DUPES_SCENES)
