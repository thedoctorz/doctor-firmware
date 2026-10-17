#include "application_manifest.h"

#include <furi_hal_version.h>
#include <furi.h>

bool flipper_application_manifest_is_valid(const FlipperApplicationManifest* manifest) {
    furi_check(manifest);

    if((manifest->base.manifest_magic != FAP_MANIFEST_MAGIC) ||
       (manifest->base.manifest_version != FAP_MANIFEST_SUPPORTED_VERSION)) {
        return false;
    }

    return true;
}

static uint16_t manifest_api_version_oldest(const ElfApiInterface* api_interface) {
#ifdef FAP_API_COMPAT_VERSION_MAJOR
    return MIN((uint16_t)FAP_API_COMPAT_VERSION_MAJOR, api_interface->api_version_major);
#else
    return api_interface->api_version_major;
#endif
}

static uint16_t manifest_api_version_newest(const ElfApiInterface* api_interface) {
#ifdef FAP_API_COMPAT_VERSION_MAJOR
    return MAX((uint16_t)FAP_API_COMPAT_VERSION_MAJOR, api_interface->api_version_major);
#else
    return api_interface->api_version_major;
#endif
}

bool flipper_application_manifest_is_too_old(
    const FlipperApplicationManifest* manifest,
    const ElfApiInterface* api_interface) {
    furi_check(manifest);
    furi_check(api_interface);

    if(manifest->base.api_version.major < manifest_api_version_oldest(api_interface) /* ||
       manifest->base.api_version.minor > app->api_interface->api_version_minor */) {
        return false;
    }

    return true;
}

bool flipper_application_manifest_is_too_new(
    const FlipperApplicationManifest* manifest,
    const ElfApiInterface* api_interface) {
    furi_check(manifest);
    furi_check(api_interface);

    if(manifest->base.api_version.major > manifest_api_version_newest(api_interface) /* ||
       manifest->base.api_version.minor > app->api_interface->api_version_minor */) {
        return false;
    }

    return true;
}

bool flipper_application_manifest_is_target_compatible(const FlipperApplicationManifest* manifest) {
    furi_check(manifest);

    const Version* version = furi_hal_version_get_firmware_version();
    return version_get_target(version) == manifest->base.hardware_target_id;
}
