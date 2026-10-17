#include "../subghz_i.h" // IWYU pragma: keep
#include "../helpers/subghz_add_manually_plugin.h"

static const SubGhzScenePluginInfo subghz_add_manually_plugin_info = {
    .app_id = SUBGHZ_ADD_MANUALLY_PLUGIN_APP_ID,
    .path = SUBGHZ_ADD_MANUALLY_PLUGIN_PATH,
    .missing_message = "Add Manually plugin\nis missing or\noutdated. Update\nresources.",
};

#define SUBGHZ_SCENE_PLUGIN_SLOT(subghz) ((subghz)->add_manually_plugin)
#define SUBGHZ_SCENE_PLUGIN_INFO         subghz_add_manually_plugin_info

SUBGHZ_SCENE_PLUGIN_STUBS(SUBGHZ_ADD_MANUALLY_SCENES)
