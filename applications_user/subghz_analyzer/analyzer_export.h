#pragma once

#include "analyzer_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_SETTING_USER_PATH EXT_PATH("subghz/assets/setting_user")

typedef enum {
    AnalyzerSaveCreated,
    AnalyzerSaveReusedExisting,
    AnalyzerSaveListFull,
    AnalyzerSaveError,
} AnalyzerSaveResult;

const char* analyzer_export_result_text(AnalyzerSaveResult result);

AnalyzerSaveResult analyzer_export_save_preset(
    const AnalyzerModel* model,
    const char* name,
    char* stored_name,
    size_t stored_name_cap,
    int* preset_index);

bool analyzer_export_preselect(uint32_t frequency, int preset_index);

void analyzer_export_open_subghz(uint32_t frequency, int preset_index);

#ifdef __cplusplus
}
#endif
