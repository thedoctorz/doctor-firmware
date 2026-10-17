#include "analyzer_env.h"

#include <furi.h>
#include <string.h>

#define ANALYZER_ENV_AM_CARRIER_DB 8.0f
#define ANALYZER_ENV_FM_CARRIER_DB 3.0f
#define ANALYZER_ENV_FLOOR_NEAR_DB 6.0f
#define ANALYZER_ENV_FLAT_DB       6.0f

#define ANALYZER_ENV_OVERLOAD_DBM  (-35.0f)
#define ANALYZER_ENV_MIN_DECISIVE  6u
#define ANALYZER_ENV_MIN_AGREE_PCT 80u

#define ANALYZER_ENV_FLOOR_UNSET       INT8_MIN
#define ANALYZER_ENV_FLOOR_CEILING_DBM (-85.0f)

struct AnalyzerEnv {
    uint8_t samples[ANALYZER_ENV_SAMPLES];
    uint16_t hist[256];

    int8_t floor_raw;
    bool floor_valid;
    uint32_t period_us;

    uint16_t windows_signal;
    uint16_t windows_decisive;
    uint32_t weight_am;
    uint32_t weight_fm;
    float best_am_carrier_db;
    float best_fm_carrier_db;
    float peak_dbm;
};

AnalyzerEnv* analyzer_env_alloc(void) {
    AnalyzerEnv* instance = malloc(sizeof(AnalyzerEnv));
    memset(instance, 0, sizeof(AnalyzerEnv));
    analyzer_env_reset(instance);
    return instance;
}

void analyzer_env_free(AnalyzerEnv* instance) {
    furi_check(instance);
    free(instance);
}

void analyzer_env_reset(AnalyzerEnv* instance) {
    furi_check(instance);
    instance->floor_raw = ANALYZER_ENV_FLOOR_UNSET;
    instance->floor_valid = false;
    instance->period_us = ANALYZER_ENV_PERIOD_US;
    instance->windows_signal = 0;
    instance->windows_decisive = 0;
    instance->weight_am = 0;
    instance->weight_fm = 0;
    instance->best_am_carrier_db = 0.0f;
    instance->best_fm_carrier_db = 1000.0f;
    instance->peak_dbm = -200.0f;
}

int8_t analyzer_env_level_raw(const SubGhzDevice* device) {
    uint8_t raw = 0;
    subghz_devices_rssi_burst(device, &raw, 1, 0);
    return (int8_t)raw;
}

static int8_t analyzer_env_percentile(const uint16_t* hist, uint32_t total, uint32_t permille) {
    const uint32_t want = (total * permille) / 1000u;
    uint32_t seen = 0;
    for(int32_t v = -128; v <= 127; v++) {
        seen += hist[(uint8_t)(int8_t)v];
        if(seen > want) return (int8_t)v;
    }
    return 127;
}

static bool analyzer_env_level_at(const uint8_t* s, size_t count, size_t i, int8_t mid) {
    const bool cur = (int8_t)s[i] > mid;
    if(i == 0 || i + 1 >= count) return cur;
    const bool prev = (int8_t)s[i - 1] > mid;
    const bool next = (int8_t)s[i + 1] > mid;
    return (prev && cur) || (cur && next) || (prev && next);
}

static float analyzer_env_spread_db(
    AnalyzerEnv* instance,
    const uint8_t* samples,
    size_t count,
    int8_t mid,
    bool above,
    uint32_t* population) {
    memset(instance->hist, 0, sizeof(instance->hist));
    uint32_t n = 0;
    for(size_t i = 0; i < count; i++) {
        const bool is_above = (int8_t)samples[i] > mid;
        if(is_above != above) continue;
        instance->hist[samples[i]]++;
        n++;
    }
    if(population) *population = n;
    if(n == 0) return 0.0f;

    const int8_t lo = analyzer_env_percentile(instance->hist, n, 100);
    const int8_t hi = analyzer_env_percentile(instance->hist, n, 900);
    return ((float)hi - (float)lo) / 2.0f;
}

static void analyzer_env_runs(AnalyzerEnv* instance, size_t count, AnalyzerEnvWindow* out) {
    const int32_t floor_near =
        (int32_t)instance->floor_raw + (int32_t)(ANALYZER_ENV_FLOOR_NEAR_DB * 2.0f);

    bool prev = analyzer_env_level_at(instance->samples, count, 0, out->mid_level);
    size_t run_start = 0;
    int8_t run_min = (int8_t)instance->samples[0];

    for(size_t i = 1; i <= count; i++) {
        const bool cur = (i < count) ?
                             analyzer_env_level_at(instance->samples, count, i, out->mid_level) :
                             !prev;
        if(i < count && (int8_t)instance->samples[i] < run_min)
            run_min = (int8_t)instance->samples[i];

        if(cur == prev && i < count) continue;

        const bool touches_edge = (run_start == 0) || (i == count);
        if(!prev && !touches_edge) {
            if(out->interior_off_runs < UINT8_MAX) out->interior_off_runs++;
            if((int32_t)run_min <= floor_near) out->off_runs_reach_floor = true;
        }

        prev = cur;
        run_start = i;
        run_min = (i < count) ? (int8_t)instance->samples[i] : 0;
    }
}

static void analyzer_env_eval(AnalyzerEnv* instance, size_t count, AnalyzerEnvWindow* out) {
    memset(out, 0, sizeof(*out));
    if(count == 0) return;

    memset(instance->hist, 0, sizeof(instance->hist));
    for(size_t i = 0; i < count; i++)
        instance->hist[instance->samples[i]]++;

    out->p10 = analyzer_env_percentile(instance->hist, count, 100);
    out->p50 = analyzer_env_percentile(instance->hist, count, 500);
    out->p90 = analyzer_env_percentile(instance->hist, count, 900);
    out->p90_dbm = analyzer_env_raw_to_dbm(out->p90);
    out->depth_db = ((float)out->p90 - (float)out->p10) / 2.0f;
    out->mid_level = (int8_t)(((int32_t)out->p10 + (int32_t)out->p90) / 2);

    out->carrier_db =
        analyzer_env_spread_db(instance, instance->samples, count, out->mid_level, true, NULL);

    analyzer_env_runs(instance, count, out);

    AnalyzerEnvVote spread = AnalyzerEnvVoteNone;
    if(out->carrier_db >= ANALYZER_ENV_AM_CARRIER_DB) {
        spread = AnalyzerEnvVoteAm;
    } else if(out->carrier_db <= ANALYZER_ENV_FM_CARRIER_DB && out->depth_db < ANALYZER_ENV_FLAT_DB) {
        spread = AnalyzerEnvVoteFm;
    }

    const float floor_dbm = analyzer_env_floor_dbm(instance);
    AnalyzerEnvVote structure = AnalyzerEnvVoteNone;
    if(out->interior_off_runs >= 2 && out->off_runs_reach_floor) {
        structure = AnalyzerEnvVoteAm;
    } else if(out->depth_db < ANALYZER_ENV_FLAT_DB && out->p90_dbm >= floor_dbm + ANALYZER_ENV_SNR_DB) {
        structure = AnalyzerEnvVoteFm;
    }

    out->overloaded = (out->p90_dbm > ANALYZER_ENV_OVERLOAD_DBM);

    if(out->overloaded) {
        out->vote = AnalyzerEnvVoteNone;
        out->weight = 0;
    } else if(structure == AnalyzerEnvVoteAm) {
        out->vote = AnalyzerEnvVoteAm;
        out->weight = (spread == AnalyzerEnvVoteAm) ? 2 : 1;
    } else if(spread != AnalyzerEnvVoteNone && spread == structure) {
        out->vote = spread;
        out->weight = 2;
    } else if(spread != AnalyzerEnvVoteNone && structure == AnalyzerEnvVoteNone) {
        out->vote = spread;
        out->weight = 1;
    } else if(structure != AnalyzerEnvVoteNone && spread == AnalyzerEnvVoteNone) {
        out->vote = structure;
        out->weight = 1;
    } else {
        out->vote = AnalyzerEnvVoteNone;
        out->weight = 0;
    }
}

static size_t analyzer_env_capture(AnalyzerEnv* instance, const SubGhzDevice* device) {
    const uint32_t span_us = subghz_devices_rssi_burst(
        device, instance->samples, ANALYZER_ENV_SAMPLES, ANALYZER_ENV_PERIOD_US);
    if(span_us == 0) return 0;

    instance->period_us = span_us / ANALYZER_ENV_SAMPLES;
    if(instance->period_us == 0) instance->period_us = ANALYZER_ENV_PERIOD_US;
    return ANALYZER_ENV_SAMPLES;
}

void analyzer_env_learn_floor(AnalyzerEnv* instance, const SubGhzDevice* device, uint8_t windows) {
    furi_check(instance);
    if(windows == 0) windows = 1;

    analyzer_env_capture(instance, device);

    int8_t lowest = INT8_MAX;
    for(uint8_t w = 0; w < windows; w++) {
        const size_t count = analyzer_env_capture(instance, device);
        if(count == 0) break;

        memset(instance->hist, 0, sizeof(instance->hist));
        for(size_t i = 0; i < count; i++)
            instance->hist[instance->samples[i]]++;

        const int8_t p10 = analyzer_env_percentile(instance->hist, count, 100);
        if(p10 < lowest) lowest = p10;
    }

    if(lowest != INT8_MAX) {
        const int8_t ceiling = (int8_t)((ANALYZER_ENV_FLOOR_CEILING_DBM + 74.0f) * 2.0f);
        instance->floor_raw = (lowest > ceiling) ? ceiling : lowest;
        instance->floor_valid = true;
    }
}

float analyzer_env_floor_dbm(const AnalyzerEnv* instance) {
    furi_check(instance);
    if(!instance->floor_valid) return -110.0f;
    return analyzer_env_raw_to_dbm(instance->floor_raw);
}

int8_t analyzer_env_get_floor_raw(const AnalyzerEnv* instance) {
    furi_check(instance);
    return instance->floor_valid ? instance->floor_raw : ANALYZER_ENV_FLOOR_INVALID;
}

void analyzer_env_set_floor_raw(AnalyzerEnv* instance, int8_t raw) {
    furi_check(instance);
    if(raw == ANALYZER_ENV_FLOOR_INVALID) return;
    instance->floor_raw = raw;
    instance->floor_valid = true;
}

int8_t analyzer_env_gate_raw(const AnalyzerEnv* instance) {
    furi_check(instance);
    const int32_t gate = (int32_t)instance->floor_raw + (int32_t)(ANALYZER_ENV_SNR_DB * 2.0f);
    return (int8_t)(gate > 127 ? 127 : gate);
}

bool analyzer_env_probe(AnalyzerEnv* instance, const SubGhzDevice* device, AnalyzerEnvWindow* out) {
    furi_check(instance);

    const size_t count = analyzer_env_capture(instance, device);
    if(count == 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    analyzer_env_eval(instance, count, out);

    const float floor_dbm = analyzer_env_floor_dbm(instance);
    if(out->p90_dbm < floor_dbm + ANALYZER_ENV_SNR_DB) {
        const float p50_dbm = analyzer_env_raw_to_dbm(out->p50);
        if(p50_dbm < floor_dbm) {
            instance->floor_raw = (int8_t)((instance->floor_raw * 3 + out->p50) / 4);
        }
        return false;
    }

    instance->windows_signal++;
    if(out->p90_dbm > instance->peak_dbm) instance->peak_dbm = out->p90_dbm;

    if(out->weight == 0) return true;

    instance->windows_decisive++;
    if(out->vote == AnalyzerEnvVoteAm) {
        instance->weight_am += out->weight;
        if(out->carrier_db > instance->best_am_carrier_db) {
            instance->best_am_carrier_db = out->carrier_db;
        }
    } else {
        instance->weight_fm += out->weight;
        if(out->carrier_db < instance->best_fm_carrier_db) {
            instance->best_fm_carrier_db = out->carrier_db;
        }
    }
    return true;
}

AnalyzerEnvVote analyzer_env_verdict(const AnalyzerEnv* instance, uint8_t* confidence) {
    furi_check(instance);
    if(confidence) *confidence = 0;

    if(instance->windows_decisive < ANALYZER_ENV_MIN_DECISIVE) return AnalyzerEnvVoteNone;

    const uint32_t total = instance->weight_am + instance->weight_fm;
    if(total == 0) return AnalyzerEnvVoteNone;

    const bool am = instance->weight_am > instance->weight_fm;
    const uint32_t winner = am ? instance->weight_am : instance->weight_fm;
    const uint32_t agree_pct = (winner * 100u) / total;
    if(agree_pct < ANALYZER_ENV_MIN_AGREE_PCT) return AnalyzerEnvVoteNone;

    if(confidence) {
        float conf = (float)agree_pct / 100.0f;

        const float count_factor =
            (instance->windows_decisive >= ANALYZER_ENV_MIN_DECISIVE * 2) ?
                1.0f :
                (float)instance->windows_decisive / (float)(ANALYZER_ENV_MIN_DECISIVE * 2);
        conf *= 0.6f + 0.4f * count_factor;

        const float margin = am ? (instance->best_am_carrier_db - ANALYZER_ENV_AM_CARRIER_DB) :
                                  (ANALYZER_ENV_FM_CARRIER_DB - instance->best_fm_carrier_db);
        const float margin_factor = (margin >= 4.0f) ? 1.0f : (0.7f + 0.3f * (margin / 4.0f));
        conf *= (margin_factor > 0.7f) ? margin_factor : 0.7f;

        int32_t pct = (int32_t)(conf * 100.0f + 0.5f);
        if(pct < 0) pct = 0;
        if(pct > 100) pct = 100;
        *confidence = (uint8_t)pct;
    }

    return am ? AnalyzerEnvVoteAm : AnalyzerEnvVoteFm;
}

uint16_t analyzer_env_signal_windows(const AnalyzerEnv* instance) {
    furi_check(instance);
    return instance->windows_signal;
}

float analyzer_env_peak_dbm(const AnalyzerEnv* instance) {
    furi_check(instance);
    return instance->peak_dbm;
}
