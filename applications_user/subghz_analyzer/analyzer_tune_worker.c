#include "analyzer_tune_worker.h"

#include <furi.h>
#include <lib/subghz/subghz_worker.h>

#include <math.h>
#include <string.h>

#define TAG "SubGhzAnalyzer"

#define ANALYZER_FLOOR_WINDOWS  8u
#define ANALYZER_LISTEN_MS      4000u
#define ANALYZER_LISTEN_WINDOWS 8u
#define ANALYZER_ARM_MS         400u
#define ANALYZER_ARM_POLL_MS    2u
#define ANALYZER_CAPTURE_MS     120u
#define ANALYZER_SETTLE_MS      12u

#define ANALYZER_PROBE_BW_HZ 270833.0f

#define ANALYZER_DEV_BW_SHARE 0.6f

#define ANALYZER_MOD_TRUST_CONFIDENCE 75u

#define ANALYZER_RATE_STRIDE 6u
#define ANALYZER_RATE_COARSE \
    ((ANALYZER_RATE_COUNT + ANALYZER_RATE_STRIDE - 1u) / ANALYZER_RATE_STRIDE)
#define ANALYZER_RATE_SNAP  1u
#define ANALYZER_RATE_STEPS (ANALYZER_RATE_COARSE + 2u * ANALYZER_RATE_SNAP)

#define ANALYZER_DEV_WINDOW 10

#define ANALYZER_ATTEN_STEPS  4u
#define ANALYZER_LNA_STEPS    8u
#define ANALYZER_GAIN_STEPS   (ANALYZER_LNA_STEPS + 1u + 2u + ANALYZER_ATTEN_STEPS + 2u)
#define ANALYZER_OVERLOAD_RUN 3u

#define ANALYZER_RAW_WORKER_MERGE_US 30u
#define ANALYZER_RAW_DROP_US         20u

#define ANALYZER_CAPTURE_FILTER_MIN_US 2u
#define ANALYZER_CAPTURE_FILTER_MAX_US 30u

#define ANALYZER_SYM_OBS_MAX 48u

#define ANALYZER_SYM_QUORUM 3u

#define ANALYZER_BLIND_RATE_BAUD 6149u

typedef struct {
    AnalyzerScoreResult score;
    bool judged;
} AnalyzerTrial;

struct AnalyzerWorker {
    FuriThread* thread;
    FuriMutex* mutex;
    volatile bool running;
    volatile bool cancel;
    volatile bool finished;

    const SubGhzDevice* device;
    SubGhzWorker* capture;
    AnalyzerScore* score;
    AnalyzerEnv* env;
    AnalyzerLog* log;

    volatile bool feed_enabled;
    volatile bool overrun;

    AnalyzerSession session;
    uint32_t frequency;

    uint16_t plan_total;
    uint16_t plan_done;

    uint32_t max_edges_seen;
    uint32_t trials_run;
    uint32_t started_tick;
    uint32_t wait_total_ms;

    uint32_t sym_obs[ANALYZER_SYM_OBS_MAX];
    uint8_t sym_obs_count;
    uint16_t raster_trials;
    uint16_t scored_trials;
    uint16_t ook_trials;
    uint16_t ook_keyed_trials;
    uint32_t probe_rate_baud;

    AnalyzerProgress progress;
    AnalyzerResult best;
    bool best_valid;

    uint8_t blob[ANALYZER_MODEL_BLOB_MAX];
};

const char* analyzer_stage_name(AnalyzerStage stage) {
    switch(stage) {
    case AnalyzerStageSeed:
        return "Seed check";
    case AnalyzerStageBandwidth:
        return "Bandwidth";
    case AnalyzerStageRate:
        return "Symbol rate";
    case AnalyzerStageDeviation:
        return "Deviation";
    case AnalyzerStageGain:
        return "Gain";
    case AnalyzerStageCentre:
        return "Centring";
    case AnalyzerStageConfirm:
        return "Confirming";
    case AnalyzerStageDone:
    default:
        return "Done";
    }
}

const char* analyzer_outcome_name(AnalyzerOutcome outcome) {
    switch(outcome) {
    case AnalyzerOutcomeTuned:
        return "Tuned";
    case AnalyzerOutcomeCarrier:
        return "Carrier only";
    case AnalyzerOutcomeNoSignal:
        return "No signal";
    case AnalyzerOutcomeUnclear:
        return "Unclear";
    case AnalyzerOutcomePending:
    default:
        return "Pending";
    }
}

static void analyzer_worker_pair_callback(void* context, bool level, uint32_t duration) {
    AnalyzerWorker* instance = context;
    if(!instance->feed_enabled) return;
    analyzer_score_feed(instance->score, level, duration);
}

static void analyzer_worker_overrun_callback(void* context) {
    AnalyzerWorker* instance = context;
    instance->overrun = true;
}

static bool analyzer_worker_aborted(AnalyzerWorker* instance) {
    return instance->cancel;
}

static void
    analyzer_worker_set_stage(AnalyzerWorker* instance, AnalyzerStage stage, uint16_t steps) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->progress.stage = stage;
    instance->progress.step = 0;
    instance->progress.steps = steps;
    furi_mutex_release(instance->mutex);

    analyzer_log_newline(instance->log);
    analyzer_log_printf(instance->log, "== stage: %s ==", analyzer_stage_name(stage));
}

static void analyzer_worker_step_done(AnalyzerWorker* instance) {
    if(instance->plan_done < instance->plan_total) instance->plan_done++;

    const uint8_t pct =
        (uint8_t)(instance->plan_total ? (100u * instance->plan_done) / instance->plan_total : 0u);

    uint16_t eta_s = 0;
    if(instance->plan_done >= 4u && instance->plan_done < instance->plan_total) {
        uint32_t elapsed = furi_get_tick() - instance->started_tick;
        const uint32_t idle = (instance->wait_total_ms * furi_kernel_get_tick_frequency()) / 1000u;
        elapsed = (elapsed > idle) ? (elapsed - idle) : 0u;
        const uint32_t remaining = instance->plan_total - instance->plan_done;
        const uint32_t left_ticks = (elapsed / instance->plan_done) * remaining;
        const uint32_t left_s = left_ticks / furi_kernel_get_tick_frequency();
        eta_s = (uint16_t)(left_s > UINT16_MAX ? UINT16_MAX : left_s);
    }

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->progress.step++;
    instance->progress.progress = pct > 100 ? 100 : pct;
    instance->progress.eta_s = eta_s;
    furi_mutex_release(instance->mutex);
}

static void analyzer_worker_publish_level(AnalyzerWorker* instance, int8_t level, int8_t gate) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->progress.level_raw = level;
    instance->progress.gate_raw = gate;
    instance->progress.signal = level > gate;
    furi_mutex_release(instance->mutex);
}

static void analyzer_worker_publish_wait(AnalyzerWorker* instance, bool waiting, uint32_t ms) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->progress.waiting = waiting;
    instance->progress.wait_s = (uint16_t)(ms / 1000u);
    furi_mutex_release(instance->mutex);
}

static bool analyzer_worker_push(AnalyzerWorker* instance, const AnalyzerModel* model) {
    const size_t len = analyzer_model_build(model, instance->blob, sizeof(instance->blob));
    if(len == 0) return false;

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->progress.model = *model;
    furi_mutex_release(instance->mutex);

    if(subghz_devices_hop_is_supported(instance->device) &&
       subghz_devices_hop_goto(instance->device, model->frequency, instance->blob)) {
        subghz_devices_hop_resume(instance->device);
        return true;
    }

    subghz_devices_hop_end(instance->device);

    subghz_devices_stop_async_rx(instance->device);
    subghz_devices_idle(instance->device);
    subghz_devices_load_preset(instance->device, FuriHalSubGhzPresetCustom, instance->blob);
    subghz_devices_set_frequency(instance->device, model->frequency);
    subghz_devices_flush_rx(instance->device);
    subghz_devices_start_async_rx(instance->device, subghz_worker_rx_callback, instance->capture);
    subghz_devices_hop_begin(instance->device);
    return true;
}

static int8_t analyzer_worker_gate(AnalyzerWorker* instance, const AnalyzerModel* model) {
    const float bw = (float)analyzer_model_bandwidth_hz(model);
    const float shift_db = 10.0f * log10f(bw / ANALYZER_PROBE_BW_HZ);

    const int32_t gate = (int32_t)analyzer_env_gate_raw(instance->env) +
                         (int32_t)(shift_db * 2.0f + (shift_db < 0 ? -0.5f : 0.5f));
    if(gate > 127) return 127;
    if(gate < -128) return -128;
    return (int8_t)gate;
}

static void analyzer_worker_log_trial(
    AnalyzerWorker* instance,
    const AnalyzerModel* model,
    const AnalyzerTrial* trial) {
    if(!analyzer_log_is_open(instance->log)) return;

    const char* verdict = "armfail";
    if(trial->judged) {
        switch(trial->score.verdict) {
        case AnalyzerScoreVerdictScored:
            verdict = "scored";
            break;
        case AnalyzerScoreVerdictNoise:
            verdict = "noise";
            break;
        case AnalyzerScoreVerdictRaster:
            verdict = "raster";
            break;
        case AnalyzerScoreVerdictFloor:
            verdict = "floor";
            break;
        default:
            verdict = "nosig";
            break;
        }
    }

    analyzer_log_printf(
        instance->log,
        "trial %-4lu mod=%-7s bw=%-6lu dev=%-6lu rate=%-6lu lna=%u agc=%u dcf=%u foff=%-4d "
        "cs=%u -> %-7s score=%-3u quant=%-3u rep=%u sym=%luus edges=%lu "
        "raster=%luus x%lu.%02lu duty=%u%%",
        (unsigned long)instance->trials_run,
        (model->mod == AnalyzerModOok) ? "OOK" :
                                         (model->mod == AnalyzerModGfsk ? "GFSK" : "2-FSK"),
        (unsigned long)analyzer_model_bandwidth_hz(model),
        (unsigned long)((model->mod == AnalyzerModOok) ? 0u : analyzer_model_deviation_hz(model)),
        (unsigned long)analyzer_model_rate_baud(model),
        model->max_lna_gain,
        (model->magn_target == ANALYZER_AGC_AUTO) ? 9u : model->magn_target,
        model->dcfilt_off ? 1u : 0u,
        model->freq_offset,
        model->cs_gate ? 1u : 0u,
        verdict,
        trial->judged ? trial->score.score : 0u,
        trial->judged ? trial->score.quantization : 0u,
        (trial->judged && trial->score.repetition) ? 1u : 0u,
        (unsigned long)(trial->judged ? trial->score.base_symbol_us : 0u),
        (unsigned long)(trial->judged ? trial->score.edges : 0u),
        (unsigned long)(trial->judged ? trial->score.raster_us : 0u),
        (unsigned long)(trial->judged ? trial->score.raster_ratio_pct / 100u : 0u),
        (unsigned long)(trial->judged ? trial->score.raster_ratio_pct % 100u : 0u),
        trial->judged ? trial->score.duty_pct : 0u);

    if(trial->judged && trial->score.edges >= ANALYZER_SCORE_MIN_EDGES) {
        char clusters[96];
        analyzer_score_format_clusters(instance->score, clusters, sizeof(clusters));
        if(clusters[0]) {
            analyzer_log_printf(instance->log, "         widths: %s", clusters);
        }
    }
}

static AnalyzerModel analyzer_worker_seed_model(AnalyzerWorker* instance, uint8_t atten);

static void
    analyzer_worker_base_model(AnalyzerWorker* instance, uint8_t atten, AnalyzerModel* out) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    const bool valid = instance->best_valid;
    if(valid) *out = instance->best.model;
    furi_mutex_release(instance->mutex);
    if(!valid) *out = analyzer_worker_seed_model(instance, atten);
}

static void analyzer_worker_note_symbol(AnalyzerWorker* instance, uint32_t symbol_us) {
    if(symbol_us == 0) return;
    if(instance->sym_obs_count >= ANALYZER_SYM_OBS_MAX) return;
    instance->sym_obs[instance->sym_obs_count++] = symbol_us;
}

static uint32_t analyzer_worker_symbol_consensus(AnalyzerWorker* instance, uint8_t* votes_out) {
    uint32_t best_mean = 0;
    uint8_t best_votes = 0;

    for(uint8_t i = 0; i < instance->sym_obs_count; i++) {
        const uint32_t centre = instance->sym_obs[i];
        const uint32_t lo = (uint32_t)((float)centre * (1.0f - ANALYZER_SCORE_AGREE));
        const uint32_t hi = (uint32_t)((float)centre * (1.0f + ANALYZER_SCORE_AGREE));

        uint64_t sum = 0;
        uint8_t votes = 0;
        for(uint8_t j = 0; j < instance->sym_obs_count; j++) {
            if(instance->sym_obs[j] >= lo && instance->sym_obs[j] <= hi) {
                sum += instance->sym_obs[j];
                votes++;
            }
        }
        if(votes > best_votes) {
            best_votes = votes;
            best_mean = (uint32_t)(sum / votes);
        }
    }

    if(votes_out) *votes_out = best_votes;
    return (best_votes >= ANALYZER_SYM_QUORUM) ? best_mean : 0u;
}

static void analyzer_worker_capture(
    AnalyzerWorker* instance,
    const AnalyzerModel* model,
    bool want_signal,
    AnalyzerTrial* out) {
    memset(out, 0, sizeof(*out));

    analyzer_worker_push(instance, model);
    furi_delay_ms(ANALYZER_SETTLE_MS);

    const int8_t gate = analyzer_worker_gate(instance, model);
    bool armed = false;

    const uint32_t deadline_ms = want_signal ? 0u : ANALYZER_ARM_MS;
    uint32_t waited = 0;
    for(;;) {
        if(analyzer_worker_aborted(instance)) {
            analyzer_worker_publish_wait(instance, false, 0);
            return;
        }

        const int8_t level = analyzer_env_level_raw(instance->device);
        analyzer_worker_publish_level(instance, level, gate);
        if((level > gate) == want_signal) {
            armed = true;
            break;
        }
        if(deadline_ms && waited >= deadline_ms) break;

        if(want_signal) analyzer_worker_publish_wait(instance, true, waited);
        furi_delay_ms(ANALYZER_ARM_POLL_MS);
        waited += ANALYZER_ARM_POLL_MS;
    }
    analyzer_worker_publish_wait(instance, false, 0);
    if(want_signal) instance->wait_total_ms += waited;

    instance->trials_run++;
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    if(armed || !want_signal) {
        instance->progress.starved = 0;
    } else if(instance->progress.starved < UINT8_MAX) {
        instance->progress.starved++;
    }
    furi_mutex_release(instance->mutex);

    if(!armed) {
        analyzer_worker_log_trial(instance, model, out);
        return;
    }

    instance->feed_enabled = false;
    furi_delay_ms(2);

    const uint32_t raster_us = analyzer_score_raster_us(analyzer_model_rate_baud(model));
    uint32_t filter_us = raster_us / 2u;
    if(filter_us < ANALYZER_CAPTURE_FILTER_MIN_US) filter_us = ANALYZER_CAPTURE_FILTER_MIN_US;
    if(filter_us > ANALYZER_CAPTURE_FILTER_MAX_US) filter_us = ANALYZER_CAPTURE_FILTER_MAX_US;

    subghz_worker_set_filter(instance->capture, (uint16_t)filter_us);
    analyzer_score_set_raster(instance->score, raster_us);
    analyzer_score_set_floor(instance->score, filter_us);

    analyzer_score_reset(instance->score);
    instance->overrun = false;
    instance->feed_enabled = true;

    uint32_t left = ANALYZER_CAPTURE_MS;
    while(left > 0 && !instance->overrun && !analyzer_worker_aborted(instance)) {
        const uint32_t step = left < 10 ? left : 10;
        furi_delay_ms(step);
        left -= step;
    }
    instance->feed_enabled = false;
    furi_delay_ms(2);

    analyzer_score_eval(instance->score, &out->score);
    out->judged = true;

    if(!want_signal) {
        analyzer_worker_log_trial(instance, model, out);
        return;
    }

    if(out->score.edges > instance->max_edges_seen) instance->max_edges_seen = out->score.edges;

    if(model->mod == AnalyzerModOok && out->judged &&
       out->score.edges >= ANALYZER_CARRIER_MIN_EDGES) {
        instance->ook_trials++;
        if(out->score.duty_pct >= ANALYZER_CARRIER_DUTY_PCT &&
           out->score.duty_pct <= 100u - ANALYZER_CARRIER_DUTY_PCT) {
            instance->ook_keyed_trials++;
        }
    }

    if(out->score.verdict == AnalyzerScoreVerdictRaster ||
       out->score.verdict == AnalyzerScoreVerdictFloor) {
        instance->raster_trials++;
    } else if(out->score.verdict == AnalyzerScoreVerdictScored) {
        instance->scored_trials++;
        analyzer_worker_note_symbol(instance, out->score.base_symbol_us);
    }

    analyzer_worker_log_trial(instance, model, out);
}

static void analyzer_worker_measure(
    AnalyzerWorker* instance,
    const AnalyzerModel* model,
    AnalyzerTrial* out) {
    analyzer_worker_capture(instance, model, true, out);
}

static bool analyzer_worker_bw_passes_tones(const AnalyzerModel* m) {
    if(m->mod == AnalyzerModOok) return true;
    const uint32_t dev = analyzer_model_deviation_hz(m);
    if(dev == 0) return true;
    return analyzer_model_bandwidth_hz(m) >= 2u * dev;
}

static bool analyzer_worker_consider(
    AnalyzerWorker* instance,
    const AnalyzerModel* candidate,
    const AnalyzerTrial* trial) {
    if(!trial->judged) return false;
    if(trial->score.verdict != AnalyzerScoreVerdictScored) return false;
    if(!analyzer_worker_bw_passes_tones(candidate)) return false;

    const uint8_t score = trial->score.score;

    bool better;
    if(!instance->best_valid) {
        better = true;
    } else if(score != instance->best.score) {
        better = score > instance->best.score;
    } else {
        better = analyzer_model_bandwidth_hz(candidate) <
                 analyzer_model_bandwidth_hz(&instance->best.model);
    }
    if(!better) return false;

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->best.model = *candidate;
    instance->best.score = score;
    instance->best.symbol_us = trial->score.base_symbol_us;
    instance->best.symbol_baud =
        trial->score.base_symbol_us ? (1000000u / trial->score.base_symbol_us) : 0u;
    instance->best.edges = trial->score.edges;
    instance->best.encoding = trial->score.encoding;
    instance->best.run1_pct = trial->score.run1_pct;
    instance->best.run2_pct = trial->score.run2_pct;
    instance->best.run3_pct = trial->score.run3_pct;
    instance->best.manchester_viol_pct = trial->score.manchester_viol_pct;
    instance->best_valid = true;
    furi_mutex_release(instance->mutex);

    return true;
}

static void analyzer_worker_get_best(AnalyzerWorker* instance, AnalyzerModel* out) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    *out = instance->best.model;
    furi_mutex_release(instance->mutex);
}

static void
    analyzer_worker_finish(AnalyzerWorker* instance, AnalyzerOutcome outcome, uint8_t confidence) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->best.outcome = outcome;
    instance->best.confidence = confidence;
    instance->best.peak_dbm = instance->session.peak_dbm;
    instance->best.floor_dbm = analyzer_env_floor_dbm(instance->env);
    instance->progress.stage = AnalyzerStageDone;
    instance->progress.progress = 100;
    instance->progress.signal = false;
    furi_mutex_release(instance->mutex);
}

static uint8_t analyzer_worker_dev_for_bw(uint8_t bw_idx) {
    AnalyzerModel probe;
    analyzer_model_init(&probe);
    probe.bw_idx = bw_idx;
    const uint32_t want = analyzer_model_bandwidth_hz(&probe) / 4u;

    uint8_t best = 0;
    uint32_t best_err = UINT32_MAX;
    for(uint8_t i = 0; i < ANALYZER_DEV_COUNT; i++) {
        probe.dev_idx = i;
        const uint32_t have = analyzer_model_deviation_hz(&probe);
        const uint32_t err = (have > want) ? (have - want) : (want - have);
        if(err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return best;
}

static uint8_t analyzer_worker_dev_limit(const AnalyzerModel* model) {
    const uint32_t cap =
        (uint32_t)((float)analyzer_model_bandwidth_hz(model) * ANALYZER_DEV_BW_SHARE);

    AnalyzerModel probe = *model;
    uint8_t limit = 0;
    for(uint8_t i = 0; i < ANALYZER_DEV_COUNT; i++) {
        probe.dev_idx = i;
        if(analyzer_model_deviation_hz(&probe) > cap) break;
        limit = i;
    }
    return limit;
}

static AnalyzerModel analyzer_worker_seed_model(AnalyzerWorker* instance, uint8_t atten) {
    const AnalyzerSession* session = &instance->session;

    AnalyzerModel m;
    analyzer_model_init(&m);
    m.frequency = instance->frequency;
    m.close_in_rx = atten;
    m.mod = (session->seed_mod < AnalyzerModCount) ? session->seed_mod : AnalyzerMod2Fsk;

    const uint32_t dev = session->seed_deviation_hz;
    const uint32_t baud = session->seed_rate_baud;

    if(dev) m.dev_idx = analyzer_model_dev_index_for_hz(dev);

    analyzer_model_set_rate_baud(&m, baud ? baud : ANALYZER_BLIND_RATE_BAUD);

    if(session->seed_bandwidth_hz) {
        m.bw_idx = analyzer_model_bw_index_atleast(session->seed_bandwidth_hz);
    } else if(dev || baud) {
        const uint32_t carson = 2u * dev + baud;
        m.bw_idx = analyzer_model_bw_index_atleast(carson + carson / 2u);
    }

    return m;
}

static void analyzer_worker_stage_seed(AnalyzerWorker* instance, uint8_t atten) {
    const AnalyzerSession* session = &instance->session;
    if(!session->seed_bandwidth_hz && session->seed_mod >= AnalyzerModCount) return;

    analyzer_worker_set_stage(instance, AnalyzerStageSeed, 1);

    const AnalyzerModel seed = analyzer_worker_seed_model(instance, atten);
    analyzer_log_printf(
        instance->log,
        "seed from lock stage: %s bw=%lu dev=%lu rate=%lu",
        (seed.mod == AnalyzerModOok) ? "OOK" : (seed.mod == AnalyzerModGfsk ? "GFSK" : "2-FSK"),
        (unsigned long)analyzer_model_bandwidth_hz(&seed),
        (unsigned long)analyzer_model_deviation_hz(&seed),
        (unsigned long)analyzer_model_rate_baud(&seed));

    AnalyzerTrial trial;
    analyzer_worker_measure(instance, &seed, &trial);
    analyzer_worker_consider(instance, &seed, &trial);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->progress.step++;
    furi_mutex_release(instance->mutex);
}

static void analyzer_worker_stage_bandwidth(
    AnalyzerWorker* instance,
    const uint8_t* mods,
    uint8_t mod_count,
    uint8_t atten) {
    analyzer_worker_set_stage(
        instance, AnalyzerStageBandwidth, (uint16_t)(mod_count * ANALYZER_BW_COUNT));

    AnalyzerModel candidate;
    analyzer_model_init(&candidate);
    candidate.frequency = instance->frequency;
    candidate.close_in_rx = atten;

    for(uint8_t m = 0; m < mod_count; m++) {
        for(uint8_t bw = 0; bw < ANALYZER_BW_COUNT; bw++) {
            if(analyzer_worker_aborted(instance)) return;

            candidate.mod = mods[m];
            candidate.bw_idx = bw;
            candidate.rate_exact = false;

            if(instance->session.seed_deviation_hz) {
                candidate.dev_idx =
                    analyzer_model_dev_index_for_hz(instance->session.seed_deviation_hz);
            } else {
                candidate.dev_idx = analyzer_worker_dev_for_bw(bw);
            }

            if(candidate.mod != AnalyzerModOok) {
                if(instance->session.seed_rate_baud) {
                    analyzer_model_set_rate_baud(&candidate, instance->session.seed_rate_baud);
                } else {
                    analyzer_model_set_rate_baud(&candidate, instance->probe_rate_baud);
                }
            } else {
                analyzer_model_set_rate_baud(
                    &candidate,
                    instance->session.seed_rate_baud ? instance->session.seed_rate_baud :
                                                       instance->probe_rate_baud);
            }

            AnalyzerTrial trial;
            analyzer_worker_measure(instance, &candidate, &trial);
            analyzer_worker_consider(instance, &candidate, &trial);
            analyzer_worker_step_done(instance);
        }
    }
}

static void analyzer_worker_stage_rate(
    AnalyzerWorker* instance,
    bool coarse,
    const uint8_t* mods,
    uint8_t mod_count,
    uint8_t atten) {
    analyzer_worker_set_stage(
        instance,
        AnalyzerStageRate,
        coarse ? (uint16_t)(mod_count * ANALYZER_RATE_COARSE + ANALYZER_RATE_SNAP) :
                 ANALYZER_RATE_SNAP);

    if(coarse) {
        AnalyzerModel base;
        analyzer_worker_base_model(instance, atten, &base);

        if(!instance->session.seed_bandwidth_hz) {
            base.bw_idx = analyzer_model_bw_index_for_hz((uint32_t)ANALYZER_PROBE_BW_HZ);
        }

        for(uint8_t m = 0; m < mod_count; m++) {
            for(uint8_t i = 0; i < ANALYZER_RATE_COUNT; i = (uint8_t)(i + ANALYZER_RATE_STRIDE)) {
                if(analyzer_worker_aborted(instance)) return;

                AnalyzerModel trial_model = base;
                trial_model.mod = mods[m];
                trial_model.rate_exact = false;
                trial_model.rate_idx = i;

                AnalyzerTrial trial;
                analyzer_worker_measure(instance, &trial_model, &trial);
                analyzer_worker_consider(instance, &trial_model, &trial);
                analyzer_worker_step_done(instance);
            }
        }
    }

    uint8_t votes = 0;
    const uint32_t consensus = analyzer_worker_symbol_consensus(instance, &votes);
    if(consensus == 0) {
        analyzer_log_printf(
            instance->log,
            "rate: no symbol time survived the raster check (%u scored, %u raster) - "
            "best agreement %u/%u measurements",
            instance->scored_trials,
            instance->raster_trials,
            votes,
            instance->sym_obs_count);
        return;
    }

    const uint32_t baud = 1000000u / consensus;
    analyzer_log_printf(
        instance->log,
        "rate: measured unit interval %lu us (%u of %u trials agree) -> %lu Bd",
        (unsigned long)consensus,
        votes,
        instance->sym_obs_count,
        (unsigned long)baud);

    instance->probe_rate_baud = baud;

    AnalyzerModel center;
    analyzer_worker_base_model(instance, atten, &center);

    AnalyzerModel exact = center;
    analyzer_model_set_rate_baud(&exact, baud);

    AnalyzerTrial trial;
    analyzer_worker_measure(instance, &exact, &trial);
    if(!analyzer_worker_consider(instance, &exact, &trial)) {
        if(trial.judged && trial.score.verdict != AnalyzerScoreVerdictNoSignal) {
            furi_mutex_acquire(instance->mutex, FuriWaitForever);
            instance->best.model = exact;
            instance->best_valid = true;
            furi_mutex_release(instance->mutex);
            analyzer_log_printf(
                instance->log, "rate: adopted measured %lu Bd unscored", (unsigned long)baud);
        }
    }
    analyzer_worker_step_done(instance);
}

static void analyzer_worker_stage_deviation(AnalyzerWorker* instance) {
    AnalyzerModel candidate;
    analyzer_worker_get_best(instance, &candidate);

    const uint8_t limit = analyzer_worker_dev_limit(&candidate);

    uint8_t first = 0;
    uint8_t last = limit;
    if(instance->session.seed_deviation_hz) {
        const int32_t centre =
            analyzer_model_dev_index_for_hz(instance->session.seed_deviation_hz);
        const int32_t lo = centre - ANALYZER_DEV_WINDOW;
        const int32_t hi = centre + ANALYZER_DEV_WINDOW;
        first = (uint8_t)(lo < 0 ? 0 : lo);
        last = (uint8_t)(hi > (int32_t)limit ? limit : hi);
        if(first > last) first = last;
    }

    const uint16_t planned = (uint16_t)(last - first + 1u);
    if(instance->plan_total > (uint16_t)(ANALYZER_DEV_COUNT - planned)) {
        instance->plan_total -= (uint16_t)(ANALYZER_DEV_COUNT - planned);
    }

    analyzer_worker_set_stage(instance, AnalyzerStageDeviation, planned);

    for(uint8_t dev = first; dev <= last; dev++) {
        if(analyzer_worker_aborted(instance)) return;

        AnalyzerModel trial_model = candidate;
        trial_model.dev_idx = dev;

        AnalyzerTrial trial;
        analyzer_worker_measure(instance, &trial_model, &trial);
        analyzer_worker_consider(instance, &trial_model, &trial);
        analyzer_worker_step_done(instance);
    }
}

static void analyzer_worker_stage_gain(AnalyzerWorker* instance) {
    analyzer_worker_set_stage(instance, AnalyzerStageGain, ANALYZER_GAIN_STEPS);

    AnalyzerModel base;
    analyzer_worker_get_best(instance, &base);

    for(uint8_t max_lna_gain = 0; max_lna_gain < ANALYZER_LNA_STEPS; max_lna_gain++) {
        if(analyzer_worker_aborted(instance)) return;

        AnalyzerModel trial_model = base;
        trial_model.max_lna_gain = max_lna_gain;
        trial_model.magn_target = ANALYZER_AGC_AUTO;
        trial_model.dcfilt_off = false;

        AnalyzerTrial trial;
        analyzer_worker_measure(instance, &trial_model, &trial);
        analyzer_worker_consider(instance, &trial_model, &trial);
        analyzer_worker_step_done(instance);
    }

    AnalyzerModel best;
    analyzer_worker_get_best(instance, &best);
    if(analyzer_worker_aborted(instance)) return;

    AnalyzerModel magn_alt = best;
    magn_alt.magn_target = analyzer_model_agc_target_alternative(&best);
    {
        AnalyzerTrial trial;
        analyzer_worker_measure(instance, &magn_alt, &trial);
        analyzer_worker_consider(instance, &magn_alt, &trial);
        analyzer_worker_step_done(instance);
    }

    analyzer_worker_get_best(instance, &best);

    for(uint8_t dcfilt_trial = 0; dcfilt_trial < 2; dcfilt_trial++) {
        if(analyzer_worker_aborted(instance)) return;

        AnalyzerModel trial_model = best;
        trial_model.dcfilt_off = (dcfilt_trial != 0);

        AnalyzerTrial trial;
        analyzer_worker_measure(instance, &trial_model, &trial);
        analyzer_worker_consider(instance, &trial_model, &trial);
        analyzer_worker_step_done(instance);
    }

    analyzer_worker_get_best(instance, &best);

    for(uint8_t atten_trial = 0; atten_trial < ANALYZER_ATTEN_STEPS; atten_trial++) {
        if(analyzer_worker_aborted(instance)) return;
        if(atten_trial == best.close_in_rx) continue;

        AnalyzerModel trial_model = best;
        trial_model.close_in_rx = atten_trial;

        AnalyzerTrial trial;
        analyzer_worker_measure(instance, &trial_model, &trial);
        analyzer_worker_consider(instance, &trial_model, &trial);
        analyzer_worker_step_done(instance);
    }

    analyzer_worker_get_best(instance, &best);

    if(best.mod != AnalyzerModOok) {
        for(uint8_t cs_trial = 0; cs_trial < 2; cs_trial++) {
            if(analyzer_worker_aborted(instance)) return;

            AnalyzerModel trial_model = best;
            trial_model.cs_gate = (cs_trial == 0);

            AnalyzerTrial trial;
            analyzer_worker_measure(instance, &trial_model, &trial);
            analyzer_worker_consider(instance, &trial_model, &trial);
            analyzer_worker_step_done(instance);
        }
    }
}

static void analyzer_worker_stage_centre(AnalyzerWorker* instance) {
    analyzer_worker_set_stage(instance, AnalyzerStageCentre, 1);

    AnalyzerModel candidate;
    analyzer_worker_get_best(instance, &candidate);

    const AnalyzerSurvey* survey = &instance->session.survey;
    int32_t correction_lsb = 0;
    if(survey->valid && survey->peak_offset_hz != 0) {
        correction_lsb =
            (int32_t)(((int64_t)survey->peak_offset_hz * 1000) / ANALYZER_SURVEY_LSB_MHZ);
    } else {
        correction_lsb = subghz_devices_get_freq_est(instance->device);
    }

    if(correction_lsb != 0) {
        const int32_t next = (int32_t)candidate.freq_offset + correction_lsb;
        candidate.freq_offset = (int8_t)(next < -128 ? -128 : (next > 127 ? 127 : next));

        AnalyzerTrial trial;
        analyzer_worker_measure(instance, &candidate, &trial);
        if(!analyzer_worker_consider(instance, &candidate, &trial) && !trial.judged &&
           survey->valid && survey->peak_offset_hz != 0) {
            furi_mutex_acquire(instance->mutex, FuriWaitForever);
            instance->best.model.freq_offset = candidate.freq_offset;
            furi_mutex_release(instance->mutex);
            analyzer_log_printf(
                instance->log,
                "centre: signal gone; applied survey offset %+d LSB unconfirmed",
                candidate.freq_offset);
        }
    }
    analyzer_worker_step_done(instance);
}

static uint8_t analyzer_worker_stage_confirm(AnalyzerWorker* instance) {
    analyzer_worker_set_stage(instance, AnalyzerStageConfirm, 2);

    AnalyzerModel candidate;
    analyzer_worker_get_best(instance, &candidate);

    uint8_t votes = 0;
    const uint32_t consensus = analyzer_worker_symbol_consensus(instance, &votes);
    if(consensus > 0) {
        const uint32_t baud = 1000000u / consensus;
        AnalyzerModel exact = candidate;
        analyzer_model_set_rate_baud(&exact, baud);
        if(exact.rate_exact) {
            AnalyzerTrial trial;
            analyzer_worker_measure(instance, &exact, &trial);
            const bool scored = trial.judged && trial.score.verdict == AnalyzerScoreVerdictScored;
            if(scored && trial.score.score + 5u >= instance->best.score) {
                furi_mutex_acquire(instance->mutex, FuriWaitForever);
                instance->best.model = exact;
                if(trial.score.score > instance->best.score) {
                    instance->best.score = trial.score.score;
                }
                furi_mutex_release(instance->mutex);
            } else if(!trial.judged) {
                furi_mutex_acquire(instance->mutex, FuriWaitForever);
                instance->best.model = exact;
                furi_mutex_release(instance->mutex);
                analyzer_log_printf(
                    instance->log,
                    "confirm: signal gone; snapped rate to measured %lu Bd unconfirmed",
                    (unsigned long)baud);
            }
            analyzer_worker_step_done(instance);
        }
    }

    analyzer_worker_get_best(instance, &candidate);

    AnalyzerTrial trial;
    analyzer_worker_measure(instance, &candidate, &trial);
    analyzer_worker_step_done(instance);

    if(!trial.judged || trial.score.verdict != AnalyzerScoreVerdictScored) return 40u;

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    if(trial.score.score > instance->best.score) instance->best.score = trial.score.score;
    instance->best.edges = trial.score.edges;
    instance->best.encoding = trial.score.encoding;
    instance->best.run1_pct = trial.score.run1_pct;
    instance->best.run2_pct = trial.score.run2_pct;
    instance->best.run3_pct = trial.score.run3_pct;
    instance->best.manchester_viol_pct = trial.score.manchester_viol_pct;
    furi_mutex_release(instance->mutex);

    const uint32_t final_sym = analyzer_worker_symbol_consensus(instance, NULL);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->best.symbol_us = final_sym;
    instance->best.symbol_baud = final_sym ? (1000000u / final_sym) : 0u;
    furi_mutex_release(instance->mutex);

    if(final_sym == 0) return 25u;

    return trial.score.repetition ? 90u : 70u;
}

#define ANALYZER_CONTROL_OFFSET_HZ 2000000

static bool analyzer_worker_control_at(
    AnalyzerWorker* instance,
    const AnalyzerModel* winner,
    int32_t offset_hz,
    AnalyzerTrial* out) {
    AnalyzerModel off = *winner;

    if(offset_hz != 0) {
        const int64_t f = (int64_t)winner->frequency + offset_hz;
        if(f <= 0 || !analyzer_model_frequency_valid((uint32_t)f)) return false;
        off.frequency = (uint32_t)f;
        off.freq_offset = 0;
    }

    analyzer_worker_capture(instance, &off, false, out);
    return out->judged;
}

static void analyzer_worker_stage_control(AnalyzerWorker* instance, uint32_t symbol_us) {
    AnalyzerModel winner;
    analyzer_worker_get_best(instance, &winner);

    AnalyzerTrial control;
    bool taken =
        analyzer_worker_control_at(instance, &winner, ANALYZER_CONTROL_OFFSET_HZ, &control);
    const char* where = "+2 MHz";
    if(!taken) {
        taken =
            analyzer_worker_control_at(instance, &winner, -ANALYZER_CONTROL_OFFSET_HZ, &control);
        where = "-2 MHz";
    }
    if(!taken) {
        taken = analyzer_worker_control_at(instance, &winner, 0, &control);
        where = "this channel, quiet";

        if(taken &&
           analyzer_env_level_raw(instance->device) > analyzer_worker_gate(instance, &winner)) {
            taken = false;
        }
    }
    analyzer_worker_step_done(instance);

    if(analyzer_worker_aborted(instance)) return;

    if(!taken) {
        analyzer_log_printf(
            instance->log,
            "control: no quiet reference found either side or on channel; not taken");
        return;
    }

    analyzer_log_printf(instance->log, "control: taken %s", where);

    const uint32_t base = control.score.base_symbol_us;
    const bool scored = (control.score.verdict == AnalyzerScoreVerdictScored);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->best.control_taken = true;
    instance->best.control_symbol_us = scored ? base : 0u;
    furi_mutex_release(instance->mutex);

    if(!scored) {
        analyzer_log_printf(
            instance->log,
            "control: quiet channel gives no symbol train (%lu edges) - the tune is "
            "measuring the transmitter",
            (unsigned long)control.score.edges);
        return;
    }

    const uint32_t lo = (uint32_t)((float)symbol_us * (1.0f - ANALYZER_SCORE_AGREE));
    const uint32_t hi = (uint32_t)((float)symbol_us * (1.0f + ANALYZER_SCORE_AGREE));
    if(symbol_us && base >= lo && base <= hi) {
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->best.deaf = true;
        furi_mutex_release(instance->mutex);
        analyzer_log_printf(
            instance->log,
            "*** control: the quiet channel produces the same %lu us unit interval "
            "(quant %u) as the tune did. This preset demodulates noise, so the sweep "
            "measured the receiver rather than the fob - it is deaf and a RAW record "
            "with it will fill up with the demodulator's own output. ***",
            (unsigned long)base,
            control.score.quantization);
        return;
    }

    analyzer_log_printf(
        instance->log,
        "control: quiet channel demodulates a %lu us unit interval, different from the "
        "%lu us the tune measured - another emitter on the channel, or a burst that "
        "outlasted the gate",
        (unsigned long)base,
        (unsigned long)symbol_us);
}

static void analyzer_worker_write_report(AnalyzerWorker* instance) {
    if(!analyzer_log_is_open(instance->log)) return;

    AnalyzerResult r;
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    r = instance->best;
    furi_mutex_release(instance->mutex);

    const AnalyzerModel* m = &r.model;
    const size_t len = analyzer_model_build(m, instance->blob, sizeof(instance->blob));

    analyzer_log_newline(instance->log);
    analyzer_log_printf(instance->log, "================ RESULT ================");
    analyzer_log_printf(instance->log, "outcome        : %s", analyzer_outcome_name(r.outcome));
    analyzer_log_printf(instance->log, "confidence     : %u%%", r.confidence);
    analyzer_log_printf(instance->log, "score          : %u/100", r.score);
    analyzer_log_printf(
        instance->log,
        "frequency      : %lu Hz (FSCTRL0 %+d = %+ld Hz)",
        (unsigned long)m->frequency,
        m->freq_offset,
        (long)(((int64_t)m->freq_offset * ANALYZER_SURVEY_LSB_MHZ) / 1000));
    analyzer_log_printf(
        instance->log,
        "modulation     : %s",
        (m->mod == AnalyzerModOok) ? "ASK/OOK" : (m->mod == AnalyzerModGfsk ? "GFSK" : "2-FSK"));
    analyzer_log_printf(
        instance->log, "rx bandwidth   : %lu Hz", (unsigned long)analyzer_model_bandwidth_hz(m));
    if(m->mod != AnalyzerModOok) {
        analyzer_log_printf(
            instance->log,
            "deviation      : %lu Hz",
            (unsigned long)analyzer_model_deviation_hz(m));
    }
    analyzer_log_printf(
        instance->log,
        "data rate      : %lu Bd%s",
        (unsigned long)analyzer_model_rate_baud(m),
        m->rate_exact ? " (exact DRATE_E/M)" : " (grid)");
    if(r.symbol_us) {
        analyzer_log_printf(
            instance->log,
            "measured symbol: %lu us -> %lu Bd (consensus of %u trials)",
            (unsigned long)r.symbol_us,
            (unsigned long)r.symbol_baud,
            instance->sym_obs_count);
    } else {
        analyzer_log_printf(
            instance->log,
            "measured symbol: NOT RESOLVED - %u of %lu trials returned only the "
            "demodulator's own 8*DRATE raster",
            instance->raster_trials,
            (unsigned long)instance->trials_run);
        analyzer_log_printf(
            instance->log,
            "  the rate below is a blind-capture fallback, not a measurement: it puts "
            "the raster under the RAW recorder's drop threshold so nothing recordable "
            "is quantised away. Re-run closer to the transmitter, or with the "
            "modulation family forced, to resolve a symbol time.");
    }
    if(r.peak_dbm > r.floor_dbm) {
        analyzer_log_printf(
            instance->log,
            "levels         : peak %d dBm, floor %d dBm",
            (int)r.peak_dbm,
            (int)r.floor_dbm);
    } else {
        analyzer_log_printf(
            instance->log,
            "levels         : peak not measured by the lock stage, floor %d dBm",
            (int)r.floor_dbm);
    }
    if(r.symbol_us && r.encoding != AnalyzerEncodingUnknown) {
        analyzer_log_printf(
            instance->log,
            "line code      : %s (runs 1x %u%%, 2x %u%%, 3x+ %u%%; %u%% of bit periods "
            "without a mid-transition)",
            analyzer_encoding_name((AnalyzerEncoding)r.encoding),
            r.run1_pct,
            r.run2_pct,
            r.run3_pct,
            r.manchester_viol_pct);
    }
    analyzer_log_printf(
        instance->log,
        "agc            : max_lna %u, magn_target %u, close_in_rx %u, dcfilt %s, "
        "carrier-sense qualifier %s",
        m->max_lna_gain,
        (m->magn_target == ANALYZER_AGC_AUTO) ? 9u : m->magn_target,
        m->close_in_rx,
        m->dcfilt_off ? "off" : "on",
        m->cs_gate ? "on (SYNC_MODE=100)" : "off (SYNC_MODE=000)");
    analyzer_log_printf(
        instance->log, "trials measured: %lu", (unsigned long)instance->trials_run);

    {
        uint32_t required = 0;
        const AnalyzerReach reach = analyzer_model_reach(
            (m->mod == AnalyzerModOok) ? 0u : analyzer_model_deviation_hz(m),
            r.symbol_baud ? r.symbol_baud : analyzer_model_rate_baud(m),
            &required);
        analyzer_log_printf(
            instance->log,
            "cc1101 reach   : %s (Carson needs %lu Hz of %lu Hz available)",
            analyzer_model_reach_name(reach),
            (unsigned long)required,
            (unsigned long)ANALYZER_CC1101_BW_MAX_HZ);
    }

    if(!r.control_taken) {
        analyzer_log_printf(
            instance->log,
            "noise control   : not taken - neither 2 MHz either side nor this channel was "
            "quiet enough to use as a reference, so nothing here rules out the preset "
            "demodulating noise");
    } else if(r.deaf) {
        analyzer_log_printf(
            instance->log,
            "noise control   : FAILED - the same %lu us unit interval came back where the "
            "transmitter cannot be heard. This preset hears the receiver, not the remote.",
            (unsigned long)r.control_symbol_us);
    } else if(r.control_symbol_us) {
        analyzer_log_printf(
            instance->log,
            "noise control   : passed with a caveat - the reference capture demodulated a "
            "%lu us unit interval, unlike the %lu us measured here",
            (unsigned long)r.control_symbol_us,
            (unsigned long)r.symbol_us);
    } else {
        analyzer_log_printf(
            instance->log,
            "noise control   : passed - where this preset cannot hear the remote it "
            "produces no symbol train at all");
    }

    if(r.survey.valid) {
        analyzer_log_printf(
            instance->log,
            "occupied bw    : %lu Hz (-20dB raw %lu Hz, rbw %lu Hz)",
            (unsigned long)r.survey.occupied_bw_hz,
            (unsigned long)r.survey.width_20db_hz,
            (unsigned long)r.survey.rbw_hz);
        const uint32_t implied =
            analyzer_survey_implied_deviation(&r.survey, analyzer_model_rate_baud(m));
        if(implied) {
            analyzer_log_printf(
                instance->log,
                "implied dev    : %lu Hz (from occupied bw and symbol rate)",
                (unsigned long)implied);
        }
        const uint32_t bw = analyzer_model_bandwidth_hz(m);
        if(r.survey.occupied_bw_hz && bw < r.survey.occupied_bw_hz) {
            analyzer_log_printf(
                instance->log,
                "WARNING: chosen rx bandwidth %lu Hz is narrower than the measured "
                "occupied bandwidth %lu Hz",
                (unsigned long)bw,
                (unsigned long)r.survey.occupied_bw_hz);
        }
    }

    analyzer_log_newline(instance->log);
    analyzer_log_printf(instance->log, "--- preset for RAW record ---");
    analyzer_log_hex(instance->log, "Custom_preset_data:", instance->blob, len);
    analyzer_log_newline(instance->log);
    analyzer_log_printf(instance->log, "--- paste into a .sub RAW file header ---");
    analyzer_log_printf(instance->log, "Filetype: Flipper SubGhz RAW File");
    analyzer_log_printf(instance->log, "Version: 1");
    analyzer_log_printf(instance->log, "Frequency: %lu", (unsigned long)m->frequency);
    analyzer_log_printf(instance->log, "Preset: FuriHalSubGhzPresetCustom");
    analyzer_log_printf(instance->log, "Custom_preset_module: CC1101");
    analyzer_log_hex(instance->log, "Custom_preset_data:", instance->blob, len);
    analyzer_log_printf(instance->log, "Protocol: RAW");
    analyzer_log_newline(instance->log);
    analyzer_log_newline(instance->log);
    analyzer_log_printf(
        instance->log,
        "RAW capture path: SubGhzWorker merges pulses below %u us (preserving "
        "total time); protocol RAW drops pulses of %u us or less, and because "
        "last_level only advances on a write, a dropped pulse takes the "
        "following one with it.",
        (unsigned)ANALYZER_RAW_WORKER_MERGE_US,
        (unsigned)ANALYZER_RAW_DROP_US);

    if(r.symbol_us == 0) {
        analyzer_log_printf(
            instance->log, "  unit interval not measured; no judgement on recordability.");
    } else if(r.symbol_us <= ANALYZER_RAW_DROP_US) {
        analyzer_log_printf(
            instance->log,
            "  *** WARNING: measured unit interval %lu us is at or below the %u us "
            "RAW drop threshold. Read RAW will keep little or nothing of this "
            "signal, and will fail silently - an empty capture looks like bad "
            "range or a wrong preset. Lower te_short in "
            "lib/subghz/protocols/raw.c before recording. ***",
            (unsigned long)r.symbol_us,
            (unsigned)ANALYZER_RAW_DROP_US);
    } else if(r.symbol_us < ANALYZER_RAW_DROP_US * 2u) {
        analyzer_log_printf(
            instance->log,
            "  *** CAUTION: measured unit interval %lu us leaves under 2x margin "
            "over the %u us RAW drop threshold; transmitter jitter can push "
            "individual pulses below it, losing two transitions each time. ***",
            (unsigned long)r.symbol_us,
            (unsigned)ANALYZER_RAW_DROP_US);
    } else {
        analyzer_log_printf(
            instance->log,
            "  measured unit interval %lu us clears both thresholds with margin; "
            "this signal records faithfully.",
            (unsigned long)r.symbol_us);
    }

    analyzer_log_flush(instance->log);
}

static void analyzer_worker_sweep(AnalyzerWorker* instance) {
    const AnalyzerSession* session = &instance->session;

    analyzer_env_reset(instance->env);
    analyzer_env_set_floor_raw(instance->env, session->floor_raw);
    uint8_t atten = session->atten;
    uint8_t mod_confidence = session->env_confidence;

    analyzer_log_printf(
        instance->log,
        "seed: %lu.%03lu MHz  floor %d dBm  atten %u  vote %s (%u%%)  survey %u passes",
        (unsigned long)(instance->frequency / 1000000u),
        (unsigned long)((instance->frequency % 1000000u) / 1000u),
        (int)session->floor_dbm,
        atten,
        (session->env_vote == AnalyzerEnvVoteAm) ?
            "AM/OOK" :
            (session->env_vote == AnalyzerEnvVoteFm ? "FM/FSK" : "none"),
        mod_confidence,
        session->survey_passes);

    const bool twin_lobe = session->survey.valid && session->survey.twin_lobe &&
                           session->survey.lobe_split_hz;
    if(session->survey.valid && session->survey.lobe_rejected_hz) {
        analyzer_log_printf(
            instance->log,
            "twin lobe rejected: %lu Hz split, %s (rbw %lu Hz) - searching both families "
            "and the full deviation range",
            (unsigned long)session->survey.lobe_rejected_hz,
            session->survey.width_resolved ? "wider than the measured -20dB width" :
                                             "no width the sweep held could account for it",
            (unsigned long)session->survey.rbw_hz);
    }

    const bool confident = (mod_confidence >= ANALYZER_MOD_TRUST_CONFIDENCE);

    uint8_t mods[2];
    uint8_t mod_count;
    const char* mod_reason;
    if(twin_lobe && session->env_vote != AnalyzerEnvVoteAm) {
        mods[0] = AnalyzerMod2Fsk;
        mod_count = 1;
        mod_reason = "twin lobe -> FSK";
    } else if(twin_lobe) {
        mods[0] = AnalyzerModOok;
        mods[1] = AnalyzerMod2Fsk;
        mod_count = 2;
        mod_reason = "twin lobe vs AM vote -> both families";
    } else if(session->env_vote == AnalyzerEnvVoteFm && confident) {
        mods[0] = AnalyzerMod2Fsk;
        mod_count = 1;
        mod_reason = "confident FM -> FSK";
    } else if(session->env_vote == AnalyzerEnvVoteAm && confident) {
        mods[0] = AnalyzerModOok;
        mod_count = 1;
        mod_reason = "confident AM -> OOK";
    } else {
        mods[0] = AnalyzerModOok;
        mods[1] = AnalyzerMod2Fsk;
        mod_count = 2;
        if(session->env_vote == AnalyzerEnvVoteNone) mod_confidence = 0;
        mod_reason = "vote not decisive -> both families";
    }

    analyzer_log_printf(instance->log, "modulation search: %s", mod_reason);

    const bool may_fsk = (mods[0] != AnalyzerModOok) || (mod_count > 1);
    const uint16_t fsk_tail = ANALYZER_DEV_COUNT + 1u;

    instance->plan_total = (uint16_t)(mod_count * (ANALYZER_BW_COUNT + ANALYZER_RATE_COARSE)) +
                           2u * ANALYZER_RATE_SNAP + (may_fsk ? fsk_tail : 0u) +
                           ANALYZER_GAIN_STEPS + 3u;
    instance->plan_done = 0;

    analyzer_worker_stage_seed(instance, atten);
    if(analyzer_worker_aborted(instance)) return;

    analyzer_worker_stage_rate(instance, true, mods, mod_count, atten);
    if(analyzer_worker_aborted(instance)) return;

    analyzer_worker_stage_bandwidth(instance, mods, mod_count, atten);
    if(analyzer_worker_aborted(instance)) return;

    if(!instance->best_valid) {
        const bool duty_pinned = (instance->ook_trials > 0) && (instance->ook_keyed_trials == 0);
        const bool carrier = (instance->max_edges_seen <= ANALYZER_CARRIER_EDGE_MAX) ||
                             duty_pinned;
        if(duty_pinned) {
            analyzer_log_printf(
                instance->log,
                "amplitude never keyed: %u OOK trials judged, none with duty between "
                "%u%% and %u%% -> carrier present but unmodulated",
                instance->ook_trials,
                ANALYZER_CARRIER_DUTY_PCT,
                100u - ANALYZER_CARRIER_DUTY_PCT);
        }
        analyzer_log_printf(
            instance->log,
            "no candidate scored; max edges seen in any capture: %lu (%u trials measured "
            "only the receiver's own raster or capture filter) -> %s",
            (unsigned long)instance->max_edges_seen,
            instance->raster_trials,
            carrier ? "unmodulated carrier" : "unclear");

        AnalyzerModel blind;
        analyzer_worker_base_model(instance, atten, &blind);
        analyzer_model_set_rate_baud(&blind, ANALYZER_BLIND_RATE_BAUD);
        if(instance->session.survey.valid && instance->session.survey.width_20db_hz) {
            blind.bw_idx = analyzer_model_bw_index_atleast(
                instance->session.survey.width_20db_hz +
                instance->session.survey.width_20db_hz / 2u);
        }
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->best.model = blind;
        furi_mutex_release(instance->mutex);
        analyzer_worker_push(instance, &blind);

        analyzer_worker_finish(
            instance,
            carrier ? AnalyzerOutcomeCarrier : AnalyzerOutcomeUnclear,
            carrier ? 80u : mod_confidence);
        return;
    }

    AnalyzerModel winner;
    analyzer_worker_get_best(instance, &winner);
    const bool fsk = (winner.mod != AnalyzerModOok);
    if(may_fsk && !fsk && instance->plan_total > fsk_tail) instance->plan_total -= fsk_tail;

    if(fsk) {
        analyzer_worker_stage_deviation(instance);
        if(analyzer_worker_aborted(instance)) return;
    }

    analyzer_worker_stage_rate(instance, false, mods, mod_count, atten);
    if(analyzer_worker_aborted(instance)) return;

    analyzer_worker_stage_gain(instance);
    if(analyzer_worker_aborted(instance)) return;

    if(fsk) {
        analyzer_worker_stage_centre(instance);
        if(analyzer_worker_aborted(instance)) return;
    }

    uint8_t confidence = analyzer_worker_stage_confirm(instance);
    if(analyzer_worker_aborted(instance)) return;

    {
        const uint32_t agreed = analyzer_worker_symbol_consensus(instance, NULL);
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->best.symbol_us = agreed;
        instance->best.symbol_baud = agreed ? (1000000u / agreed) : 0u;
        furi_mutex_release(instance->mutex);
    }

    analyzer_worker_stage_control(instance, instance->best.symbol_us);
    if(analyzer_worker_aborted(instance)) return;

    analyzer_worker_get_best(instance, &winner);

    if(instance->best.deaf) {
        analyzer_model_set_rate_baud(&winner, ANALYZER_BLIND_RATE_BAUD);
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->best.model = winner;
        instance->best.symbol_us = 0;
        instance->best.symbol_baud = 0;
        furi_mutex_release(instance->mutex);
        analyzer_log_printf(
            instance->log,
            "outcome: preset failed the quiet-channel control; exporting blind-capture "
            "preset at %u Bd instead of a tune built on it",
            (unsigned)ANALYZER_BLIND_RATE_BAUD);
        analyzer_worker_push(instance, &winner);
        analyzer_worker_finish(instance, AnalyzerOutcomeUnclear, 0u);
        return;
    }

    if(!instance->best.control_taken) {
        confidence = (confidence > 65u) ? (uint8_t)(confidence - 15u) : confidence;
    }

    if(instance->best.symbol_us == 0) {
        analyzer_model_set_rate_baud(&winner, ANALYZER_BLIND_RATE_BAUD);
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->best.model = winner;
        furi_mutex_release(instance->mutex);
        analyzer_log_printf(
            instance->log,
            "outcome: symbol time unresolved; exporting blind-capture preset at %u Bd "
            "(raster %lu us)",
            (unsigned)ANALYZER_BLIND_RATE_BAUD,
            (unsigned long)analyzer_score_raster_us(ANALYZER_BLIND_RATE_BAUD));
        analyzer_worker_push(instance, &winner);
        analyzer_worker_finish(instance, AnalyzerOutcomeUnclear, confidence);
        return;
    }

    analyzer_worker_push(instance, &winner);

    analyzer_worker_finish(instance, AnalyzerOutcomeTuned, confidence);
}

static void analyzer_worker_teardown(AnalyzerWorker* instance) {
    subghz_devices_hop_end(instance->device);
    subghz_worker_stop(instance->capture);
    subghz_devices_stop_async_rx(instance->device);
    subghz_devices_idle(instance->device);
}

static int32_t analyzer_worker_thread(void* context) {
    AnalyzerWorker* instance = context;

    analyzer_worker_sweep(instance);

    if(instance->best.outcome == AnalyzerOutcomePending) {
        AnalyzerOutcome outcome;
        if(instance->best_valid) {
            AnalyzerModel winner;
            analyzer_worker_get_best(instance, &winner);
            analyzer_worker_push(instance, &winner);
            outcome = AnalyzerOutcomeTuned;
        } else if(analyzer_env_signal_windows(instance->env)) {
            const bool duty_pinned = (instance->ook_trials > 0) &&
                                     (instance->ook_keyed_trials == 0);
            outcome = ((instance->max_edges_seen <= ANALYZER_CARRIER_EDGE_MAX) || duty_pinned) ?
                          AnalyzerOutcomeCarrier :
                          AnalyzerOutcomeUnclear;
        } else {
            outcome = AnalyzerOutcomeNoSignal;
        }
        analyzer_worker_finish(instance, outcome, 0);
    }

    analyzer_worker_write_report(instance);
    analyzer_log_close(instance->log);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    strncpy(
        instance->best.log_path,
        analyzer_log_path(instance->log),
        sizeof(instance->best.log_path) - 1u);
    instance->best.log_path[sizeof(instance->best.log_path) - 1u] = '\0';
    instance->best.logged = (instance->best.log_path[0] != '\0');
    furi_mutex_release(instance->mutex);

    analyzer_worker_teardown(instance);
    instance->finished = true;

    return 0;
}

AnalyzerWorker* analyzer_worker_alloc(const SubGhzDevice* device) {
    AnalyzerWorker* instance = malloc(sizeof(AnalyzerWorker));
    memset(instance, 0, sizeof(AnalyzerWorker));

    instance->device = device;
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->score = analyzer_score_alloc();
    instance->env = analyzer_env_alloc();
    instance->log = analyzer_log_alloc();
    instance->capture = subghz_worker_alloc();

    subghz_worker_set_pair_callback(instance->capture, analyzer_worker_pair_callback);
    subghz_worker_set_context(instance->capture, instance);
    subghz_worker_set_overrun_callback(instance->capture, analyzer_worker_overrun_callback);

    subghz_worker_set_filter(instance->capture, ANALYZER_CAPTURE_FILTER_MAX_US);

    analyzer_model_init(&instance->progress.model);
    instance->best.model = instance->progress.model;

    instance->thread =
        furi_thread_alloc_ex("SubGhzAnalyzer", 4096, analyzer_worker_thread, instance);

    return instance;
}

void analyzer_worker_free(AnalyzerWorker* instance) {
    furi_check(instance);
    if(instance->running) analyzer_worker_stop(instance);

    furi_thread_free(instance->thread);
    subghz_worker_free(instance->capture);

    analyzer_log_free(instance->log);
    analyzer_env_free(instance->env);
    analyzer_score_free(instance->score);
    furi_mutex_free(instance->mutex);
    free(instance);
}

bool analyzer_worker_start(AnalyzerWorker* instance, const AnalyzerSession* session) {
    furi_check(instance);
    furi_check(session);
    if(instance->running) return false;

    instance->session = *session;

    AnalyzerModel start;
    analyzer_model_init(&start);
    if(analyzer_model_frequency_valid(session->frequency)) {
        start.frequency = session->frequency;
    }
    start.close_in_rx = session->atten;
    instance->frequency = start.frequency;

    const size_t len = analyzer_model_build(&start, instance->blob, sizeof(instance->blob));
    if(len == 0) return false;

    memset(&instance->best, 0, sizeof(instance->best));
    instance->best.model = start;
    instance->best.outcome = AnalyzerOutcomePending;
    instance->best.survey = session->survey;
    instance->best_valid = false;
    instance->max_edges_seen = 0;
    instance->trials_run = 0;
    instance->sym_obs_count = 0;
    instance->raster_trials = 0;
    instance->scored_trials = 0;
    instance->ook_trials = 0;
    instance->ook_keyed_trials = 0;
    instance->probe_rate_baud = session->seed_rate_baud ? session->seed_rate_baud :
                                                          ANALYZER_BLIND_RATE_BAUD;
    instance->started_tick = furi_get_tick();
    instance->wait_total_ms = 0;
    memset(&instance->progress, 0, sizeof(instance->progress));
    instance->progress.model = start;
    instance->progress.stage = AnalyzerStageSeed;

    if(analyzer_log_open(instance->log, start.frequency)) {
        DateTime now;
        furi_hal_rtc_get_datetime(&now);
        analyzer_log_printf(
            instance->log,
            "SubGHz Analyzer session %04u-%02u-%02u %02u:%02u:%02u",
            now.year,
            now.month,
            now.day,
            now.hour,
            now.minute,
            now.second);
        analyzer_log_printf(instance->log, "radio: %s", subghz_devices_get_name(instance->device));
        analyzer_log_printf(
            instance->log,
            "frequency: %lu Hz (%s)",
            (unsigned long)start.frequency,
            session->frequency_manual ? "entered by hand" : "found by the hunt stage");
        analyzer_log_printf(
            instance->log,
            "lock stage seed: mod=%u bw=%lu Hz dev=%lu Hz rate=%lu Bd",
            session->seed_mod,
            (unsigned long)session->seed_bandwidth_hz,
            (unsigned long)session->seed_deviation_hz,
            (unsigned long)session->seed_rate_baud);
        analyzer_log_printf(
            instance->log,
            "analyser capture: SubGhzWorker glitch filter tracks each trial's rate "
            "at half its 8*DRATE raster, %u-%u us (firmware default 30 us), no RSSI "
            "squelch, no protocol decoder",
            (unsigned)ANALYZER_CAPTURE_FILTER_MIN_US,
            (unsigned)ANALYZER_CAPTURE_FILTER_MAX_US);
    }

    subghz_devices_reset(instance->device);
    subghz_devices_load_preset(instance->device, FuriHalSubGhzPresetCustom, instance->blob);
    subghz_devices_set_frequency(instance->device, start.frequency);
    subghz_devices_flush_rx(instance->device);

    analyzer_score_reset(instance->score);

    subghz_devices_start_async_rx(instance->device, subghz_worker_rx_callback, instance->capture);
    subghz_worker_start(instance->capture);
    subghz_devices_hop_begin(instance->device);

    instance->feed_enabled = false;
    instance->cancel = false;
    instance->finished = false;
    instance->running = true;
    furi_thread_start(instance->thread);
    return true;
}

void analyzer_worker_stop(AnalyzerWorker* instance) {
    furi_check(instance);
    if(!instance->running) return;

    instance->cancel = true;
    furi_thread_join(instance->thread);
    instance->running = false;
}

void analyzer_worker_cancel(AnalyzerWorker* instance) {
    furi_check(instance);
    instance->cancel = true;
}

bool analyzer_worker_is_finished(AnalyzerWorker* instance) {
    furi_check(instance);
    return instance->finished;
}

void analyzer_worker_get_result(AnalyzerWorker* instance, AnalyzerResult* out) {
    furi_check(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    *out = instance->best;
    furi_mutex_release(instance->mutex);
}

void analyzer_worker_get_progress(AnalyzerWorker* instance, AnalyzerProgress* out) {
    furi_check(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    *out = instance->progress;
    furi_mutex_release(instance->mutex);
}
