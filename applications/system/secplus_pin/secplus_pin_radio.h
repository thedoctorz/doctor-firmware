#pragma once

#include <furi.h>
#include <lib/subghz/subghz_tx_rx_worker.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/protocols/protocol_items.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <flipper_format/flipper_format.h>

typedef struct {
    uint32_t frequency;
    FuriString* preset_name;
    uint8_t* custom_preset;
    size_t custom_preset_size;
} SecPlusPinPreset;

bool secplus_pin_preset_by_name(const char* name, FuriHalSubGhzPreset* out);

typedef enum {
    SecPlusPinTxOk,
    SecPlusPinTxNotAllowed,
    SecPlusPinTxError,
} SecPlusPinTxStatus;

SecPlusPinTxStatus secplus_pin_radio_tx(const SecPlusPinPreset* preset, FlipperFormat* fff);
