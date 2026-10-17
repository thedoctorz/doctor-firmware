#pragma once

#include <gui/scene_manager.h>
#include <flipper_application/plugins/plugin_manager.h>

#include "../subghz.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    PluginManager* manager;
    const SceneManagerHandlers* handlers;
    bool entered;
    uint32_t scene;
    uint8_t depth;
    bool release_pending;
} SubGhzScenePlugin;

typedef struct {
    const char* app_id;
    const char* path;
    const char* missing_message;
} SubGhzScenePluginInfo;

void subghz_scene_plugin_on_enter(
    SubGhz* subghz,
    SubGhzScenePlugin* plugin,
    const SubGhzScenePluginInfo* info,
    uint32_t scene);

bool subghz_scene_plugin_on_event(
    SubGhz* subghz,
    SubGhzScenePlugin* plugin,
    uint32_t scene,
    SceneManagerEvent event);

void subghz_scene_plugin_on_exit(SubGhz* subghz, SubGhzScenePlugin* plugin, uint32_t scene);

void subghz_scene_plugin_release(SubGhz* subghz, SubGhzScenePlugin* plugin);

#define SUBGHZ_SCENE_PLUGIN_INDEX(name) SubGhzScenePluginIndex_##name,

#define SUBGHZ_SCENE_PLUGIN_STUB(name)                                                        \
    void subghz_scene_##name##_on_enter(void* context) {                                      \
        SubGhz* subghz = context;                                                             \
        subghz_scene_plugin_on_enter(                                                         \
            subghz,                                                                           \
            &SUBGHZ_SCENE_PLUGIN_SLOT(subghz),                                                \
            &SUBGHZ_SCENE_PLUGIN_INFO,                                                        \
            SubGhzScenePluginIndex_##name);                                                   \
    }                                                                                         \
    bool subghz_scene_##name##_on_event(void* context, SceneManagerEvent event) {             \
        SubGhz* subghz = context;                                                             \
        return subghz_scene_plugin_on_event(                                                  \
            subghz, &SUBGHZ_SCENE_PLUGIN_SLOT(subghz), SubGhzScenePluginIndex_##name, event); \
    }                                                                                         \
    void subghz_scene_##name##_on_exit(void* context) {                                       \
        SubGhz* subghz = context;                                                             \
        subghz_scene_plugin_on_exit(                                                          \
            subghz, &SUBGHZ_SCENE_PLUGIN_SLOT(subghz), SubGhzScenePluginIndex_##name);        \
    }

#define SUBGHZ_SCENE_PLUGIN_STUBS(list) \
    enum {                              \
        list(SUBGHZ_SCENE_PLUGIN_INDEX) \
    };                                  \
    list(SUBGHZ_SCENE_PLUGIN_STUB)

#define SUBGHZ_SCENE_PLUGIN_ON_ENTER(name) subghz_scene_##name##_on_enter,
#define SUBGHZ_SCENE_PLUGIN_ON_EVENT(name) subghz_scene_##name##_on_event,
#define SUBGHZ_SCENE_PLUGIN_ON_EXIT(name)  subghz_scene_##name##_on_exit,

#define SUBGHZ_SCENE_PLUGIN_HANDLERS(handlers, list)               \
    static const AppSceneOnEnterCallback handlers##_on_enter[] = { \
        list(SUBGHZ_SCENE_PLUGIN_ON_ENTER)};                       \
    static const AppSceneOnEventCallback handlers##_on_event[] = { \
        list(SUBGHZ_SCENE_PLUGIN_ON_EVENT)};                       \
    static const AppSceneOnExitCallback handlers##_on_exit[] = {   \
        list(SUBGHZ_SCENE_PLUGIN_ON_EXIT)};                        \
    static const SceneManagerHandlers handlers = {                 \
        .on_enter_handlers = handlers##_on_enter,                  \
        .on_event_handlers = handlers##_on_event,                  \
        .on_exit_handlers = handlers##_on_exit,                    \
        .scene_num = COUNT_OF(handlers##_on_enter),                \
    };

#ifdef __cplusplus
}
#endif
