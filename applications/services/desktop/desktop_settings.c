#include "desktop_settings.h"
#include "desktop_settings_filename.h"

#include <saved_struct.h>
#include <storage/storage.h>
#include <stddef.h>
#include <string.h>

#define TAG "DesktopSettings"

#define DESKTOP_SETTINGS_VER_14 (14)
#define DESKTOP_SETTINGS_VER_17 (17)
#define DESKTOP_SETTINGS_VER_18 (18)
#define DESKTOP_SETTINGS_VER    (19)

#define DESKTOP_SETTINGS_PATH  INT_PATH(DESKTOP_SETTINGS_FILE_NAME)
#define DESKTOP_SETTINGS_MAGIC (0x17)

#define DESKTOP_SETTINGS_DUMMY_APP_COUNT (9)

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DESKTOP_SETTINGS_DUMMY_APP_COUNT];
} DesktopSettingsV14;

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DESKTOP_SETTINGS_DUMMY_APP_COUNT];
} DesktopSettingsV17;

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DESKTOP_SETTINGS_DUMMY_APP_COUNT];
    char menu_style[32];
} DesktopSettingsV18;

static void desktop_settings_terminate_apps(FavoriteApp* apps, size_t count) {
    for(size_t i = 0; i < count; i++) {
        apps[i].name_or_path[sizeof(apps[i].name_or_path) - 1] = '\0';
    }
}

static void desktop_settings_terminate_strings(DesktopSettings* settings) {
    desktop_settings_terminate_apps(settings->favorite_apps, FavoriteAppNumber);
    settings->menu_style[sizeof(settings->menu_style) - 1] = '\0';
}

static void desktop_settings_copy_common(
    DesktopSettings* settings,
    uint32_t auto_lock_delay_ms,
    uint8_t usb_inhibit_auto_lock,
    uint8_t displayBatteryPercentage,
    uint8_t display_clock,
    const FavoriteApp* favorite_apps,
    const char* menu_style) {
    memset(settings, 0, sizeof(DesktopSettings));
    settings->auto_lock_delay_ms = auto_lock_delay_ms;
    settings->usb_inhibit_auto_lock = usb_inhibit_auto_lock;
    settings->displayBatteryPercentage = displayBatteryPercentage;
    settings->display_clock = display_clock;
    memcpy(settings->favorite_apps, favorite_apps, sizeof(settings->favorite_apps));
    if(menu_style) {
        strlcpy(settings->menu_style, menu_style, sizeof(settings->menu_style));
    }
}

void desktop_settings_load(DesktopSettings* settings) {
    furi_assert(settings);

    bool success = false;

    do {
        uint8_t version;
        if(!saved_struct_get_metadata(DESKTOP_SETTINGS_PATH, NULL, &version, NULL)) break;

        if(version == DESKTOP_SETTINGS_VER) {
            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings,
                sizeof(DesktopSettings),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER);

        } else if(version == DESKTOP_SETTINGS_VER_18) {
            DesktopSettingsV18* settings_v18 = malloc(sizeof(DesktopSettingsV18));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v18,
                sizeof(DesktopSettingsV18),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_18);

            if(success) {
                desktop_settings_copy_common(
                    settings,
                    settings_v18->auto_lock_delay_ms,
                    settings_v18->usb_inhibit_auto_lock,
                    settings_v18->displayBatteryPercentage,
                    settings_v18->display_clock,
                    settings_v18->favorite_apps,
                    settings_v18->menu_style);
            }

            free(settings_v18);

        } else if(version == DESKTOP_SETTINGS_VER_17) {
            DesktopSettingsV17* settings_v17 = malloc(sizeof(DesktopSettingsV17));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v17,
                sizeof(DesktopSettingsV17),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_17);

            if(success) {
                desktop_settings_copy_common(
                    settings,
                    settings_v17->auto_lock_delay_ms,
                    settings_v17->usb_inhibit_auto_lock,
                    settings_v17->displayBatteryPercentage,
                    settings_v17->display_clock,
                    settings_v17->favorite_apps,
                    NULL);
            }

            free(settings_v17);

        } else if(version == DESKTOP_SETTINGS_VER_14) {
            DesktopSettingsV14* settings_v14 = malloc(sizeof(DesktopSettingsV14));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v14,
                sizeof(DesktopSettingsV14),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_14);

            if(success) {
                desktop_settings_copy_common(
                    settings,
                    settings_v14->auto_lock_delay_ms,
                    0,
                    settings_v14->displayBatteryPercentage,
                    settings_v14->display_clock,
                    settings_v14->favorite_apps,
                    NULL);
            }

            free(settings_v14);
        }

    } while(false);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to load file, using defaults");
        memset(settings, 0, sizeof(DesktopSettings));
        desktop_settings_save(settings);
    } else {
        desktop_settings_terminate_strings(settings);
    }
}

void desktop_settings_save(const DesktopSettings* settings) {
    furi_assert(settings);

    const bool success = saved_struct_save(
        DESKTOP_SETTINGS_PATH,
        settings,
        sizeof(DesktopSettings),
        DESKTOP_SETTINGS_MAGIC,
        DESKTOP_SETTINGS_VER);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save file");
    }
}
