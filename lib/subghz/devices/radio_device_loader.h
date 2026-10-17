#pragma once

#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/drivers/subghz/si4463_ext/si4463_ext_interconnect.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Radio selection for external FAP helpers*/
typedef enum {
    SubGhzRadioDeviceTypeInternal,
    SubGhzRadioDeviceTypeExternalCC1101,
} SubGhzRadioDeviceType;

bool radio_device_loader_is_connect_external(const char* name);

bool radio_device_loader_is_external(const SubGhzDevice* radio_device);

const char* radio_device_loader_external_name(void);

const SubGhzDevice* radio_device_loader_set(
    const SubGhzDevice* current_radio_device,
    SubGhzRadioDeviceType radio_device_type);

void radio_device_loader_end(const SubGhzDevice* radio_device);

#ifdef __cplusplus
}
#endif
