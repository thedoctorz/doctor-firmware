#include <subghz/subghz_i.h>
#include <subghz/helpers/subghz_settings_manager_plugin.h>

#include <flipper_application/flipper_application.h>

SUBGHZ_SCENE_PLUGIN_HANDLERS(subghz_settings_manager_handlers, SUBGHZ_SETTINGS_MANAGER_SCENES)

static const FlipperAppPluginDescriptor subghz_settings_manager_plugin_descriptor = {
    .appid = SUBGHZ_SETTINGS_MANAGER_PLUGIN_APP_ID,
    .ep_api_version = SUBGHZ_FEATURE_PLUGIN_API_VERSION,
    .entry_point = &subghz_settings_manager_handlers,
};

const FlipperAppPluginDescriptor* subghz_settings_manager_ep(void) {
    return &subghz_settings_manager_plugin_descriptor;
}
