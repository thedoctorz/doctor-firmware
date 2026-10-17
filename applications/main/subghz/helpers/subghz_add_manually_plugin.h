#pragma once

#include "subghz_feature_plugin.h"

#define SUBGHZ_ADD_MANUALLY_SCENES(X) \
    X(set_type)                       \
    X(set_key)                        \
    X(set_serial)                     \
    X(set_button)                     \
    X(set_counter)                    \
    X(set_seed)

#define SUBGHZ_ADD_MANUALLY_PLUGIN_APP_ID "SubGhzAddManuallyPlugin"
#define SUBGHZ_ADD_MANUALLY_PLUGIN_PATH   SUBGHZ_FEATURE_PLUGIN_DIR "subghz_add_manually.fal"
