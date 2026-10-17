#pragma once

#include <gui/scene_manager.h>

#include "../subghz.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUBGHZ_FREQUENCY_ANALYZER_PLUGIN_APP_ID "SubGhzFrequencyAnalyzerPlugin"

typedef struct {
    void (*on_enter)(SubGhz* subghz);
    bool (*on_event)(SubGhz* subghz, SceneManagerEvent event);
    void (*on_exit)(SubGhz* subghz);
} SubGhzFrequencyAnalyzerPlugin;

void subghz_frequency_analyzer_plugin_load(SubGhz* subghz);

void subghz_frequency_analyzer_plugin_unload(SubGhz* subghz);

#ifdef __cplusplus
}
#endif
