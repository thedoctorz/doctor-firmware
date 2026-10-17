#pragma once

#include "analyzer_env.h"
#include "analyzer_model.h"
#include "analyzer_survey.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AnalyzerPipelineHunt,
    AnalyzerPipelineSet,
    AnalyzerPipelineLock,
    AnalyzerPipelineTune,
    AnalyzerPipelineResult,
    AnalyzerPipelineCount,
} AnalyzerPipelineStage;

const char* analyzer_pipeline_name(AnalyzerPipelineStage stage);

typedef struct {
    uint32_t frequency;
    float hunt_rssi_dbm;
    bool frequency_manual;

    bool lock_valid;
    AnalyzerEnvVote env_vote;
    uint8_t env_confidence;
    uint8_t atten;
    int8_t floor_raw;
    float floor_dbm;
    float peak_dbm;

    AnalyzerSurvey survey;
    uint16_t survey_passes;

    uint8_t seed_mod;
    uint32_t seed_bandwidth_hz;
    uint32_t seed_deviation_hz;
    uint32_t seed_rate_baud;

    bool tune_valid;
} AnalyzerSession;

void analyzer_session_init(AnalyzerSession* session);

void analyzer_session_clear_lock(AnalyzerSession* session);

void analyzer_session_derive_seed(AnalyzerSession* session);

#ifdef __cplusplus
}
#endif
