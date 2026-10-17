#include <subghz/subghz_i.h>
#include <subghz/helpers/subghz_clean_dupes_plugin.h>

#include <flipper_application/flipper_application.h>

SUBGHZ_SCENE_PLUGIN_HANDLERS(subghz_clean_dupes_handlers, SUBGHZ_CLEAN_DUPES_SCENES)

static const FlipperAppPluginDescriptor subghz_clean_dupes_plugin_descriptor = {
    .appid = SUBGHZ_CLEAN_DUPES_PLUGIN_APP_ID,
    .ep_api_version = SUBGHZ_FEATURE_PLUGIN_API_VERSION,
    .entry_point = &subghz_clean_dupes_handlers,
};

const FlipperAppPluginDescriptor* subghz_clean_dupes_ep(void) {
    return &subghz_clean_dupes_plugin_descriptor;
}
