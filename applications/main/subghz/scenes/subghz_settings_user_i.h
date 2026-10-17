#pragma once

#include "../subghz_i.h"

#include <stdio.h>

// Active setting_user file edited by the in-app settings manager.
#define SUBGHZ_SETTINGS_USER_PATH EXT_PATH("subghz/assets/setting_user")

// Which kind of item the shared "item actions" / "text input" scenes operate on.
typedef enum {
    SubGhzSettingsUserCtxFrequency = 0,
    SubGhzSettingsUserCtxPreset = 1,
} SubGhzSettingsUserCtx;

static inline uint8_t subghz_settings_user_values_count(size_t count) {
    if(count == 0) return 1;
    return (count > 255) ? 255 : (uint8_t)count;
}

static inline void
    subghz_settings_user_format_freq_buf(char* out, size_t cap, uint32_t frequency) {
    snprintf(out, cap, "%lu.%02lu", frequency / 1000000UL, (frequency % 1000000UL) / 10000UL);
}

static inline void subghz_settings_user_clamp_frequency(SubGhz* subghz) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    uint32_t freq = subghz->last_settings->frequency;
    size_t count = subghz_setting_get_frequency_count(setting);
    for(size_t i = 0; i < count; i++) {
        if(subghz_setting_get_frequency(setting, i) == freq) return;
    }
    subghz->last_settings->frequency = subghz_setting_get_default_frequency(setting);
    subghz_last_settings_save(subghz->last_settings);
}

static inline void subghz_settings_user_clamp_preset_index(SubGhz* subghz) {
    size_t count = subghz_setting_get_preset_count(subghz_txrx_get_setting(subghz->txrx));
    if(count == 0) return;
    if(subghz->last_settings->preset_index >= count) {
        subghz->last_settings->preset_index = 1;
        subghz_last_settings_save(subghz->last_settings);
    }
}

// Persist the live setting to the setting_user file. Returns false on write error.
static inline bool subghz_settings_user_persist(SubGhz* subghz) {
    bool ok =
        subghz_setting_save(subghz_txrx_get_setting(subghz->txrx), SUBGHZ_SETTINGS_USER_PATH);
    if(!ok) {
        FURI_LOG_E("SubGhzSettingsUser", "Failed to save %s", SUBGHZ_SETTINGS_USER_PATH);
        notification_message(subghz->notifications, &sequence_error);
        return false;
    }
    subghz_settings_user_clamp_preset_index(subghz);
    subghz_settings_user_clamp_frequency(subghz);
    return true;
}

// Modal yes/no confirmation. Returns true if the user pressed the right (confirm) button.
static inline bool
    subghz_settings_user_confirm(SubGhz* subghz, const char* header, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, header, 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Cancel", NULL, "Yes");
    DialogMessageButton result = dialog_message_show(subghz->dialogs, message);
    dialog_message_free(message);
    return result == DialogMessageButtonRight;
}
