#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_SURVEY_BINS 51u

#define ANALYZER_SURVEY_CENTRE_BIN (ANALYZER_SURVEY_BINS / 2u)

#define ANALYZER_SURVEY_STRIDE 5

#define ANALYZER_SURVEY_LSB_MHZ 1586914u

#define ANALYZER_SURVEY_PASSES 6u

#define ANALYZER_SURVEY_LOBE_DIP_DB 6.0f

#define ANALYZER_SURVEY_SNR_GATE_DB 4.0f

#define ANALYZER_SURVEY_LOBE_WIDTH_SLACK_PCT 25u

#define ANALYZER_SURVEY_LOBE_MIN_SEP_FACTOR 1u

typedef struct {
    int8_t rssi_raw;
    bool seen;
} AnalyzerSurveyBin;

typedef struct {
    uint32_t centre_hz;
    uint32_t rbw_hz;
    AnalyzerSurveyBin bins[ANALYZER_SURVEY_BINS];

    float floor_dbm;
    float peak_dbm;
    int32_t peak_offset_hz;

    uint32_t width_6db_hz;
    uint32_t width_20db_hz;
    uint32_t occupied_bw_hz;
    int32_t edge_lo_hz;
    int32_t edge_hi_hz;
    bool width_resolved;

    bool twin_lobe;
    uint32_t lobe_split_hz;
    uint32_t lobe_rejected_hz;

    uint16_t bins_over_floor;
    bool valid;
} AnalyzerSurvey;

int32_t analyzer_survey_bin_offset_hz(uint16_t bin);

int8_t analyzer_survey_bin_freqoff(uint16_t bin);

void analyzer_survey_reset(AnalyzerSurvey* survey, uint32_t centre_hz, uint32_t rbw_hz);

void analyzer_survey_accumulate(AnalyzerSurvey* survey, uint16_t bin, int8_t rssi_raw);

void analyzer_survey_finish(AnalyzerSurvey* survey, float floor_dbm);

uint32_t analyzer_survey_implied_deviation(const AnalyzerSurvey* survey, uint32_t rate_baud);

static inline float analyzer_survey_raw_to_dbm(int8_t raw) {
    return (float)raw / 2.0f - 74.0f;
}

#ifdef __cplusplus
}
#endif
