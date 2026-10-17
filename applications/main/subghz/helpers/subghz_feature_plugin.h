#pragma once

#include <flipper_application/plugins/plugin_manager.h>
#include <gui/scene_manager.h>
#include <storage/storage.h>

#include "../subghz.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUBGHZ_FEATURE_PLUGIN_API_VERSION 2

#define SUBGHZ_FEATURE_PLUGIN_DIR EXT_PATH("apps_data/subghz/plugins/")

const void* subghz_feature_plugin_load(
    SubGhz* subghz,
    PluginManager** manager,
    const char* app_id,
    const char* path,
    const char* missing_message);

void subghz_feature_plugin_unload(SubGhz* subghz, PluginManager** manager);

bool subghz_feature_plugin_handle_missing(SubGhz* subghz, SceneManagerEvent event);

#ifdef __cplusplus
}
#endif
