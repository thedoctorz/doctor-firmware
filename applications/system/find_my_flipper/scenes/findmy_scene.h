#pragma once

#include <gui/scene_manager.h>
#include "findmy.h"

#define ADD_SCENE(prefix, name, id) FindMyScene##id,
typedef enum {
#include "findmy_scenes.h"
    FindMySceneNum,
} FindMyScene;
#undef ADD_SCENE

extern const SceneManagerHandlers findmy_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "findmy_scenes.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "findmy_scenes.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "findmy_scenes.h"
#undef ADD_SCENE
