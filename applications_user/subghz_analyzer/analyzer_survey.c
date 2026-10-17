#include "analyzer_survey.h"

#include <furi.h>
#include <math.h>
#include <string.h>

#define ANALYZER_SURVEY_RAW_PER_DB 2.0f

#define ANALYZER_SURVEY_UNSET INT8_MIN

int32_t analyzer_survey_bin_offset_hz(uint16_t bin) {
    const int32_t lsb =
        ((int32_t)bin - (int32_t)ANALYZER_SURVEY_CENTRE_BIN) * ANALYZER_SURVEY_STRIDE;
    return (int32_t)(((int64_t)lsb * ANALYZER_SURVEY_LSB_MHZ) / 1000);
}

int8_t analyzer_survey_bin_freqoff(uint16_t bin) {
    const int32_t lsb =
        ((int32_t)bin - (int32_t)ANALYZER_SURVEY_CENTRE_BIN) * ANALYZER_SURVEY_STRIDE;
    if(lsb > 127) return 127;
    if(lsb < -128) return -128;
    return (int8_t)lsb;
}

void analyzer_survey_reset(AnalyzerSurvey* survey, uint32_t centre_hz, uint32_t rbw_hz) {
    furi_check(survey);
    memset(survey, 0, sizeof(*survey));
    survey->centre_hz = centre_hz;
    survey->rbw_hz = rbw_hz;
    for(uint16_t i = 0; i < ANALYZER_SURVEY_BINS; i++) {
        survey->bins[i].rssi_raw = ANALYZER_SURVEY_UNSET;
        survey->bins[i].seen = false;
    }
}

void analyzer_survey_accumulate(AnalyzerSurvey* survey, uint16_t bin, int8_t rssi_raw) {
    furi_check(survey);
    if(bin >= ANALYZER_SURVEY_BINS) return;

    AnalyzerSurveyBin* b = &survey->bins[bin];
    if(!b->seen || rssi_raw > b->rssi_raw) b->rssi_raw = rssi_raw;
    b->seen = true;
}

static bool analyzer_survey_shoulders(
    const AnalyzerSurvey* survey,
    uint16_t peak_bin,
    float drop_db,
    int32_t* lo_hz,
    int32_t* hi_hz) {
    const int32_t threshold =
        (int32_t)survey->bins[peak_bin].rssi_raw - (int32_t)(drop_db * ANALYZER_SURVEY_RAW_PER_DB);

    int32_t lo = -1;
    for(int32_t i = (int32_t)peak_bin; i >= 0; i--) {
        if(!survey->bins[i].seen || (int32_t)survey->bins[i].rssi_raw < threshold) {
            lo = i;
            break;
        }
    }

    int32_t hi = -1;
    for(int32_t i = (int32_t)peak_bin; i < (int32_t)ANALYZER_SURVEY_BINS; i++) {
        if(!survey->bins[i].seen || (int32_t)survey->bins[i].rssi_raw < threshold) {
            hi = i;
            break;
        }
    }

    if(lo < 0 || hi < 0) return false;

    *lo_hz = analyzer_survey_bin_offset_hz((uint16_t)lo);
    *hi_hz = analyzer_survey_bin_offset_hz((uint16_t)hi);
    return true;
}

static void analyzer_survey_detect_lobes(AnalyzerSurvey* survey) {
    const int32_t floor_raw = (int32_t)((survey->floor_dbm + 74.0f) * ANALYZER_SURVEY_RAW_PER_DB);
    const int32_t gate =
        floor_raw + (int32_t)(ANALYZER_SURVEY_SNR_GATE_DB * ANALYZER_SURVEY_RAW_PER_DB);

    const int32_t bin_spacing_hz = analyzer_survey_bin_offset_hz(ANALYZER_SURVEY_CENTRE_BIN + 1u) -
                                   analyzer_survey_bin_offset_hz(ANALYZER_SURVEY_CENTRE_BIN);
    const uint32_t min_sep_hz = survey->rbw_hz * ANALYZER_SURVEY_LOBE_MIN_SEP_FACTOR;
    const int32_t min_sep_bins =
        (bin_spacing_hz > 0) ?
            (int32_t)((min_sep_hz + (uint32_t)bin_spacing_hz - 1u) / (uint32_t)bin_spacing_hz) :
            (int32_t)ANALYZER_SURVEY_BINS;

    int32_t best_a = -1, best_b = -1;
    for(int32_t i = 1; i + 1 < (int32_t)ANALYZER_SURVEY_BINS; i++) {
        if(!survey->bins[i].seen) continue;
        const int32_t v = survey->bins[i].rssi_raw;
        if(v < gate) continue;
        if(v < (int32_t)survey->bins[i - 1].rssi_raw) continue;
        if(v < (int32_t)survey->bins[i + 1].rssi_raw) continue;

        if(best_a < 0) {
            best_a = i;
        } else if(i - best_a >= min_sep_bins) {
            int32_t valley = INT32_MAX;
            for(int32_t k = best_a + 1; k < i; k++) {
                if(survey->bins[k].seen && survey->bins[k].rssi_raw < valley) {
                    valley = survey->bins[k].rssi_raw;
                }
            }
            const int32_t lower =
                (survey->bins[best_a].rssi_raw < v) ? survey->bins[best_a].rssi_raw : v;
            if(valley != INT32_MAX && (lower - valley) >= (int32_t)(ANALYZER_SURVEY_LOBE_DIP_DB *
                                                                    ANALYZER_SURVEY_RAW_PER_DB)) {
                best_b = i;
                break;
            }
            if(v > survey->bins[best_a].rssi_raw) best_a = i;
        }
    }

    survey->twin_lobe = false;
    survey->lobe_split_hz = 0;
    survey->lobe_rejected_hz = 0;

    if(best_a < 0 || best_b <= best_a) return;

    const int32_t a = analyzer_survey_bin_offset_hz((uint16_t)best_a);
    const int32_t b = analyzer_survey_bin_offset_hz((uint16_t)best_b);
    const uint32_t split = (uint32_t)(b - a);

    const uint32_t width = survey->width_20db_hz ? survey->width_20db_hz : survey->width_6db_hz;
    const uint32_t allow = width + (width * ANALYZER_SURVEY_LOBE_WIDTH_SLACK_PCT) / 100u;
    if(!survey->width_resolved || split > allow) {
        survey->lobe_rejected_hz = split;
        return;
    }

    survey->twin_lobe = true;
    survey->lobe_split_hz = split;
}

void analyzer_survey_finish(AnalyzerSurvey* survey, float floor_dbm) {
    furi_check(survey);

    survey->floor_dbm = floor_dbm;
    survey->valid = false;
    survey->bins_over_floor = 0;

    int32_t peak_bin = -1;
    for(uint16_t i = 0; i < ANALYZER_SURVEY_BINS; i++) {
        if(!survey->bins[i].seen) continue;
        if(peak_bin < 0 || survey->bins[i].rssi_raw > survey->bins[peak_bin].rssi_raw) {
            peak_bin = i;
        }
        if(analyzer_survey_raw_to_dbm(survey->bins[i].rssi_raw) >=
           floor_dbm + ANALYZER_SURVEY_SNR_GATE_DB) {
            survey->bins_over_floor++;
        }
    }
    if(peak_bin < 0) return;

    survey->peak_dbm = analyzer_survey_raw_to_dbm(survey->bins[peak_bin].rssi_raw);
    survey->peak_offset_hz = analyzer_survey_bin_offset_hz((uint16_t)peak_bin);

    const float headroom_db = survey->peak_dbm - floor_dbm;

    int32_t lo = 0, hi = 0;
    survey->width_6db_hz = 0;
    survey->width_20db_hz = 0;
    survey->edge_lo_hz = 0;
    survey->edge_hi_hz = 0;

    if(headroom_db >= 6.0f &&
       analyzer_survey_shoulders(survey, (uint16_t)peak_bin, 6.0f, &lo, &hi)) {
        survey->width_6db_hz = (uint32_t)(hi - lo);
        survey->edge_lo_hz = lo;
        survey->edge_hi_hz = hi;
    }
    if(headroom_db >= 20.0f &&
       analyzer_survey_shoulders(survey, (uint16_t)peak_bin, 20.0f, &lo, &hi)) {
        survey->width_20db_hz = (uint32_t)(hi - lo);
        survey->edge_lo_hz = lo;
        survey->edge_hi_hz = hi;
    }

    survey->width_resolved = false;
    if(survey->width_6db_hz && survey->width_6db_hz < survey->rbw_hz) survey->width_6db_hz = 0;
    if(survey->width_20db_hz && survey->width_20db_hz < survey->rbw_hz) survey->width_20db_hz = 0;
    if(survey->width_6db_hz == 0 && survey->width_20db_hz == 0) {
        survey->edge_lo_hz = 0;
        survey->edge_hi_hz = 0;
    } else {
        survey->width_resolved = true;
    }

    const uint32_t measured = survey->width_20db_hz ? survey->width_20db_hz : survey->width_6db_hz;
    if(measured > 0) {
        const float m = (float)measured;
        const float r = (float)survey->rbw_hz;
        const float corrected = (m > r) ? sqrtf(m * m - r * r) : 0.0f;
        survey->occupied_bw_hz = (uint32_t)corrected;
    } else {
        survey->occupied_bw_hz = 0;
    }

    analyzer_survey_detect_lobes(survey);

    survey->valid = true;
}

uint32_t analyzer_survey_implied_deviation(const AnalyzerSurvey* survey, uint32_t rate_baud) {
    furi_check(survey);
    if(!survey->valid) return 0;

    if(survey->twin_lobe && survey->lobe_split_hz > 0) return survey->lobe_split_hz / 2u;

    if(survey->occupied_bw_hz == 0) return 0;
    if(survey->occupied_bw_hz <= rate_baud) return 0;

    return (survey->occupied_bw_hz - rate_baud) / 2u;
}
