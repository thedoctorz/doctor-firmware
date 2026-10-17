#include "subghz_scene_plugin.h"

#include "subghz_feature_plugin.h"

static void subghz_scene_plugin_unload(SubGhz* subghz, SubGhzScenePlugin* plugin) {
    plugin->handlers = NULL;
    plugin->release_pending = false;
    subghz_feature_plugin_unload(subghz, &plugin->manager);
}

static void subghz_scene_plugin_call_end(SubGhz* subghz, SubGhzScenePlugin* plugin) {
    furi_assert(plugin->depth);
    if(--plugin->depth == 0 && plugin->release_pending) {
        subghz_scene_plugin_unload(subghz, plugin);
    }
}

void subghz_scene_plugin_on_enter(
    SubGhz* subghz,
    SubGhzScenePlugin* plugin,
    const SubGhzScenePluginInfo* info,
    uint32_t scene) {
    if(!plugin->handlers) {
        plugin->handlers = subghz_feature_plugin_load(
            subghz, &plugin->manager, info->app_id, info->path, info->missing_message);
        if(!plugin->handlers) return;
    }
    furi_check(scene < plugin->handlers->scene_num);
    plugin->release_pending = false;

    plugin->entered = true;
    plugin->scene = scene;
    plugin->depth++;
    plugin->handlers->on_enter_handlers[scene](subghz);
    subghz_scene_plugin_call_end(subghz, plugin);
}

bool subghz_scene_plugin_on_event(
    SubGhz* subghz,
    SubGhzScenePlugin* plugin,
    uint32_t scene,
    SceneManagerEvent event) {
    if(!plugin->handlers) {
        return subghz_feature_plugin_handle_missing(subghz, event);
    }
    plugin->depth++;
    bool consumed = plugin->handlers->on_event_handlers[scene](subghz, event);
    subghz_scene_plugin_call_end(subghz, plugin);
    return consumed;
}

void subghz_scene_plugin_on_exit(SubGhz* subghz, SubGhzScenePlugin* plugin, uint32_t scene) {
    if(!plugin->handlers || !plugin->entered) return;
    plugin->entered = false;
    plugin->depth++;
    plugin->handlers->on_exit_handlers[scene](subghz);
    subghz_scene_plugin_call_end(subghz, plugin);
}

void subghz_scene_plugin_release(SubGhz* subghz, SubGhzScenePlugin* plugin) {
    if(!plugin->handlers) return;
    if(plugin->entered) {
        subghz_scene_plugin_on_exit(subghz, plugin, plugin->scene);
    }
    if(plugin->depth) {
        plugin->release_pending = true;
    } else {
        subghz_scene_plugin_unload(subghz, plugin);
    }
}
