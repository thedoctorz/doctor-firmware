#include "registry.h"

#include "cc1101_int/cc1101_int_interconnect.h"
#include <flipper_application/plugins/plugin_manager.h>
#include <loader/firmware_api/firmware_api.h>

#define TAG "SubGhzDeviceRegistry"

#define SUBGHZ_DEVICE_REGISTRY_PLUGIN_MAX 4

typedef struct {
    PluginManager* manager;
    const SubGhzDevice* device;
} SubGhzDeviceRegistryPlugin;

struct SubGhzDeviceRegistry {
    FuriMutex* mutex;
    SubGhzDeviceRegistryPlugin plugins[SUBGHZ_DEVICE_REGISTRY_PLUGIN_MAX];
    size_t plugin_count;
};

static SubGhzDeviceRegistry* subghz_device_registry = NULL;

void subghz_device_registry_init(void) {
    SubGhzDeviceRegistry* subghz_device =
        (SubGhzDeviceRegistry*)malloc(sizeof(SubGhzDeviceRegistry));
    subghz_device->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    subghz_device->plugin_count = 0;
    subghz_device_registry = subghz_device;
}

void subghz_device_registry_deinit(void) {
    for(size_t i = 0; i < subghz_device_registry->plugin_count; i++) {
        plugin_manager_free(subghz_device_registry->plugins[i].manager);
    }
    furi_mutex_free(subghz_device_registry->mutex);
    free(subghz_device_registry);
    subghz_device_registry = NULL;
}

static const SubGhzDevice* subghz_device_registry_find(const char* name) {
    if(strcmp(name, subghz_device_cc1101_int.name) == 0) return &subghz_device_cc1101_int;
    for(size_t i = 0; i < subghz_device_registry->plugin_count; i++) {
        if(strcmp(name, subghz_device_registry->plugins[i].device->name) == 0) {
            return subghz_device_registry->plugins[i].device;
        }
    }
    return NULL;
}

static const SubGhzDevice* subghz_device_registry_load(const char* name) {
    if(subghz_device_registry->plugin_count >= SUBGHZ_DEVICE_REGISTRY_PLUGIN_MAX) return NULL;

    FuriString* path = furi_string_alloc_printf(
        EXT_PATH("apps_data/subghz/plugins/") SUBGHZ_RADIO_DEVICE_PLUGIN_FAL_PREFIX "%s.fal",
        name);
    PluginManager* manager = plugin_manager_alloc(
        SUBGHZ_RADIO_DEVICE_PLUGIN_APP_ID,
        SUBGHZ_RADIO_DEVICE_PLUGIN_API_VERSION,
        firmware_api_interface);
    PluginManagerError error = plugin_manager_load_single(manager, furi_string_get_cstr(path));
    const SubGhzDevice* device = NULL;
    if(error == PluginManagerErrorNone) {
        device = plugin_manager_get_ep(manager, 0);
        if(device && strcmp(device->name, name) != 0) {
            FURI_LOG_E(TAG, "%s provides '%s'", furi_string_get_cstr(path), device->name);
            device = NULL;
        }
    } else {
        FURI_LOG_W(TAG, "No radio device %s (error %d)", furi_string_get_cstr(path), error);
    }
    furi_string_free(path);

    if(!device) {
        plugin_manager_free(manager);
        return NULL;
    }
    SubGhzDeviceRegistryPlugin* plugin =
        &subghz_device_registry->plugins[subghz_device_registry->plugin_count++];
    plugin->manager = manager;
    plugin->device = device;
    FURI_LOG_I(TAG, "Loaded radio device %s", name);
    return device;
}

static bool subghz_device_registry_device_is_connect(const SubGhzDevice* device) {
    return device->interconnect->is_connect && device->interconnect->is_connect();
}

static void subghz_device_registry_unload_last(void) {
    SubGhzDeviceRegistryPlugin* plugin =
        &subghz_device_registry->plugins[--subghz_device_registry->plugin_count];
    plugin_manager_free(plugin->manager);
    plugin->manager = NULL;
    plugin->device = NULL;
}

bool subghz_device_registry_is_valid(void) {
    return subghz_device_registry != NULL;
}

const SubGhzDevice* subghz_device_registry_get_by_name(const char* name) {
    furi_assert(subghz_device_registry);
    if(name == NULL) return NULL;

    furi_check(furi_mutex_acquire(subghz_device_registry->mutex, FuriWaitForever) == FuriStatusOk);
    const SubGhzDevice* device = subghz_device_registry_find(name);
    if(!device) device = subghz_device_registry_load(name);
    furi_mutex_release(subghz_device_registry->mutex);
    return device;
}

bool subghz_device_registry_is_loaded(const char* name) {
    furi_assert(subghz_device_registry);
    if(name == NULL) return false;

    furi_check(furi_mutex_acquire(subghz_device_registry->mutex, FuriWaitForever) == FuriStatusOk);
    bool loaded = subghz_device_registry_find(name) != NULL;
    furi_mutex_release(subghz_device_registry->mutex);
    return loaded;
}

bool subghz_device_registry_is_connect(const char* name) {
    furi_assert(subghz_device_registry);
    if(name == NULL) return false;

    furi_check(furi_mutex_acquire(subghz_device_registry->mutex, FuriWaitForever) == FuriStatusOk);
    bool is_connect = false;
    const SubGhzDevice* device = subghz_device_registry_find(name);
    if(device) {
        is_connect = subghz_device_registry_device_is_connect(device);
    } else {
        device = subghz_device_registry_load(name);
        if(device) {
            is_connect = subghz_device_registry_device_is_connect(device);
            if(!is_connect) subghz_device_registry_unload_last();
        }
    }
    furi_mutex_release(subghz_device_registry->mutex);
    return is_connect;
}

const SubGhzDevice* subghz_device_registry_get_by_index(size_t index) {
    furi_assert(subghz_device_registry);
    if(index == 0) return &subghz_device_cc1101_int;
    if(index <= subghz_device_registry->plugin_count) {
        return subghz_device_registry->plugins[index - 1].device;
    }
    return NULL;
}
