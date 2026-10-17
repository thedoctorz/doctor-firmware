#include "analyzer_score.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANALYZER_SCORE_RING       512u
#define ANALYZER_SCORE_REP_WINDOW 32u

#define ANALYZER_SCORE_MIN_CONSIDERED 16u

#define ANALYZER_SCORE_LEVEL_BIT 0x80000000u
#define ANALYZER_SCORE_DUR_MASK  0x7FFFFFFFu

struct AnalyzerScore {
    uint32_t ring[ANALYZER_SCORE_RING];
    volatile uint32_t head;
    volatile uint32_t edges;
    uint16_t hist[ANALYZER_SCORE_BINS];
    uint8_t symbols[ANALYZER_SCORE_RING];
    uint32_t hashes[ANALYZER_SCORE_RING];
    uint32_t raster_us;
    uint32_t floor_us;
};

AnalyzerScore* analyzer_score_alloc(void) {
    AnalyzerScore* instance = malloc(sizeof(AnalyzerScore));
    analyzer_score_reset(instance);
    return instance;
}

void analyzer_score_free(AnalyzerScore* instance) {
    free(instance);
}

void analyzer_score_reset(AnalyzerScore* instance) {
    instance->head = 0;
    instance->edges = 0;
    memset(instance->hist, 0, sizeof(instance->hist));
}

void analyzer_score_set_raster(AnalyzerScore* instance, uint32_t raster_us) {
    instance->raster_us = raster_us;
}

void analyzer_score_set_floor(AnalyzerScore* instance, uint32_t floor_us) {
    instance->floor_us = floor_us;
}

uint32_t analyzer_score_raster_us(uint32_t baud) {
    if(baud == 0) return 0;
    const uint32_t us = 1000000u / (8u * baud);
    return us ? us : 1u;
}

void analyzer_score_feed(AnalyzerScore* instance, bool level, uint32_t duration) {
    if(duration == 0) return;
    if(duration > ANALYZER_SCORE_DUR_MASK) duration = ANALYZER_SCORE_DUR_MASK;

    instance->ring[instance->head] = duration | (level ? ANALYZER_SCORE_LEVEL_BIT : 0u);
    instance->head = (instance->head + 1) % ANALYZER_SCORE_RING;
    instance->edges++;
}

static uint32_t analyzer_score_bin_floor_us(size_t bin) {
    if(bin >= ANALYZER_SCORE_BINS) bin = ANALYZER_SCORE_BINS - 1;
    const float span = log2f((float)ANALYZER_SCORE_MAX_US / (float)ANALYZER_SCORE_MIN_US);
    const float lo =
        log2f((float)ANALYZER_SCORE_MIN_US) + span * (float)bin / (float)ANALYZER_SCORE_BINS;
    return (uint32_t)exp2f(lo);
}

static size_t analyzer_score_bin_of(uint32_t duration) {
    if(duration < ANALYZER_SCORE_MIN_US || duration > ANALYZER_SCORE_MAX_US) {
        return ANALYZER_SCORE_BINS;
    }
    const float span = log2f((float)ANALYZER_SCORE_MAX_US / (float)ANALYZER_SCORE_MIN_US);
    const float pos = log2f((float)duration / (float)ANALYZER_SCORE_MIN_US) / span;
    size_t bin = (size_t)(pos * (float)ANALYZER_SCORE_BINS);
    if(bin >= ANALYZER_SCORE_BINS) bin = ANALYZER_SCORE_BINS - 1;
    return bin;
}

static uint32_t analyzer_score_depth(uint32_t edges) {
    return (edges < ANALYZER_SCORE_RING) ? edges : ANALYZER_SCORE_RING;
}

static uint32_t analyzer_score_at(const AnalyzerScore* instance, uint32_t depth, uint32_t i) {
    const uint32_t start = (depth < ANALYZER_SCORE_RING) ? 0 : instance->head;
    return instance->ring[(start + i) % ANALYZER_SCORE_RING] & ANALYZER_SCORE_DUR_MASK;
}

static bool analyzer_score_level_at(const AnalyzerScore* instance, uint32_t depth, uint32_t i) {
    const uint32_t start = (depth < ANALYZER_SCORE_RING) ? 0 : instance->head;
    return (instance->ring[(start + i) % ANALYZER_SCORE_RING] & ANALYZER_SCORE_LEVEL_BIT) != 0u;
}

static void analyzer_score_build_histogram(AnalyzerScore* instance, uint32_t depth) {
    memset(instance->hist, 0, sizeof(instance->hist));
    for(uint32_t i = 0; i < depth; i++) {
        const size_t bin = analyzer_score_bin_of(analyzer_score_at(instance, depth, i));
        if(bin < ANALYZER_SCORE_BINS && instance->hist[bin] < UINT16_MAX) {
            instance->hist[bin]++;
        }
    }
}

static uint8_t analyzer_score_fit(
    AnalyzerScore* instance,
    uint32_t depth,
    float unit,
    uint32_t* considered,
    float* refined,
    bool record) {
    if(considered) *considered = 0;
    if(refined) *refined = 0.0f;
    if(unit < (float)ANALYZER_SCORE_MIN_US) return 0;

    const float inv = 1.0f / unit;

    uint32_t seen = 0;
    uint32_t hits = 0;
    float sum = 0.0f;
    uint32_t summed = 0;

    for(uint32_t i = 0; i < depth; i++) {
        const uint32_t d = analyzer_score_at(instance, depth, i);
        const float r = (float)d * inv;

        if(r > (float)ANALYZER_SCORE_MAX_SYMBOLS) continue;

        if(r < 0.5f) {
            if(record && seen < ANALYZER_SCORE_RING) instance->symbols[seen] = 0;
            seen++;
            continue;
        }

        const uint32_t n = (uint32_t)(r + 0.5f);

        if(record && seen < ANALYZER_SCORE_RING) {
            instance->symbols[seen] = (uint8_t)(n > 255u ? 255u : n);
        }
        seen++;

        float err = r - (float)n;
        if(err < 0.0f) err = -err;
        if(err <= ANALYZER_SCORE_TOLERANCE) {
            hits++;
            sum += (float)d / (float)n;
            summed++;
        }
    }

    if(considered) *considered = seen;
    if(seen == 0) return 0;
    if(refined && summed) *refined = sum / (float)summed;

    const float raw = (float)hits / (float)seen;
    float corrected = (raw - ANALYZER_SCORE_CHANCE) / (1.0f - ANALYZER_SCORE_CHANCE);
    if(corrected < 0.0f) corrected = 0.0f;

    const uint32_t q = (uint32_t)(corrected * 100.0f + 0.5f);
    return (uint8_t)(q > 100u ? 100u : q);
}

static uint32_t analyzer_score_fundamental(
    AnalyzerScore* instance,
    uint32_t depth,
    uint8_t* quality,
    uint32_t* considered) {
    *quality = 0;
    *considered = 0;

    uint32_t longest = 0;
    for(uint32_t i = 0; i < depth; i++) {
        const uint32_t d = analyzer_score_at(instance, depth, i);
        if(d > longest) longest = d;
    }
    if(longest > ANALYZER_SCORE_MAX_US) longest = ANALYZER_SCORE_MAX_US;
    if(longest < ANALYZER_SCORE_MIN_US) return 0;

    const uint32_t cover = (uint32_t)((float)depth * ANALYZER_SCORE_UNIT_COVER);

    const float start = (instance->floor_us > ANALYZER_SCORE_MIN_US) ?
                            (float)instance->floor_us :
                            (float)ANALYZER_SCORE_MIN_US;

    uint8_t grid[ANALYZER_SCORE_UNIT_CANDIDATES];
    uint8_t count = 0;
    uint8_t best_q = 0;
    for(float unit = start; unit <= (float)longest && count < ANALYZER_SCORE_UNIT_CANDIDATES;
        unit *= ANALYZER_SCORE_UNIT_STEP) {
        uint32_t seen = 0;
        const uint8_t q = analyzer_score_fit(instance, depth, unit, &seen, NULL, false);
        grid[count++] = (seen >= cover) ? q : 0u;
        if(seen >= cover && q > best_q) best_q = q;
    }
    if(best_q < ANALYZER_SCORE_UNIT_MIN_Q) return 0;

    const uint8_t bar = (best_q > ANALYZER_SCORE_UNIT_MARGIN + ANALYZER_SCORE_UNIT_MIN_Q) ?
                            (uint8_t)(best_q - ANALYZER_SCORE_UNIT_MARGIN) :
                            (uint8_t)ANALYZER_SCORE_UNIT_MIN_Q;

    uint8_t pick = count;
    while(pick > 0u && grid[pick - 1u] < bar)
        pick--;
    if(pick == 0u) return 0;

    float winner = start;
    for(uint8_t i = 1; i < pick; i++)
        winner *= ANALYZER_SCORE_UNIT_STEP;

    float refined = 0.0f;
    analyzer_score_fit(instance, depth, winner, NULL, &refined, false);
    if(refined >= (float)ANALYZER_SCORE_MIN_US) winner = refined;

    *quality = analyzer_score_fit(instance, depth, winner, considered, NULL, true);

    const uint32_t base = (uint32_t)(winner + 0.5f);
    return (base < ANALYZER_SCORE_MIN_US) ? 0 : base;
}

static uint32_t analyzer_score_hash(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for(size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static bool analyzer_score_repeats(AnalyzerScore* instance, uint32_t count) {
    if(count < ANALYZER_SCORE_REP_WINDOW * 2) return false;

    const uint32_t windows = count - ANALYZER_SCORE_REP_WINDOW + 1;
    for(uint32_t i = 0; i < windows; i++) {
        instance->hashes[i] =
            analyzer_score_hash(&instance->symbols[i], ANALYZER_SCORE_REP_WINDOW);
    }

    for(uint32_t i = 0; i < windows; i++) {
        for(uint32_t j = i + ANALYZER_SCORE_REP_WINDOW; j < windows; j++) {
            if(instance->hashes[i] == instance->hashes[j]) return true;
        }
    }
    return false;
}

void analyzer_score_format_clusters(const AnalyzerScore* instance, char* out, size_t cap) {
    if(cap == 0) return;
    out[0] = '\0';

    uint16_t peak = 0;
    for(size_t i = 0; i < ANALYZER_SCORE_BINS; i++) {
        if(instance->hist[i] > peak) peak = instance->hist[i];
    }
    if(peak == 0) return;

    uint16_t floor_pop = peak / 16u;
    if(floor_pop < 2u) floor_pop = 2u;

    size_t used = 0;
    uint8_t shown = 0;
    for(size_t i = 0; i < ANALYZER_SCORE_BINS && shown < 6u; i++) {
        if(instance->hist[i] < floor_pop) continue;

        const int n = snprintf(
            out + used,
            cap - used,
            "%s%lux%u",
            used ? " " : "",
            (unsigned long)analyzer_score_bin_floor_us(i),
            instance->hist[i]);
        if(n <= 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
        shown++;
    }
}

const char* analyzer_encoding_name(AnalyzerEncoding encoding) {
    switch(encoding) {
    case AnalyzerEncodingManchester:
        return "Manchester";
    case AnalyzerEncodingPulseWidth:
        return "pulse-width (te/2te)";
    case AnalyzerEncodingWide:
        return "pulse-position or multi-width";
    default:
        return "unclear";
    }
}

static void analyzer_score_classify(
    AnalyzerScore* instance,
    uint32_t depth,
    uint32_t base,
    AnalyzerScoreResult* out) {
    if(base == 0) return;

    const float inv = 1.0f / (float)base;
    uint32_t runs[3] = {0, 0, 0};
    uint32_t viol[2] = {0, 0};
    uint32_t index = 0;
    uint32_t total = 0;

    for(uint32_t i = 0; i < depth; i++) {
        const float r = (float)analyzer_score_at(instance, depth, i) * inv;
        if(r < 0.5f || r > (float)ANALYZER_SCORE_MAX_SYMBOLS) continue;

        uint32_t n = (uint32_t)(r + 0.5f);
        if(n < 1u) continue;
        total++;
        runs[(n > 3u ? 3u : n) - 1u]++;

        for(uint32_t k = 0; k + 1u < n; k++)
            viol[(index + k) & 1u]++;
        index += n;
    }
    if(total == 0) return;

    out->run1_pct = (uint8_t)((runs[0] * 100u) / total);
    out->run2_pct = (uint8_t)((runs[1] * 100u) / total);
    out->run3_pct = (uint8_t)((runs[2] * 100u) / total);

    const uint32_t pairs = index / 2u;
    if(pairs == 0) return;
    const uint32_t worst = (viol[0] < viol[1]) ? viol[0] : viol[1];
    const uint32_t pct = (worst * 100u) / pairs;
    out->manchester_viol_pct = (uint8_t)(pct > 100u ? 100u : pct);

    if(pct <= ANALYZER_SCORE_MANCHESTER_PCT) {
        out->encoding = AnalyzerEncodingManchester;
    } else if(out->run3_pct >= ANALYZER_SCORE_WIDE_PCT) {
        out->encoding = AnalyzerEncodingWide;
    } else {
        out->encoding = AnalyzerEncodingPulseWidth;
    }
}

void analyzer_score_eval(AnalyzerScore* instance, AnalyzerScoreResult* out) {
    memset(out, 0, sizeof(*out));

    const uint32_t edges = instance->edges;
    const uint32_t depth = analyzer_score_depth(edges);

    out->edges = edges;

    if(edges < ANALYZER_SCORE_MIN_EDGES) {
        out->verdict = AnalyzerScoreVerdictNoSignal;
        return;
    }
    if(edges > ANALYZER_SCORE_MAX_EDGES) {
        out->verdict = AnalyzerScoreVerdictNoise;
        return;
    }

    analyzer_score_build_histogram(instance, depth);

    uint8_t quality = 0;
    uint32_t considered = 0;
    const uint32_t base = analyzer_score_fundamental(instance, depth, &quality, &considered);
    out->base_symbol_us = base;
    if(base == 0) {
        out->verdict = AnalyzerScoreVerdictNoise;
        return;
    }

    uint64_t high_us = 0;
    uint64_t total_us = 0;
    for(uint32_t i = 0; i < depth; i++) {
        const uint32_t d = analyzer_score_at(instance, depth, i);
        total_us += d;
        if(analyzer_score_level_at(instance, depth, i)) high_us += d;
    }
    out->duty_pct = total_us ? (uint8_t)((high_us * 100u) / total_us) : 0u;

    out->raster_us = instance->raster_us;
    if(instance->raster_us > 0) {
        out->raster_ratio_pct = (base * 100u) / instance->raster_us;
        if(base < ANALYZER_SCORE_RASTER_MIN * instance->raster_us) {
            out->verdict = AnalyzerScoreVerdictRaster;
            return;
        }
    }

    if(instance->floor_us > 0 && base < ANALYZER_SCORE_FLOOR_MIN * instance->floor_us) {
        out->verdict = AnalyzerScoreVerdictFloor;
        return;
    }

    out->considered = considered;

    if(considered < ANALYZER_SCORE_MIN_CONSIDERED) {
        out->verdict = AnalyzerScoreVerdictNoise;
        return;
    }

    out->quantization = quality;
    out->repetition = analyzer_score_repeats(instance, considered);

    analyzer_score_classify(instance, depth, base, out);

    uint32_t score = (out->quantization * (100u - ANALYZER_SCORE_REP_POINTS)) / 100u;
    if(out->repetition) score += ANALYZER_SCORE_REP_POINTS;
    out->score = (uint8_t)(score > 100u ? 100u : score);

    out->verdict = (out->quantization < ANALYZER_SCORE_QUANT_MIN) ? AnalyzerScoreVerdictNoise :
                                                                    AnalyzerScoreVerdictScored;
}
