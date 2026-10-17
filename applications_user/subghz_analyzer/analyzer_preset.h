#pragma once

#include "analyzer_model.h"

#include <lib/subghz/subghz_setting.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_PRESET_NAME_LEN 20u
#define ANALYZER_PRESET_MAX_RANK 4u

typedef struct {
    bool valid;
    uint8_t mod;
    uint32_t bandwidth_hz;
    uint32_t rate_baud;
    bool has_deviation;
    uint32_t deviation_hz;
    bool dcfilt_off;
    bool cs_gate;
} AnalyzerPresetParams;

typedef enum {
    AnalyzerMatchEquivalent,
    AnalyzerMatchClose,
    AnalyzerMatchDifferent,
    AnalyzerMatchWrongFamily,
} AnalyzerMatchTier;

const char* analyzer_preset_tier_name(AnalyzerMatchTier tier);

typedef struct {
    char name[ANALYZER_PRESET_NAME_LEN];
    int setting_index;
    AnalyzerMatchTier tier;
    bool exact_blob;
    uint16_t grid_delta;
    AnalyzerParam worst_axis;
    uint32_t worst_measured;
    uint32_t worst_preset;
    bool narrower_than_signal;
} AnalyzerPresetMatch;

bool analyzer_preset_decode(const uint8_t* blob, size_t len, AnalyzerPresetParams* out);

void analyzer_preset_params_from_model(const AnalyzerModel* model, AnalyzerPresetParams* out);

uint8_t analyzer_preset_rank(
    SubGhzSetting* setting,
    const AnalyzerModel* measured,
    uint32_t occupied_bw_hz,
    AnalyzerPresetMatch* out,
    uint8_t cap);

void analyzer_preset_derive_name(const AnalyzerModel* model, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
