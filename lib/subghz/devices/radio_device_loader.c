#include "radio_device_loader.h"
#include "registry.h"

#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/drivers/subghz/si4463_ext/si4463_ext_interconnect.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <furi.h>
#include <furi_hal.h>

#define TAG "RadioDeviceLoader"

static bool radio_device_loader_otg_enabled_by_loader = false;

static void radio_device_loader_power_on(void) {
    uint8_t attempts = 0;
    while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10);
    }
    if(furi_hal_power_is_otg_enabled()) {
        radio_device_loader_otg_enabled_by_loader = true;
    }
}

static void radio_device_loader_power_off(void) {
    if(radio_device_loader_otg_enabled_by_loader && furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
        radio_device_loader_otg_enabled_by_loader = false;
    }
}

bool radio_device_loader_is_connect_external(const char* name) {
    bool is_connect = false;
    bool is_otg_enabled = furi_hal_power_is_otg_enabled();

    if(!is_otg_enabled) {
        radio_device_loader_power_on();
    }

    is_connect = subghz_device_registry_is_connect(name);

    if(!is_otg_enabled) {
        radio_device_loader_power_off();
    }
    return is_connect;
}

bool radio_device_loader_is_external(const SubGhzDevice* radio_device) {
    if(!radio_device) return false;
    return radio_device != subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
}

const char* radio_device_loader_external_name(void) {
    static const char* const names[] = {
        SUBGHZ_DEVICE_CC1101_EXT_NAME,
        SUBGHZ_DEVICE_SI4463_EXT_NAME,
    };
    for(size_t i = 0; i < COUNT_OF(names); i++) {
        if(subghz_device_registry_is_loaded(names[i]) &&
           subghz_device_registry_is_connect(names[i])) {
            return names[i];
        }
    }
    for(size_t i = 0; i < COUNT_OF(names); i++) {
        if(!subghz_device_registry_is_loaded(names[i]) &&
           subghz_device_registry_is_connect(names[i])) {
            return names[i];
        }
    }

    return NULL;
}

const SubGhzDevice* radio_device_loader_set(
    const SubGhzDevice* current_radio_device,
    SubGhzRadioDeviceType radio_device_type) {
    const SubGhzDevice* target_radio_device = NULL;

    if(radio_device_type == SubGhzRadioDeviceTypeExternalCC1101) {
        bool otg_was_enabled = furi_hal_power_is_otg_enabled();
        if(!otg_was_enabled) {
            radio_device_loader_power_on();
        }

        if(radio_device_loader_is_external(current_radio_device) &&
           subghz_devices_is_connect(current_radio_device)) {
            target_radio_device = current_radio_device;
        } else {
            const char* name = radio_device_loader_external_name();
            if(name) {
                radio_device_loader_power_on();
                target_radio_device = subghz_devices_get_by_name(name);
            }
        }

        if(!target_radio_device && !otg_was_enabled) {
            radio_device_loader_power_off();
        }
    }

    if(!target_radio_device) {
        target_radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        if(!target_radio_device) {
            FURI_LOG_E(TAG, "Failed to get internal CC1101 device");
            return NULL;
        }
    }

    if(current_radio_device == target_radio_device) {
        return target_radio_device;
    }

    if(current_radio_device) {
        radio_device_loader_end(current_radio_device);
    }

    if(target_radio_device != subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME)) {
        if(!subghz_devices_begin(target_radio_device)) {
            FURI_LOG_E(TAG, "External radio begin failed");
            radio_device_loader_end(target_radio_device);
            target_radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        }
    }

    return target_radio_device;
}

void radio_device_loader_end(const SubGhzDevice* radio_device) {
    furi_check(radio_device);

    if(radio_device != subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME)) {
        subghz_devices_end(radio_device);
    }
    radio_device_loader_power_off();
}
