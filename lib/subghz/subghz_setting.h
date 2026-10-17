
#pragma once

#include <math.h>
#include <furi.h>
#include <furi_hal.h>
#include <lib/flipper_format/flipper_format.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUBGHZ_SETTING_DEFAULT_PRESET_COUNT 4

/** Hard caps for setting_user lists / custom preset blobs (load + in-app editor). */
#define SUBGHZ_SETTING_MAX_FREQUENCY_COUNT        128
#define SUBGHZ_SETTING_MAX_HOPPER_FREQUENCY_COUNT 128
#define SUBGHZ_SETTING_MAX_PRESET_COUNT           48
#define SUBGHZ_SETTING_MAX_PRESET_DATA_SIZE       512

typedef struct SubGhzSetting SubGhzSetting;

SubGhzSetting* subghz_setting_alloc(void);

void subghz_setting_free(SubGhzSetting* instance);

void subghz_setting_load(SubGhzSetting* instance, const char* file_path);

size_t subghz_setting_get_frequency_count(SubGhzSetting* instance);

size_t subghz_setting_get_hopper_frequency_count(SubGhzSetting* instance);

size_t subghz_setting_get_preset_count(SubGhzSetting* instance);

const char* subghz_setting_get_preset_name(SubGhzSetting* instance, size_t idx);

/** Index of the preset with the given name, or -1 if there is no such preset */
int subghz_setting_get_inx_preset_by_name(SubGhzSetting* instance, const char* preset_name);

uint8_t* subghz_setting_get_preset_data(SubGhzSetting* instance, size_t idx);

size_t subghz_setting_get_preset_data_size(SubGhzSetting* instance, size_t idx);

/** Preset data of the preset with the given name, or NULL if there is no such preset */
uint8_t* subghz_setting_get_preset_data_by_name(SubGhzSetting* instance, const char* preset_name);

bool subghz_setting_load_custom_preset(
    SubGhzSetting* instance,
    const char* preset_name,
    FlipperFormat* fff_data_file);

bool subghz_setting_delete_custom_preset(SubGhzSetting* instance, const char* preset_name);

uint32_t subghz_setting_get_frequency(SubGhzSetting* instance, size_t idx);

uint32_t subghz_setting_get_hopper_frequency(SubGhzSetting* instance, size_t idx);

uint32_t subghz_setting_get_frequency_default_index(SubGhzSetting* instance);

uint32_t subghz_setting_get_default_frequency(SubGhzSetting* instance);

void subghz_setting_set_default_frequency(SubGhzSetting* instance, uint32_t frequency_to_setup);

uint8_t subghz_setting_customs_presets_to_log(SubGhzSetting* instance);

/** Number of firmware built-in presets (always first in the preset list). */
size_t subghz_setting_get_builtin_preset_count(SubGhzSetting* instance);

/** Append a frequency (rejects invalid or already-present). Returns true on add. */
bool subghz_setting_add_frequency(SubGhzSetting* instance, uint32_t frequency);

/** Remove the frequency at idx. */
bool subghz_setting_delete_frequency(SubGhzSetting* instance, size_t idx);

/** Append a hopper frequency (rejects invalid or already-present). */
bool subghz_setting_add_hopper_frequency(SubGhzSetting* instance, uint32_t frequency);

/** Remove the hopper frequency at idx. */
bool subghz_setting_delete_hopper_frequency(SubGhzSetting* instance, size_t idx);

/** True if the given frequency is currently in the hopper list. */
bool subghz_setting_is_hopper_frequency(SubGhzSetting* instance, uint32_t frequency);

bool subghz_setting_get_add_standard_frequencies(SubGhzSetting* instance);

void subghz_setting_set_add_standard_frequencies(SubGhzSetting* instance, bool add);

size_t subghz_setting_get_user_frequency_count(SubGhzSetting* instance);
uint32_t subghz_setting_get_user_frequency(SubGhzSetting* instance, size_t idx);
bool subghz_setting_add_user_frequency(SubGhzSetting* instance, uint32_t frequency);
size_t subghz_setting_delete_user_frequency_value(SubGhzSetting* instance, uint32_t frequency);

size_t subghz_setting_get_user_hopper_frequency_count(SubGhzSetting* instance);
uint32_t subghz_setting_get_user_hopper_frequency(SubGhzSetting* instance, size_t idx);
bool subghz_setting_add_user_hopper_frequency(SubGhzSetting* instance, uint32_t frequency);
size_t
    subghz_setting_delete_user_hopper_frequency_value(SubGhzSetting* instance, uint32_t frequency);

bool subghz_setting_preset_is_async_serial(SubGhzSetting* instance, size_t preset_idx);

bool subghz_setting_preset_is_si4463(SubGhzSetting* instance, size_t preset_idx);

bool subghz_setting_has_si4463_preset(SubGhzSetting* instance);

const char* subghz_setting_radio_counterpart(const char* preset_name, bool si4463);

/** Rename a user (non-built-in) custom preset. */
bool subghz_setting_rename_preset(SubGhzSetting* instance, size_t idx, const char* new_name);

/** Delete a user (non-built-in) custom preset. */
bool subghz_setting_delete_preset(SubGhzSetting* instance, size_t idx);

/** Reset frequencies, hopper and presets back to the firmware defaults. */
void subghz_setting_reset_to_default(SubGhzSetting* instance);

/** Persist the current setting to a setting_user file (explicit-list form). */
bool subghz_setting_save(SubGhzSetting* instance, const char* file_path);

#ifdef __cplusplus
}
#endif
