#pragma once

#include <furi_hal.h>
#include <stdint.h>
#include <stdbool.h>
#include <storage/storage.h>
#include <lib/subghz/types.h>

#define SUBGHZ_LAST_SETTING_FREQUENCY_ANALYZER_TRIGGER        (-93.0f)
// 1 = "AM650"
// "AM270", "AM650", "FM238", "FM12K", "FM476",
#define SUBGHZ_LAST_SETTING_DEFAULT_PRESET                    1
#define SUBGHZ_LAST_SETTING_DEFAULT_FREQUENCY                 433920000
#define SUBGHZ_LAST_SETTING_FREQUENCY_ANALYZER_FEEDBACK_LEVEL 2

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frequency;
    uint32_t preset_index; // AKA Modulation
    uint32_t frequency_analyzer_feedback_level;
    float frequency_analyzer_trigger;
    bool protocol_file_names;
    bool protocol_folders;
    bool enable_hopping;
    uint32_t ignore_filter;
    uint32_t filter;
    float rssi;
    bool delete_old_signals;
    float hopping_threshold;
    uint32_t hopping_mode;
    uint32_t hopping_mod_a;
    uint32_t hopping_mod_b;
    bool leds_and_amp;
    uint8_t tx_power;
    bool autosave;
    bool gps_companion;
    bool read_raw_auto;
} SubGhzLastSettings;

SubGhzLastSettings* subghz_last_settings_alloc(void);

void subghz_last_settings_free(SubGhzLastSettings* instance);

void subghz_last_settings_load(SubGhzLastSettings* instance, size_t preset_count);

bool subghz_last_settings_save(SubGhzLastSettings* instance);

#ifdef __cplusplus
}
#endif
