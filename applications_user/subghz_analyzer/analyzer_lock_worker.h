#pragma once

#include "analyzer_band.h"
#include "analyzer_env.h"
#include "analyzer_session.h"
#include "analyzer_survey.h"

#include <lib/subghz/devices/devices.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnalyzerLockWorker AnalyzerLockWorker;

typedef struct {
    uint32_t frequency;
    AnalyzerZoom zoom;

    AnalyzerSurvey survey;
    uint16_t survey_passes;
    AnalyzerBand band;

    AnalyzerEnvVote vote;
    uint8_t confidence;
    float floor_dbm;
    float peak_dbm;
    int8_t level_raw;
    uint8_t atten;
    bool overloaded;

    uint16_t sweeps;
} AnalyzerLockState;

AnalyzerLockWorker* analyzer_lock_worker_alloc(const SubGhzDevice* device);
void analyzer_lock_worker_free(AnalyzerLockWorker* instance);

AnalyzerEnv* analyzer_lock_worker_env(AnalyzerLockWorker* instance);

void analyzer_lock_worker_start(
    AnalyzerLockWorker* instance,
    uint32_t frequency,
    AnalyzerZoom zoom);
void analyzer_lock_worker_stop(AnalyzerLockWorker* instance);
bool analyzer_lock_worker_is_running(const AnalyzerLockWorker* instance);

void analyzer_lock_worker_retune(AnalyzerLockWorker* instance, uint32_t frequency);
void analyzer_lock_worker_set_zoom(AnalyzerLockWorker* instance, AnalyzerZoom zoom);

void analyzer_lock_worker_get_state(AnalyzerLockWorker* instance, AnalyzerLockState* out);

void analyzer_lock_worker_fill_session(AnalyzerLockWorker* instance, AnalyzerSession* session);

#ifdef __cplusplus
}
#endif
