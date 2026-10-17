#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALYZER_SCORE_BINS   48
#define ANALYZER_SCORE_MIN_US 2u
#define ANALYZER_SCORE_MAX_US 50000u

#define ANALYZER_SCORE_RASTER_MIN 3u

#define ANALYZER_SCORE_FLOOR_MIN 2u

#define ANALYZER_SCORE_UNIT_STEP   1.06f
#define ANALYZER_SCORE_UNIT_MARGIN 15u
#define ANALYZER_SCORE_UNIT_COVER  0.60f
#define ANALYZER_SCORE_UNIT_MIN_Q  35u

#define ANALYZER_SCORE_UNIT_CANDIDATES 192u

#define ANALYZER_SCORE_QUANT_MIN 40u

#define ANALYZER_SCORE_AGREE 0.20f

#define ANALYZER_SCORE_MIN_EDGES 20u

#define ANALYZER_SCORE_MAX_EDGES 8000u

#define ANALYZER_SCORE_MAX_SYMBOLS 32u

#define ANALYZER_SCORE_TOLERANCE 0.20f

#define ANALYZER_SCORE_CHANCE (2.0f * ANALYZER_SCORE_TOLERANCE)

#define ANALYZER_SCORE_REP_POINTS 30u

typedef enum {
    AnalyzerScoreVerdictNoSignal,
    AnalyzerScoreVerdictNoise,
    AnalyzerScoreVerdictRaster,
    AnalyzerScoreVerdictFloor,
    AnalyzerScoreVerdictScored,
} AnalyzerScoreVerdict;

#define ANALYZER_SCORE_MANCHESTER_PCT 2u

#define ANALYZER_SCORE_WIDE_PCT 5u

typedef enum {
    AnalyzerEncodingUnknown,
    AnalyzerEncodingManchester,
    AnalyzerEncodingPulseWidth,
    AnalyzerEncodingWide,
} AnalyzerEncoding;

const char* analyzer_encoding_name(AnalyzerEncoding encoding);

typedef struct {
    AnalyzerScoreVerdict verdict;
    uint8_t score;
    uint8_t quantization;
    bool repetition;
    uint32_t base_symbol_us;
    uint32_t edges;
    uint32_t considered;
    uint32_t raster_us;
    uint32_t raster_ratio_pct;
    uint8_t duty_pct;

    uint8_t encoding;
    uint8_t run1_pct;
    uint8_t run2_pct;
    uint8_t run3_pct;
    uint8_t manchester_viol_pct;
} AnalyzerScoreResult;

typedef struct AnalyzerScore AnalyzerScore;

AnalyzerScore* analyzer_score_alloc(void);

void analyzer_score_free(AnalyzerScore* instance);

void analyzer_score_reset(AnalyzerScore* instance);

void analyzer_score_set_raster(AnalyzerScore* instance, uint32_t raster_us);

void analyzer_score_set_floor(AnalyzerScore* instance, uint32_t floor_us);

uint32_t analyzer_score_raster_us(uint32_t baud);

void analyzer_score_feed(AnalyzerScore* instance, bool level, uint32_t duration);

void analyzer_score_eval(AnalyzerScore* instance, AnalyzerScoreResult* out);

void analyzer_score_format_clusters(const AnalyzerScore* instance, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
