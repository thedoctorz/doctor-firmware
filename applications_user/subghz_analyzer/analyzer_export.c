#include "analyzer_export.h"
#include "analyzer_preset.h"

#include <furi.h>
#include <flipper_format/flipper_format.h>
#include <loader/loader.h>
#include <storage/storage.h>

#include <string.h>

#define TAG "SubGhzAnalyzerExport"

#define ANALYZER_LAST_SETTINGS_PATH   EXT_PATH("subghz/assets/last_subghz.settings")
#define ANALYZER_LAST_FIELD_FREQUENCY "Frequency"
#define ANALYZER_LAST_FIELD_PRESET    "Preset"

const char* analyzer_export_result_text(AnalyzerSaveResult result) {
    switch(result) {
    case AnalyzerSaveCreated:
        return "Preset saved";
    case AnalyzerSaveReusedExisting:
        return "Already saved";
    case AnalyzerSaveListFull:
        return "Preset list full";
    case AnalyzerSaveError:
    default:
        return "Save failed";
    }
}

AnalyzerSaveResult analyzer_export_save_preset(
    const AnalyzerModel* model,
    const char* name,
    char* stored_name,
    size_t stored_name_cap,
    int* preset_index) {
    furi_check(model);
    furi_check(name);

    char requested[ANALYZER_PRESET_NAME_LEN];
    snprintf(requested, sizeof(requested), "%s", name);

    if(stored_name && stored_name_cap) stored_name[0] = '\0';
    if(preset_index) *preset_index = -1;

    uint8_t blob[ANALYZER_MODEL_BLOB_MAX];
    const size_t len = analyzer_model_build(model, blob, sizeof(blob));
    if(len == 0) return AnalyzerSaveError;

    SubGhzSetting* setting = subghz_setting_alloc();
    subghz_setting_load(setting, ANALYZER_SETTING_USER_PATH);

    AnalyzerSaveResult result = AnalyzerSaveError;

    do {
        const size_t count = subghz_setting_get_preset_count(setting);
        bool reused = false;
        for(size_t idx = 0; idx < count; idx++) {
            if(subghz_setting_get_preset_data_size(setting, idx) != len) continue;
            const uint8_t* data = subghz_setting_get_preset_data(setting, idx);
            if(!data || memcmp(data, blob, len) != 0) continue;

            const char* existing = subghz_setting_get_preset_name(setting, idx);
            if(stored_name && stored_name_cap && existing) {
                snprintf(stored_name, stored_name_cap, "%s", existing);
            }
            if(preset_index) *preset_index = (int)idx;
            result = AnalyzerSaveReusedExisting;
            reused = true;
            break;
        }
        if(reused) break;

        if(count >= SUBGHZ_SETTING_MAX_PRESET_COUNT) {
            result = AnalyzerSaveListFull;
            break;
        }

        char chosen[ANALYZER_PRESET_NAME_LEN];
        snprintf(chosen, sizeof(chosen), "%s", requested);
        const size_t builtin = subghz_setting_get_builtin_preset_count(setting);
        for(size_t i = 0; i < builtin; i++) {
            const char* builtin_name = subghz_setting_get_preset_name(setting, i);
            if(builtin_name && strcmp(builtin_name, requested) == 0) {
                snprintf(chosen, sizeof(chosen), "%.*s+", (int)(sizeof(chosen) - 2), requested);
                break;
            }
        }

        subghz_setting_delete_custom_preset(setting, chosen);

        FlipperFormat* ff = flipper_format_string_alloc();
        bool loaded = false;
        do {
            if(!flipper_format_write_hex(ff, "Custom_preset_data", blob, (uint16_t)len)) break;
            if(!flipper_format_rewind(ff)) break;
            if(!subghz_setting_load_custom_preset(setting, chosen, ff)) break;
            loaded = true;
        } while(false);
        flipper_format_free(ff);

        if(!loaded) break;
        if(!subghz_setting_save(setting, ANALYZER_SETTING_USER_PATH)) break;

        const size_t after = subghz_setting_get_preset_count(setting);
        for(size_t idx = 0; idx < after; idx++) {
            const char* n = subghz_setting_get_preset_name(setting, idx);
            if(n && strcmp(n, chosen) == 0) {
                if(preset_index) *preset_index = (int)idx;
                break;
            }
        }

        if(stored_name && stored_name_cap) snprintf(stored_name, stored_name_cap, "%s", chosen);
        result = AnalyzerSaveCreated;
    } while(false);

    subghz_setting_free(setting);
    return result;
}

bool analyzer_export_preselect(uint32_t frequency, int preset_index) {
    if(preset_index < 0) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool ok = false;

    do {
        if(!flipper_format_file_open_existing(file, ANALYZER_LAST_SETTINGS_PATH)) break;

        const uint32_t freq = frequency;
        const uint32_t preset = (uint32_t)preset_index;
        if(!flipper_format_insert_or_update_uint32(file, ANALYZER_LAST_FIELD_FREQUENCY, &freq, 1))
            break;
        if(!flipper_format_insert_or_update_uint32(file, ANALYZER_LAST_FIELD_PRESET, &preset, 1))
            break;
        ok = true;
    } while(false);

    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    if(!ok) FURI_LOG_W(TAG, "Could not preselect preset %d", preset_index);
    return ok;
}

void analyzer_export_open_subghz(uint32_t frequency, int preset_index) {
    char args[40];
    if(preset_index >= 0) {
        snprintf(args, sizeof(args), "readraw %lu %d", (unsigned long)frequency, preset_index);
    } else {
        snprintf(args, sizeof(args), "readraw %lu", (unsigned long)frequency);
    }

    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_enqueue_launch(loader, "subghz", args, LoaderDeferredLaunchFlagGui);
    furi_record_close(RECORD_LOADER);
}
