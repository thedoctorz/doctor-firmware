#pragma once

#include "analyzer_env.h"
#include "analyzer_log.h"
#include "analyzer_model.h"
#include "analyzer_score.h"
#include "analyzer_session.h"
#include "analyzer_survey.h"

#include <lib/subghz/devices/devices.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_CARRIER_EDGE_MAX 8u

#define ANALYZER_CARRIER_DUTY_PCT  2u
#define ANALYZER_CARRIER_MIN_EDGES 24u

typedef enum {
    AnalyzerStageSeed,
    AnalyzerStageBandwidth,
    AnalyzerStageRate,
    AnalyzerStageDeviation,
    AnalyzerStageGain,
    AnalyzerStageCentre,
    AnalyzerStageConfirm,
    AnalyzerStageDone,
    AnalyzerStageCount,
} AnalyzerStage;

const char* analyzer_stage_name(AnalyzerStage stage);

typedef enum {
    AnalyzerOutcomePending,
    AnalyzerOutcomeTuned,
    AnalyzerOutcomeCarrier,
    AnalyzerOutcomeNoSignal,
    AnalyzerOutcomeUnclear,
} AnalyzerOutcome;

const char* analyzer_outcome_name(AnalyzerOutcome outcome);

typedef struct {
    AnalyzerModel model;
    AnalyzerOutcome outcome;
    uint8_t score;
    uint8_t confidence;
    float peak_dbm;
    float floor_dbm;

    uint8_t encoding;
    uint8_t run1_pct;
    uint8_t run2_pct;
    uint8_t run3_pct;
    uint8_t manchester_viol_pct;

    uint32_t symbol_us;
    uint32_t symbol_baud;
    uint32_t edges;

    uint32_t control_symbol_us;
    bool control_taken;
    bool deaf;

    AnalyzerSurvey survey;

    char log_path[96];
    bool logged;
} AnalyzerResult;

typedef struct {
    AnalyzerStage stage;
    uint8_t progress;
    uint16_t step;
    uint16_t steps;
    AnalyzerModel model;
    int8_t level_raw;
    int8_t gate_raw;
    bool signal;
    uint8_t starved;
    uint16_t eta_s;
    bool waiting;
    uint16_t wait_s;
} AnalyzerProgress;

typedef struct AnalyzerWorker AnalyzerWorker;

AnalyzerWorker* analyzer_worker_alloc(const SubGhzDevice* device);

void analyzer_worker_free(AnalyzerWorker* instance);

bool analyzer_worker_start(AnalyzerWorker* instance, const AnalyzerSession* session);

void analyzer_worker_stop(AnalyzerWorker* instance);

void analyzer_worker_cancel(AnalyzerWorker* instance);

bool analyzer_worker_is_finished(AnalyzerWorker* instance);

void analyzer_worker_get_result(AnalyzerWorker* instance, AnalyzerResult* out);

void analyzer_worker_get_progress(AnalyzerWorker* instance, AnalyzerProgress* out);

#ifdef __cplusplus
}
#endif
