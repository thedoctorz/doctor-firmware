#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_BW_COUNT   16
#define ANALYZER_DEV_COUNT  64
#define ANALYZER_RATE_COUNT 64

#define ANALYZER_AGC_AUTO       0xFFu
#define ANALYZER_MODEL_BLOB_MAX 64

typedef enum {
    AnalyzerMod2Fsk = 0,
    AnalyzerModGfsk = 1,
    AnalyzerModOok = 2,
    AnalyzerModCount,
} AnalyzerMod;

typedef enum {
    AnalyzerParamFrequency,
    AnalyzerParamModulation,
    AnalyzerParamBandwidth,
    AnalyzerParamDeviation,
    AnalyzerParamRate,
    AnalyzerParamRxAtten,
} AnalyzerParam;

typedef struct {
    uint32_t frequency;
    int8_t freq_offset;
    uint8_t bw_idx;
    uint8_t dev_idx;
    uint8_t rate_idx;
    bool rate_exact;
    uint8_t rate_e;
    uint8_t rate_m;
    uint8_t if_idx;
    uint8_t mod;
    uint8_t close_in_rx;
    uint8_t max_lna_gain;
    uint8_t magn_target;
    bool dcfilt_off;
    bool cs_gate;
} AnalyzerModel;

void analyzer_model_init(AnalyzerModel* model);

size_t analyzer_model_build(const AnalyzerModel* model, uint8_t* out, size_t cap);

void analyzer_model_format(const AnalyzerModel* model, AnalyzerParam param, char* out, size_t cap);

uint8_t analyzer_model_agc_target_alternative(const AnalyzerModel* model);

uint32_t analyzer_model_bandwidth_hz(const AnalyzerModel* model);
uint32_t analyzer_model_deviation_hz(const AnalyzerModel* model);
uint32_t analyzer_model_rate_baud(const AnalyzerModel* model);

uint8_t analyzer_model_rate_index_for_baud(uint32_t baud);

uint8_t analyzer_model_bw_index_for_hz(uint32_t hz);
uint8_t analyzer_model_bw_index_atleast(uint32_t hz);

uint8_t analyzer_model_dev_index_for_hz(uint32_t hz);

void analyzer_model_set_rate_baud(AnalyzerModel* model, uint32_t baud);

uint32_t analyzer_model_rate_from_regs(uint8_t e, uint8_t m);

uint32_t analyzer_model_bandwidth_from_reg(uint8_t chanbw_nibble);

uint32_t analyzer_model_deviation_from_reg(uint8_t deviatn);

bool analyzer_model_frequency_valid(uint32_t frequency);

uint32_t analyzer_model_settle_ms(uint32_t bw_hz);

#define ANALYZER_CC1101_DEV_MAX_HZ 380859u
#define ANALYZER_CC1101_BW_MAX_HZ  812500u

typedef enum {
    AnalyzerReachOk,
    AnalyzerReachTight,
    AnalyzerReachDevTooWide,
    AnalyzerReachBwTooWide,
} AnalyzerReach;

AnalyzerReach analyzer_model_reach(uint32_t deviation_hz, uint32_t baud, uint32_t* required_bw_hz);

const char* analyzer_model_reach_name(AnalyzerReach reach);

#ifdef __cplusplus
}
#endif
