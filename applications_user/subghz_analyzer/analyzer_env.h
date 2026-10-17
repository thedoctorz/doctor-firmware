#pragma once

#include <lib/subghz/devices/devices.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_ENV_SAMPLES   128u
#define ANALYZER_ENV_PERIOD_US 100u

#define ANALYZER_ENV_SNR_DB 10.0f

#define ANALYZER_ENV_FLOOR_INVALID INT8_MIN

typedef enum {
    AnalyzerEnvVoteNone,
    AnalyzerEnvVoteAm,
    AnalyzerEnvVoteFm,
} AnalyzerEnvVote;

typedef struct {
    int8_t p10;
    int8_t p50;
    int8_t p90;
    int8_t mid_level;
    float p90_dbm;
    float carrier_db;
    float depth_db;
    uint8_t interior_off_runs;
    bool off_runs_reach_floor;

    bool overloaded;

    AnalyzerEnvVote vote;
    uint8_t weight;
} AnalyzerEnvWindow;

typedef struct AnalyzerEnv AnalyzerEnv;

AnalyzerEnv* analyzer_env_alloc(void);

void analyzer_env_free(AnalyzerEnv* instance);

void analyzer_env_reset(AnalyzerEnv* instance);

void analyzer_env_learn_floor(AnalyzerEnv* instance, const SubGhzDevice* device, uint8_t windows);

float analyzer_env_floor_dbm(const AnalyzerEnv* instance);

int8_t analyzer_env_get_floor_raw(const AnalyzerEnv* instance);

void analyzer_env_set_floor_raw(AnalyzerEnv* instance, int8_t raw);

int8_t analyzer_env_gate_raw(const AnalyzerEnv* instance);

bool analyzer_env_probe(AnalyzerEnv* instance, const SubGhzDevice* device, AnalyzerEnvWindow* out);

AnalyzerEnvVote analyzer_env_verdict(const AnalyzerEnv* instance, uint8_t* confidence);

uint16_t analyzer_env_signal_windows(const AnalyzerEnv* instance);

float analyzer_env_peak_dbm(const AnalyzerEnv* instance);

int8_t analyzer_env_level_raw(const SubGhzDevice* device);

static inline float analyzer_env_raw_to_dbm(int8_t raw) {
    return (float)raw / 2.0f - 74.0f;
}

#ifdef __cplusplus
}
#endif
