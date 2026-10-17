#include "analyzer_lock_worker.h"
#include "analyzer_model.h"

#include <furi.h>
#include <lib/subghz/subghz_worker.h>

#include <string.h>

#define TAG "SubGhzAnalyzerLock"

#define ANALYZER_LOCK_SETTLE_MS 12u

#define ANALYZER_LOCK_BIN_STRIDE    5u
#define ANALYZER_LOCK_FLOOR_WINDOWS 8u
#define ANALYZER_LOCK_OVERLOAD_RUN  3u
#define ANALYZER_LOCK_ATTEN_STEPS   4u

#define ANALYZER_LOCK_WINDOWS_PER_SWEEP 6u

struct AnalyzerLockWorker {
    FuriThread* thread;
    FuriMutex* mutex;
    volatile bool running;

    const SubGhzDevice* device;
    SubGhzWorker* capture;
    AnalyzerEnv* env;

    volatile uint32_t frequency;
    volatile uint8_t zoom;
    volatile bool restart;

    uint8_t atten;
    AnalyzerLockState state;

    uint8_t blob[ANALYZER_MODEL_BLOB_MAX];
};

static AnalyzerModel
    analyzer_lock_probe(uint32_t frequency, uint32_t filter_hz, uint8_t atten, int8_t offset) {
    AnalyzerModel probe;
    analyzer_model_init(&probe);
    probe.frequency = frequency;
    probe.mod = AnalyzerMod2Fsk;
    probe.bw_idx = analyzer_model_bw_index_for_hz(filter_hz);
    probe.dev_idx = analyzer_model_dev_index_for_hz(10000u);
    probe.close_in_rx = atten;
    probe.freq_offset = offset;
    return probe;
}

static bool analyzer_lock_push(AnalyzerLockWorker* instance, const AnalyzerModel* model) {
    const size_t len = analyzer_model_build(model, instance->blob, sizeof(instance->blob));
    if(len == 0) return false;

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

static void analyzer_lock_learn_floor(AnalyzerLockWorker* instance, uint32_t frequency) {
    const AnalyzerModel probe = analyzer_lock_probe(frequency, 270833u, instance->atten, 0);
    analyzer_lock_push(instance, &probe);
    furi_delay_ms(ANALYZER_LOCK_SETTLE_MS);

    analyzer_env_reset(instance->env);
    analyzer_env_learn_floor(instance->env, instance->device, ANALYZER_LOCK_FLOOR_WINDOWS);
}

static void analyzer_lock_sweep_fine(AnalyzerLockWorker* instance, uint32_t frequency) {
    AnalyzerModel probe = analyzer_lock_probe(frequency, 58036u, instance->atten, 0);
    const uint32_t rbw = analyzer_model_bandwidth_hz(&probe);
    const uint32_t settle = analyzer_model_settle_ms(rbw);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    if(instance->state.survey.centre_hz != frequency || !instance->state.survey_passes) {
        analyzer_survey_reset(&instance->state.survey, frequency, rbw);
        instance->state.survey_passes = 0;
    }
    const uint16_t pass = instance->state.survey_passes;
    furi_mutex_release(instance->mutex);

    for(uint16_t i = 0; i < ANALYZER_SURVEY_BINS; i++) {
        if(!instance->running || instance->restart) return;

        const uint16_t bin =
            (uint16_t)((i * ANALYZER_LOCK_BIN_STRIDE + pass) % ANALYZER_SURVEY_BINS);

        probe.freq_offset = analyzer_survey_bin_freqoff(bin);
        analyzer_lock_push(instance, &probe);
        furi_delay_ms(settle);

        const int8_t raw = analyzer_env_level_raw(instance->device);

        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        analyzer_survey_accumulate(&instance->state.survey, bin, raw);
        instance->state.level_raw = raw;
        furi_mutex_release(instance->mutex);
    }

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->state.survey_passes++;
    analyzer_survey_finish(&instance->state.survey, analyzer_env_floor_dbm(instance->env));
    furi_mutex_release(instance->mutex);
}

static void
    analyzer_lock_sweep_wide(AnalyzerLockWorker* instance, uint32_t centre, AnalyzerZoom zoom) {
    const uint32_t filter = analyzer_band_filter_hz(zoom);
    const uint32_t settle = analyzer_model_settle_ms(filter);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    if(instance->state.band.centre_hz != centre || instance->state.band.zoom != zoom) {
        analyzer_band_reset(&instance->state.band, centre, zoom);
    }
    furi_mutex_release(instance->mutex);

    for(uint16_t bin = 0; bin < ANALYZER_BAND_BINS; bin++) {
        if(!instance->running || instance->restart) return;

        const uint32_t frequency = analyzer_band_bin_frequency(zoom, centre, bin);
        if(!analyzer_model_frequency_valid(frequency)) continue;

        const AnalyzerModel probe = analyzer_lock_probe(frequency, filter, instance->atten, 0);
        analyzer_lock_push(instance, &probe);
        furi_delay_ms(settle);

        const int8_t raw = analyzer_env_level_raw(instance->device);

        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        analyzer_band_accumulate(&instance->state.band, bin, raw);
        instance->state.level_raw = raw;
        furi_mutex_release(instance->mutex);
    }

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    analyzer_band_finish(&instance->state.band);
    furi_mutex_release(instance->mutex);
}

static void analyzer_lock_listen(AnalyzerLockWorker* instance, uint32_t frequency) {
    const AnalyzerModel probe = analyzer_lock_probe(frequency, 270833u, instance->atten, 0);
    analyzer_lock_push(instance, &probe);
    furi_delay_ms(ANALYZER_LOCK_SETTLE_MS);

    uint8_t overloaded_run = 0;

    for(uint8_t i = 0; i < ANALYZER_LOCK_WINDOWS_PER_SWEEP; i++) {
        if(!instance->running || instance->restart) return;

        AnalyzerEnvWindow window;
        analyzer_env_probe(instance->env, instance->device, &window);

        if(window.overloaded) {
            if(++overloaded_run >= ANALYZER_LOCK_OVERLOAD_RUN &&
               (instance->atten + 1u) < ANALYZER_LOCK_ATTEN_STEPS) {
                instance->atten++;
                overloaded_run = 0;
                FURI_LOG_I(TAG, "front end overloaded, atten -> %u", instance->atten);
                analyzer_lock_learn_floor(instance, frequency);
                return;
            }
        } else {
            overloaded_run = 0;
        }

        uint8_t confidence = 0;
        const AnalyzerEnvVote vote = analyzer_env_verdict(instance->env, &confidence);

        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->state.vote = vote;
        instance->state.confidence = confidence;
        instance->state.level_raw = window.p90;
        instance->state.overloaded = window.overloaded;
        instance->state.floor_dbm = analyzer_env_floor_dbm(instance->env);
        instance->state.peak_dbm = analyzer_env_peak_dbm(instance->env);
        instance->state.atten = instance->atten;
        furi_mutex_release(instance->mutex);
    }
}

static int32_t analyzer_lock_worker_thread(void* context) {
    AnalyzerLockWorker* instance = context;

    subghz_devices_reset(instance->device);
    subghz_devices_idle(instance->device);

    uint32_t frequency = instance->frequency;

    AnalyzerModel probe = analyzer_lock_probe(frequency, 270833u, 0, 0);
    analyzer_model_build(&probe, instance->blob, sizeof(instance->blob));
    subghz_devices_load_preset(instance->device, FuriHalSubGhzPresetCustom, instance->blob);
    subghz_devices_set_frequency(instance->device, frequency);
    subghz_devices_flush_rx(instance->device);
    subghz_devices_start_async_rx(instance->device, subghz_worker_rx_callback, instance->capture);
    subghz_worker_start(instance->capture);
    subghz_devices_hop_begin(instance->device);

    analyzer_lock_learn_floor(instance, frequency);

    while(instance->running) {
        if(instance->restart) {
            instance->restart = false;
            frequency = instance->frequency;

            furi_mutex_acquire(instance->mutex, FuriWaitForever);
            instance->state.frequency = frequency;
            instance->state.survey_passes = 0;
            memset(&instance->state.survey, 0, sizeof(instance->state.survey));
            analyzer_band_reset(&instance->state.band, frequency, instance->zoom);
            instance->state.sweeps = 0;
            furi_mutex_release(instance->mutex);

            analyzer_lock_learn_floor(instance, frequency);
            continue;
        }

        const AnalyzerZoom zoom = (AnalyzerZoom)instance->zoom;

        analyzer_lock_listen(instance, frequency);
        if(!instance->running || instance->restart) continue;

        if(analyzer_band_is_fine(zoom)) {
            analyzer_lock_sweep_fine(instance, frequency);
        } else {
            analyzer_lock_sweep_wide(instance, frequency, zoom);
        }

        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        instance->state.zoom = zoom;
        instance->state.sweeps++;
        furi_mutex_release(instance->mutex);
    }

    subghz_devices_hop_end(instance->device);
    subghz_worker_stop(instance->capture);
    subghz_devices_stop_async_rx(instance->device);
    subghz_devices_idle(instance->device);
    return 0;
}

AnalyzerLockWorker* analyzer_lock_worker_alloc(const SubGhzDevice* device) {
    AnalyzerLockWorker* instance = malloc(sizeof(AnalyzerLockWorker));
    memset(instance, 0, sizeof(AnalyzerLockWorker));

    instance->device = device;
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->env = analyzer_env_alloc();
    instance->capture = subghz_worker_alloc();
    instance->thread =
        furi_thread_alloc_ex("SubGhzAnalyzerLock", 3072, analyzer_lock_worker_thread, instance);

    return instance;
}

void analyzer_lock_worker_free(AnalyzerLockWorker* instance) {
    furi_check(instance);
    if(instance->running) analyzer_lock_worker_stop(instance);

    furi_thread_free(instance->thread);
    subghz_worker_free(instance->capture);
    analyzer_env_free(instance->env);
    furi_mutex_free(instance->mutex);
    free(instance);
}

AnalyzerEnv* analyzer_lock_worker_env(AnalyzerLockWorker* instance) {
    furi_check(instance);
    return instance->env;
}

void analyzer_lock_worker_start(
    AnalyzerLockWorker* instance,
    uint32_t frequency,
    AnalyzerZoom zoom) {
    furi_check(instance);
    if(instance->running) return;

    instance->frequency = frequency;
    instance->zoom = (uint8_t)zoom;
    instance->restart = false;
    instance->atten = 0;

    memset(&instance->state, 0, sizeof(instance->state));
    instance->state.frequency = frequency;
    instance->state.zoom = zoom;
    analyzer_band_reset(&instance->state.band, frequency, zoom);

    instance->running = true;
    furi_thread_start(instance->thread);
}

void analyzer_lock_worker_stop(AnalyzerLockWorker* instance) {
    furi_check(instance);
    if(!instance->running) return;
    instance->running = false;
    furi_thread_join(instance->thread);
}

bool analyzer_lock_worker_is_running(const AnalyzerLockWorker* instance) {
    furi_check(instance);
    return instance->running;
}

void analyzer_lock_worker_retune(AnalyzerLockWorker* instance, uint32_t frequency) {
    furi_check(instance);
    instance->frequency = frequency;
    instance->restart = true;
}

void analyzer_lock_worker_set_zoom(AnalyzerLockWorker* instance, AnalyzerZoom zoom) {
    furi_check(instance);
    if(zoom >= AnalyzerZoomCount) return;
    instance->zoom = (uint8_t)zoom;
    instance->restart = true;
}

void analyzer_lock_worker_get_state(AnalyzerLockWorker* instance, AnalyzerLockState* out) {
    furi_check(instance);
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    *out = instance->state;
    furi_mutex_release(instance->mutex);
}

void analyzer_lock_worker_fill_session(AnalyzerLockWorker* instance, AnalyzerSession* session) {
    furi_check(instance);
    furi_check(session);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    session->env_vote = instance->state.vote;
    session->env_confidence = instance->state.confidence;
    session->atten = instance->state.atten;
    session->floor_dbm = instance->state.floor_dbm;
    session->peak_dbm = instance->state.peak_dbm;
    session->survey = instance->state.survey;
    session->survey_passes = instance->state.survey_passes;
    furi_mutex_release(instance->mutex);

    session->floor_raw = analyzer_env_get_floor_raw(instance->env);
    session->lock_valid = true;

    analyzer_session_derive_seed(session);
}
