#include <subghz/subghz_i.h>
#include <subghz/helpers/subghz_add_manually_plugin.h>

#include <flipper_application/flipper_application.h>

SUBGHZ_SCENE_PLUGIN_HANDLERS(subghz_add_manually_handlers, SUBGHZ_ADD_MANUALLY_SCENES)

static const FlipperAppPluginDescriptor subghz_add_manually_plugin_descriptor = {
    .appid = SUBGHZ_ADD_MANUALLY_PLUGIN_APP_ID,
    .ep_api_version = SUBGHZ_FEATURE_PLUGIN_API_VERSION,
    .entry_point = &subghz_add_manually_handlers,
};

const FlipperAppPluginDescriptor* subghz_add_manually_ep(void) {
    return &subghz_add_manually_plugin_descriptor;
}
