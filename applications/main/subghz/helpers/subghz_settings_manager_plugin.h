#pragma once
#include "subghz_feature_plugin.h"
#define SUBGHZ_SETTINGS_MANAGER_SCENES(X) \
    X(settings_user)                      \
    X(settings_user_freq)                 \
    X(settings_user_hopper)               \
    X(settings_user_presets)              \
    X(settings_user_item)                 \
    X(settings_user_text)                 \
    X(settings_user_add)
#define SUBGHZ_SETTINGS_MANAGER_PLUGIN_APP_ID "SubGhzSettingsManagerPlugin"
#define SUBGHZ_SETTINGS_MANAGER_PLUGIN_PATH   SUBGHZ_FEATURE_PLUGIN_DIR "subghz_settings_manager.fal"
