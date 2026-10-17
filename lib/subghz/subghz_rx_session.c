#include "subghz_rx_session.h"

#include "environment.h"
#include "receiver.h"
#include "subghz_protocol_registry.h"
#include "subghz_setting.h"
#include "subghz_worker.h"
#include "transmitter.h"
#include "types.h"
#include "devices/devices.h"
#include "devices/radio_device_loader.h"
#include "protocols/base.h"

#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <furi.h>
#include <lib/drivers/cc1101_regs.h>
#include <furi_hal_power.h>
#include <furi_hal_region.h>
#include <furi_hal_resources.h>
#include <furi_hal_speaker.h>
#include <loader/loader.h>
#include <storage/storage.h>
#include <toolbox/level_duration.h>
#include <toolbox/stream/stream.h>

#include <string.h>

#define TAG "SubGhzRxSession"

#define SUBGHZ_RX_DEFAULT_FREQUENCY     433920000U
#define SUBGHZ_RX_DEFAULT_PRESET        "AM650"
#define SUBGHZ_RX_RSSI_INTERVAL_DEFAULT 20U
#define SUBGHZ_RX_RSSI_INTERVAL_MIN     10U
#define SUBGHZ_RX_RSSI_INTERVAL_MAX     1000U
#define SUBGHZ_RX_RSSI_FLUSH_MS         40U
#define SUBGHZ_RX_PAIR_STREAM_SAMPLES   4096U
#define SUBGHZ_RX_PAIR_BACKPRESSURE     (SUBGHZ_RX_PAIR_STREAM_SAMPLES / 2)
#define SUBGHZ_RX_PAIRS_PER_PASS        256U
#define SUBGHZ_RX_HOP_DWELL_MS          200U
#define SUBGHZ_RX_NOISE_FLOOR_ALPHA     16
#define SUBGHZ_RX_SETTING_FILE          EXT_PATH("subghz/assets/setting_user")

typedef LevelDuration SubGhzRxPair;

typedef enum {
    SubGhzRxFlagStop = (1 << 0),
    SubGhzRxFlagRetune = (1 << 1),
} SubGhzRxFlag;

struct SubGhzRxSession {
    SubGhzRxSessionCallbacks callbacks;
    void* context;

    FuriThread* thread;
    FuriStreamBuffer* pairs;
    FuriMutex* config_mutex;
    FuriSemaphore* pause_ack;
    FuriSemaphore* resume;
    volatile bool pause_request;

    SubGhzSetting* setting;
    SubGhzEnvironment* environment;
    SubGhzReceiver* receiver;
    SubGhzWorker* worker;
    const SubGhzDevice* device;

    Loader* loader;
    bool loader_locked;
    bool charge_suppressed;
    bool running;
    bool radio_started;
    bool rx_running;

    SubGhzRadioPreset preset;
    FuriString* preset_name;
    FuriString* device_name;

    bool hopping;
    uint32_t ignore_filter;
    bool modulation_hopping;
    bool sound;
    bool speaker_acquired;
    int preset_index;
    int preset_index_b;
    FuriString* preset_name_b;
    uint32_t hop_frequencies[SUBGHZ_RX_HOP_FREQUENCY_MAX];
    size_t hop_count;
    size_t hop_index;
    uint32_t hop_stamp;

    uint32_t rssi_interval_ms;
    bool capture_pulses;
    bool external_device;

    uint32_t* pulse_ring;
    size_t pulse_head;
    size_t pulse_used;
    uint32_t* pulse_scratch;

    int8_t rssi_batch[SUBGHZ_RX_RSSI_BATCH_MAX];
    size_t rssi_count;
    int32_t noise_floor_acc;
    bool noise_floor_seeded;

    bool tx_active;
    volatile bool pairs_overrun;
};

static int8_t subghz_rx_session_clamp_dbm(float rssi) {
    if(rssi > 0) return 0;
    if(rssi < -127) return -127;
    return (int8_t)rssi;
}

static const char* subghz_rx_session_preset_alias(const char* name) {
    static const struct {
        const char* full;
        const char* shortname;
    } aliases[] = {
        {"FuriHalSubGhzPresetOok270Async", "AM270"},
        {"FuriHalSubGhzPresetOok650Async", "AM650"},
        {"FuriHalSubGhzPreset2FSKDev238Async", "FM238"},
        {"FuriHalSubGhzPreset2FSKDev476Async", "FM476"},
        {"FuriHalSubGhzPreset2FSKDev12KAsync", "FM12K"},
    };
    for(size_t i = 0; i < COUNT_OF(aliases); i++) {
        if(strcmp(name, aliases[i].full) == 0) return aliases[i].shortname;
    }
    return name;
}

static int subghz_rx_session_find_preset(SubGhzRxSession* instance, const char* name) {
    if(!name || name[0] == '\0') return -1;
    const char* wanted = subghz_rx_session_preset_alias(name);
    const size_t count = subghz_setting_get_preset_count(instance->setting);
    for(size_t i = 0; i < count; i++) {
        const char* candidate = subghz_setting_get_preset_name(instance->setting, i);
        if(candidate && strcmp(candidate, wanted) == 0) return (int)i;
    }
    return -1;
}

static int subghz_rx_session_resolve_preset(SubGhzRxSession* instance, const char* name) {
    int index = subghz_rx_session_find_preset(instance, name);
    if(index >= 0) return index;
    if(name && name[0] != '\0') {
        FURI_LOG_W(TAG, "Unknown preset \"%s\", using " SUBGHZ_RX_DEFAULT_PRESET, name);
    }
    return subghz_rx_session_find_preset(instance, SUBGHZ_RX_DEFAULT_PRESET);
}

static void subghz_rx_session_use_preset_locked(SubGhzRxSession* instance, int index) {
    instance->preset.name = instance->preset_name;
    if(index < 0) {
        furi_string_set_str(instance->preset_name, SUBGHZ_RX_DEFAULT_PRESET);
        instance->preset.data = NULL;
        instance->preset.data_size = 0;
        return;
    }
    furi_string_set_str(
        instance->preset_name, subghz_setting_get_preset_name(instance->setting, index));
    instance->preset.data = subghz_setting_get_preset_data(instance->setting, index);
    instance->preset.data_size = subghz_setting_get_preset_data_size(instance->setting, index);
}

static void subghz_rx_session_apply_preset_locked(
    SubGhzRxSession* instance,
    const char* name,
    const char* name_b) {
    instance->preset_index = subghz_rx_session_resolve_preset(instance, name);
    instance->preset_index_b = instance->modulation_hopping ?
                                   subghz_rx_session_resolve_preset(instance, name_b) :
                                   instance->preset_index;
    furi_string_set_str(
        instance->preset_name_b,
        instance->preset_index_b < 0 ?
            SUBGHZ_RX_DEFAULT_PRESET :
            subghz_setting_get_preset_name(instance->setting, instance->preset_index_b));
    subghz_rx_session_use_preset_locked(instance, instance->preset_index);
}

static void subghz_rx_session_swap_preset_locked(SubGhzRxSession* instance) {
    if(instance->preset_index == instance->preset_index_b) return;
    const int current = instance->preset_index;
    instance->preset_index = instance->preset_index_b;
    instance->preset_index_b = current;
    furi_string_set_str(
        instance->preset_name_b,
        instance->preset_index_b < 0 ?
            SUBGHZ_RX_DEFAULT_PRESET :
            subghz_setting_get_preset_name(instance->setting, instance->preset_index_b));
    subghz_rx_session_use_preset_locked(instance, instance->preset_index);
}

static bool subghz_rx_session_preset_is_ask(const uint8_t* preset_data) {
    if(preset_data == NULL) return false;

    for(size_t i = 0; preset_data[i]; i += 2) {
        if(preset_data[i] == CC1101_MDMCFG2) {
            return ((preset_data[i + 1] >> 4) & 0x07) == 3;
        }
    }
    return false;
}

static SubGhzProtocolFlag subghz_rx_session_band_flag(uint32_t frequency) {
    const SubGhzProtocolFlag all = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 |
                                   SubGhzProtocolFlag_868;

    if(frequency >= 280000000UL && frequency <= 350000000UL) return SubGhzProtocolFlag_315;
    if(frequency >= 380000000UL && frequency <= 480000000UL) return SubGhzProtocolFlag_433;
    if(frequency >= 750000000UL && frequency <= 950000000UL) return SubGhzProtocolFlag_868;
    return all;
}

static SubGhzProtocolFlag
    subghz_rx_session_gate_mask(uint32_t frequency, const uint8_t* preset_data) {
    SubGhzProtocolFlag mod = SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM;
    if(preset_data) {
        mod = subghz_rx_session_preset_is_ask(preset_data) ? SubGhzProtocolFlag_AM :
                                                             SubGhzProtocolFlag_FM;
    }
    return mod | subghz_rx_session_band_flag(frequency);
}

static void subghz_rx_session_speaker_set(SubGhzRxSession* instance, bool on) {
    if(on == instance->speaker_acquired) return;
    if(on) {
        if(!furi_hal_speaker_acquire(30)) {
            FURI_LOG_W(TAG, "Speaker busy, sound off");
            instance->sound = false;
            return;
        }
        instance->speaker_acquired = true;
        if(instance->device) {
            subghz_devices_set_async_mirror_pin(instance->device, &gpio_speaker);
        }
        return;
    }
    if(instance->device) subghz_devices_set_async_mirror_pin(instance->device, NULL);
    furi_hal_speaker_release();
    instance->speaker_acquired = false;
}

static void subghz_rx_session_radio_configure(SubGhzRxSession* instance, uint32_t frequency) {
    subghz_devices_idle(instance->device);
    if(instance->preset.data) {
        subghz_devices_load_preset(
            instance->device, FuriHalSubGhzPresetCustom, instance->preset.data);
    } else {
        subghz_devices_load_preset(instance->device, FuriHalSubGhzPresetOok650Async, NULL);
    }
    instance->preset.frequency = subghz_devices_set_frequency(instance->device, frequency);
    subghz_devices_flush_rx(instance->device);
}

static void subghz_rx_session_rx_begin(SubGhzRxSession* instance, uint32_t frequency) {
    subghz_rx_session_radio_configure(instance, frequency);
    if(instance->speaker_acquired) {
        subghz_devices_set_async_mirror_pin(instance->device, &gpio_speaker);
    }
    subghz_receiver_set_gate(
        instance->receiver,
        subghz_rx_session_gate_mask(instance->preset.frequency, instance->preset.data));
    subghz_receiver_reset(instance->receiver);
    furi_stream_buffer_reset(instance->pairs);
    instance->pairs_overrun = false;
    subghz_devices_start_async_rx(instance->device, subghz_worker_rx_callback, instance->worker);
    subghz_worker_start(instance->worker);
    instance->rx_running = true;
}

static void subghz_rx_session_rx_end(SubGhzRxSession* instance) {
    if(!instance->rx_running) return;
    if(subghz_worker_is_running(instance->worker)) subghz_worker_stop(instance->worker);
    subghz_devices_stop_async_rx(instance->device);
    if(instance->speaker_acquired) {
        subghz_devices_set_async_mirror_pin(instance->device, NULL);
    }
    subghz_devices_idle(instance->device);
    instance->rx_running = false;
}

static void subghz_rx_session_retune(SubGhzRxSession* instance, uint32_t frequency) {
    subghz_rx_session_rx_end(instance);
    subghz_rx_session_rx_begin(instance, frequency);
}

static void
    subghz_rx_session_pulse_push(SubGhzRxSession* instance, bool level, uint32_t duration) {
    if(!instance->pulse_ring) return;
    if(duration > SUBGHZ_RX_PULSE_DURATION_MASK) duration = SUBGHZ_RX_PULSE_DURATION_MASK;
    uint32_t entry = duration;
    if(level) entry |= SUBGHZ_RX_PULSE_LEVEL_BIT;
    instance->pulse_ring[instance->pulse_head] = entry;
    instance->pulse_head = (instance->pulse_head + 1) % SUBGHZ_RX_PULSE_CAPTURE_MAX;
    if(instance->pulse_used < SUBGHZ_RX_PULSE_CAPTURE_MAX) instance->pulse_used++;
}

static size_t subghz_rx_session_pulse_snapshot(SubGhzRxSession* instance) {
    if(!instance->pulse_ring || !instance->pulse_scratch || instance->pulse_used == 0) return 0;
    const size_t count = instance->pulse_used;
    size_t start =
        (instance->pulse_head + SUBGHZ_RX_PULSE_CAPTURE_MAX - count) % SUBGHZ_RX_PULSE_CAPTURE_MAX;
    for(size_t i = 0; i < count; i++) {
        instance->pulse_scratch[i] = instance->pulse_ring[start];
        start = (start + 1) % SUBGHZ_RX_PULSE_CAPTURE_MAX;
    }
    return count;
}

static char* subghz_rx_session_dump_format(FlipperFormat* format) {
    Stream* stream = flipper_format_get_raw_stream(format);
    if(!stream) return NULL;
    stream_seek(stream, 0, StreamOffsetFromStart);
    const size_t size = stream_size(stream);
    char* buf = malloc(size + 1);
    if(!buf) return NULL;
    const size_t read = stream_read(stream, (uint8_t*)buf, size);
    buf[read] = '\0';
    return buf;
}

static void subghz_rx_session_flush_rssi(SubGhzRxSession* instance) {
    if(instance->rssi_count == 0) return;
    if(instance->callbacks.on_rssi) {
        instance->callbacks.on_rssi(
            instance->rssi_batch,
            instance->rssi_count,
            instance->preset.frequency,
            instance->noise_floor_acc / SUBGHZ_RX_NOISE_FLOOR_ALPHA,
            instance->context);
    }
    instance->rssi_count = 0;
}

static void subghz_rx_session_overrun_callback(void* context) {
    SubGhzRxSession* instance = context;
    const SubGhzRxPair reset = level_duration_reset();
    if(furi_stream_buffer_send(instance->pairs, &reset, sizeof(reset), 0) != sizeof(reset)) {
        instance->pairs_overrun = true;
    }
}

static void subghz_rx_session_pair_callback(void* context, bool level, uint32_t duration) {
    SubGhzRxSession* instance = context;

    SubGhzRxPair pair = level_duration_make(level, duration);
    if(instance->pairs_overrun) {
        instance->pairs_overrun = false;
        pair = level_duration_reset();
    }
    if(furi_stream_buffer_send(instance->pairs, &pair, sizeof(pair), 0) != sizeof(pair)) {
        instance->pairs_overrun = true;
    }
}

static void subghz_rx_session_on_rx(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    SubGhzRxSession* instance = context;

    const SubGhzProtocol* protocol = decoder_base->protocol;
    if(!protocol || protocol->type == SubGhzProtocolTypeRAW) {
        subghz_receiver_reset(receiver);
        return;
    }
    if(instance->ignore_filter && (protocol->flag & instance->ignore_filter) != 0) {
        subghz_receiver_reset(receiver);
        return;
    }

    FlipperFormat* format = flipper_format_string_alloc();
    char* serialized = NULL;
    do {
        if(subghz_protocol_decoder_base_serialize(decoder_base, format, &instance->preset) !=
           SubGhzProtocolStatusOk) {
            break;
        }
        serialized = subghz_rx_session_dump_format(format);
        if(!serialized || serialized[0] == '\0') break;
        if(!instance->callbacks.on_hit) break;

        size_t pulse_count = 0;
        if(instance->capture_pulses) pulse_count = subghz_rx_session_pulse_snapshot(instance);

        const SubGhzRxHit hit = {
            .protocol = protocol->name ? protocol->name : "",
            .flipper_format = serialized,
            .frequency = instance->preset.frequency,
            .rssi = subghz_rx_session_clamp_dbm(subghz_devices_get_rssi(instance->device)),
            .timestamp_ms = furi_get_tick(),
            .transmittable = (protocol->flag & SubGhzProtocolFlag_Send) != 0,
            .pulses = pulse_count ? instance->pulse_scratch : NULL,
            .pulse_count = pulse_count,
        };
        subghz_rx_session_flush_rssi(instance);
        instance->callbacks.on_hit(&hit, instance->context);
    } while(false);

    free(serialized);
    flipper_format_free(format);
    subghz_receiver_reset(receiver);

    instance->hop_stamp = furi_get_tick();
}

static void subghz_rx_session_hop(SubGhzRxSession* instance) {
    if(!instance->hopping || instance->hop_count == 0) return;
    const uint32_t now = furi_get_tick();
    if(now - instance->hop_stamp < furi_ms_to_ticks(SUBGHZ_RX_HOP_DWELL_MS)) return;
    instance->hop_stamp = now;
    instance->hop_index = (instance->hop_index + 1) % instance->hop_count;

    subghz_rx_session_flush_rssi(instance);

    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    if(instance->modulation_hopping) subghz_rx_session_swap_preset_locked(instance);
    subghz_rx_session_retune(instance, instance->hop_frequencies[instance->hop_index]);
    furi_mutex_release(instance->config_mutex);
}

static void subghz_rx_session_pump_pause(SubGhzRxSession* instance) {
    if(!instance->thread) return;
    instance->pause_request = true;
    furi_semaphore_acquire(instance->pause_ack, FuriWaitForever);
}

static void subghz_rx_session_pump_resume(SubGhzRxSession* instance) {
    if(!instance->thread || !instance->pause_request) return;
    instance->pause_request = false;
    furi_semaphore_release(instance->resume);
}

static int32_t subghz_rx_session_pump(void* context) {
    SubGhzRxSession* instance = context;

    uint32_t rssi_stamp = furi_get_tick();
    uint32_t flush_stamp = rssi_stamp;

    while(true) {
        if(instance->pause_request) {
            furi_semaphore_release(instance->pause_ack);
            furi_semaphore_acquire(instance->resume, FuriWaitForever);
        }

        const uint32_t flags = furi_thread_flags_get();
        if(flags & SubGhzRxFlagStop) break;

        SubGhzRxPair pair;
        size_t got = furi_stream_buffer_receive(instance->pairs, &pair, sizeof(pair), 2);
        uint32_t drained = 0;
        while(got == sizeof(pair)) {
            if(level_duration_is_reset(pair)) {
                subghz_receiver_reset(instance->receiver);
                instance->pulse_used = 0;
                instance->pulse_head = 0;
            } else {
                const bool level = level_duration_get_level(pair);
                const uint32_t duration = level_duration_get_duration(pair);
                if(instance->capture_pulses) {
                    subghz_rx_session_pulse_push(instance, level, duration);
                }
                subghz_receiver_decode(instance->receiver, level, duration);
            }
            if(++drained >= SUBGHZ_RX_PAIRS_PER_PASS) break;
            got = furi_stream_buffer_receive(instance->pairs, &pair, sizeof(pair), 0);
        }
        if(drained >= SUBGHZ_RX_PAIRS_PER_PASS) furi_thread_yield();

        const uint32_t now = furi_get_tick();

        if(!instance->tx_active &&
           now - rssi_stamp >= furi_ms_to_ticks(instance->rssi_interval_ms)) {
            rssi_stamp = now;
            furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
            const bool sampled = !instance->tx_active;
            const int8_t sample =
                sampled ? subghz_rx_session_clamp_dbm(subghz_devices_get_rssi(instance->device)) :
                          0;
            furi_mutex_release(instance->config_mutex);

            if(sampled) {
                if(!instance->noise_floor_seeded) {
                    instance->noise_floor_acc = (int32_t)sample * SUBGHZ_RX_NOISE_FLOOR_ALPHA;
                    instance->noise_floor_seeded = true;
                } else {
                    instance->noise_floor_acc +=
                        sample - (instance->noise_floor_acc / SUBGHZ_RX_NOISE_FLOOR_ALPHA);
                }
                if(instance->rssi_count < SUBGHZ_RX_RSSI_BATCH_MAX) {
                    instance->rssi_batch[instance->rssi_count++] = sample;
                }
            }
        }

        const bool congested = furi_stream_buffer_bytes_available(instance->pairs) >=
                               SUBGHZ_RX_PAIR_BACKPRESSURE * sizeof(SubGhzRxPair);
        if(!congested && (instance->rssi_count >= SUBGHZ_RX_RSSI_BATCH_MAX ||
                          (instance->rssi_count > 0 &&
                           now - flush_stamp >= furi_ms_to_ticks(SUBGHZ_RX_RSSI_FLUSH_MS)))) {
            flush_stamp = now;
            subghz_rx_session_flush_rssi(instance);
        }

        if(!instance->tx_active) subghz_rx_session_hop(instance);
    }

    subghz_rx_session_flush_rssi(instance);
    if(instance->callbacks.on_stopped) instance->callbacks.on_stopped(instance->context);
    return 0;
}

SubGhzRxSession*
    subghz_rx_session_alloc(const SubGhzRxSessionCallbacks* callbacks, void* context) {
    SubGhzRxSession* instance = malloc(sizeof(SubGhzRxSession));
    memset(instance, 0, sizeof(SubGhzRxSession));

    if(callbacks) instance->callbacks = *callbacks;
    instance->context = context;
    instance->config_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->pause_ack = furi_semaphore_alloc(1, 0);
    instance->resume = furi_semaphore_alloc(1, 0);
    instance->preset_name = furi_string_alloc_set_str(SUBGHZ_RX_DEFAULT_PRESET);
    instance->preset_name_b = furi_string_alloc_set_str(SUBGHZ_RX_DEFAULT_PRESET);
    instance->device_name = furi_string_alloc();
    instance->preset.name = instance->preset_name;
    instance->preset.frequency = SUBGHZ_RX_DEFAULT_FREQUENCY;
    instance->rssi_interval_ms = SUBGHZ_RX_RSSI_INTERVAL_DEFAULT;

    instance->setting = subghz_setting_alloc();
    subghz_setting_load(instance->setting, SUBGHZ_RX_SETTING_FILE);

    instance->environment = subghz_environment_alloc();
    subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_DIR_NAME);
    subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_DIR_USER_NAME);
    subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_KIA_DIR_NAME);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        instance->environment, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(
        instance->environment, SUBGHZ_NICE_FLOR_S_DIR_NAME);
    subghz_environment_set_protocol_registry(instance->environment, &subghz_protocol_registry);

    instance->receiver = subghz_receiver_alloc_init(instance->environment);
    subghz_receiver_set_filter(instance->receiver, SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_rx_callback(instance->receiver, subghz_rx_session_on_rx, instance);

    instance->worker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(instance->worker, subghz_rx_session_overrun_callback);
    subghz_worker_set_pair_callback(instance->worker, subghz_rx_session_pair_callback);
    subghz_worker_set_context(instance->worker, instance);

    instance->pairs = furi_stream_buffer_alloc(
        sizeof(SubGhzRxPair) * SUBGHZ_RX_PAIR_STREAM_SAMPLES, sizeof(SubGhzRxPair));

    instance->loader = furi_record_open(RECORD_LOADER);
    return instance;
}

void subghz_rx_session_free(SubGhzRxSession* instance) {
    furi_assert(instance);
    subghz_rx_session_stop(instance);

    furi_record_close(RECORD_LOADER);
    furi_stream_buffer_free(instance->pairs);
    subghz_worker_free(instance->worker);
    subghz_receiver_free(instance->receiver);
    subghz_environment_free(instance->environment);
    subghz_setting_free(instance->setting);
    furi_string_free(instance->preset_name);
    furi_string_free(instance->preset_name_b);
    furi_string_free(instance->device_name);
    furi_mutex_free(instance->config_mutex);
    furi_semaphore_free(instance->pause_ack);
    furi_semaphore_free(instance->resume);
    free(instance->pulse_ring);
    free(instance->pulse_scratch);
    free(instance);
}

static void subghz_rx_session_set_pulse_capture(SubGhzRxSession* instance, bool enabled) {
    instance->capture_pulses = enabled;
    instance->pulse_head = 0;
    instance->pulse_used = 0;
    if(enabled && !instance->pulse_ring) {
        instance->pulse_ring = malloc(sizeof(uint32_t) * SUBGHZ_RX_PULSE_CAPTURE_MAX);
        instance->pulse_scratch = malloc(sizeof(uint32_t) * SUBGHZ_RX_PULSE_CAPTURE_MAX);
        if(!instance->pulse_ring || !instance->pulse_scratch) {
            free(instance->pulse_ring);
            free(instance->pulse_scratch);
            instance->pulse_ring = NULL;
            instance->pulse_scratch = NULL;
            instance->capture_pulses = false;
        }
    }
}

static void subghz_rx_session_load_hops(SubGhzRxSession* instance, const SubGhzRxConfig* config) {
    instance->hop_count = 0;
    instance->hop_index = 0;
    if(!config->hopping) {
        instance->hopping = false;
        return;
    }
    if(config->hop_frequencies && config->hop_count > 0) {
        for(size_t i = 0;
            i < config->hop_count && instance->hop_count < SUBGHZ_RX_HOP_FREQUENCY_MAX;
            i++) {
            if(!subghz_devices_is_frequency_valid(instance->device, config->hop_frequencies[i])) {
                continue;
            }
            instance->hop_frequencies[instance->hop_count++] = config->hop_frequencies[i];
        }
    } else {
        const size_t count = subghz_setting_get_hopper_frequency_count(instance->setting);
        for(size_t i = 0; i < count && instance->hop_count < SUBGHZ_RX_HOP_FREQUENCY_MAX; i++) {
            const uint32_t frequency = subghz_setting_get_hopper_frequency(instance->setting, i);
            if(!subghz_devices_is_frequency_valid(instance->device, frequency)) continue;
            instance->hop_frequencies[instance->hop_count++] = frequency;
        }
    }
    instance->hopping = instance->hop_count > 1;
}

SubGhzRxSessionStatus
    subghz_rx_session_start(SubGhzRxSession* instance, const SubGhzRxConfig* config) {
    furi_assert(instance);
    furi_assert(config);

    if(instance->running) return SubGhzRxSessionStatusErrorBusy;
    if(loader_is_locked(instance->loader)) return SubGhzRxSessionStatusErrorBusy;
    if(!loader_lock(instance->loader)) return SubGhzRxSessionStatusErrorBusy;
    instance->loader_locked = true;

    subghz_devices_init();
    const bool want_external = config->external_device || config->auto_device;
    instance->device = radio_device_loader_set(
        NULL, want_external ? SubGhzRadioDeviceTypeExternalCC1101 : SubGhzRadioDeviceTypeInternal);
    if(!instance->device) {
        subghz_devices_deinit();
        loader_unlock(instance->loader);
        instance->loader_locked = false;
        return SubGhzRxSessionStatusErrorNoDevice;
    }
    instance->external_device = radio_device_loader_is_external(instance->device);
    if(config->external_device && !config->auto_device && !instance->external_device) {
        radio_device_loader_end(instance->device);
        subghz_devices_deinit();
        loader_unlock(instance->loader);
        instance->loader_locked = false;
        instance->device = NULL;
        return SubGhzRxSessionStatusErrorNoDevice;
    }
    furi_string_set_str(instance->device_name, subghz_devices_get_name(instance->device));

    const uint32_t frequency = config->frequency ? config->frequency : SUBGHZ_RX_DEFAULT_FREQUENCY;
    if(!subghz_devices_is_frequency_valid(instance->device, frequency)) {
        radio_device_loader_end(instance->device);
        subghz_devices_deinit();
        loader_unlock(instance->loader);
        instance->loader_locked = false;
        instance->device = NULL;
        return SubGhzRxSessionStatusErrorInvalid;
    }

    instance->rssi_interval_ms = config->rssi_interval_ms ? config->rssi_interval_ms :
                                                            SUBGHZ_RX_RSSI_INTERVAL_DEFAULT;
    if(instance->rssi_interval_ms < SUBGHZ_RX_RSSI_INTERVAL_MIN) {
        instance->rssi_interval_ms = SUBGHZ_RX_RSSI_INTERVAL_MIN;
    } else if(instance->rssi_interval_ms > SUBGHZ_RX_RSSI_INTERVAL_MAX) {
        instance->rssi_interval_ms = SUBGHZ_RX_RSSI_INTERVAL_MAX;
    }
    subghz_rx_session_set_pulse_capture(instance, config->capture_pulses);
    instance->ignore_filter = config->ignore_filter;
    instance->modulation_hopping = config->modulation_hopping;
    instance->sound = config->sound;
    subghz_rx_session_apply_preset_locked(instance, config->preset, config->preset_b);
    subghz_rx_session_load_hops(instance, config);

    instance->rssi_count = 0;
    instance->noise_floor_seeded = false;
    instance->hop_stamp = furi_get_tick();
    furi_stream_buffer_reset(instance->pairs);
    subghz_receiver_reset(instance->receiver);

    furi_hal_power_suppress_charge_enter();
    instance->charge_suppressed = true;

    subghz_devices_reset(instance->device);
    instance->radio_started = true;
    if(instance->sound) subghz_rx_session_speaker_set(instance, true);
    subghz_rx_session_rx_begin(
        instance, instance->hopping ? instance->hop_frequencies[0] : frequency);

    instance->running = true;
    instance->thread =
        furi_thread_alloc_ex("SubGhzRxSession", 6144, subghz_rx_session_pump, instance);
    furi_thread_set_priority(instance->thread, FuriThreadPriorityNormal);
    furi_thread_start(instance->thread);

    FURI_LOG_I(
        TAG,
        "RX %lu Hz preset=%s hop=%d",
        instance->preset.frequency,
        furi_string_get_cstr(instance->preset_name),
        instance->hopping);
    return SubGhzRxSessionStatusOk;
}

SubGhzRxSessionStatus
    subghz_rx_session_reconfigure(SubGhzRxSession* instance, const SubGhzRxConfig* config) {
    furi_assert(instance);
    furi_assert(config);
    if(!instance->running) return SubGhzRxSessionStatusErrorInvalid;

    const uint32_t frequency = config->frequency ? config->frequency : instance->preset.frequency;
    if(!subghz_devices_is_frequency_valid(instance->device, frequency)) {
        return SubGhzRxSessionStatusErrorInvalid;
    }

    subghz_rx_session_pump_pause(instance);
    furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
    instance->rssi_count = 0;
    instance->noise_floor_seeded = false;
    subghz_rx_session_set_pulse_capture(instance, config->capture_pulses);
    instance->ignore_filter = config->ignore_filter;
    instance->modulation_hopping = config->modulation_hopping;
    if(config->preset && config->preset[0] != '\0') {
        subghz_rx_session_apply_preset_locked(instance, config->preset, config->preset_b);
    }
    subghz_rx_session_load_hops(instance, config);
    if(config->sound != instance->sound) {
        instance->sound = config->sound;
        subghz_rx_session_speaker_set(instance, instance->sound);
    }
    subghz_rx_session_retune(
        instance, instance->hopping ? instance->hop_frequencies[instance->hop_index] : frequency);
    furi_mutex_release(instance->config_mutex);
    subghz_rx_session_pump_resume(instance);
    return SubGhzRxSessionStatusOk;
}

void subghz_rx_session_stop(SubGhzRxSession* instance) {
    furi_assert(instance);
    if(!instance->running) return;
    instance->running = false;

    if(instance->thread) {
        subghz_rx_session_pump_resume(instance);
        furi_thread_flags_set(furi_thread_get_id(instance->thread), SubGhzRxFlagStop);
        furi_thread_join(instance->thread);
        furi_thread_free(instance->thread);
        instance->thread = NULL;
    }

    subghz_rx_session_rx_end(instance);
    subghz_rx_session_speaker_set(instance, false);
    if(instance->radio_started) {
        subghz_devices_idle(instance->device);
        subghz_devices_sleep(instance->device);
        instance->radio_started = false;
    }
    if(instance->device) {
        radio_device_loader_end(instance->device);
        subghz_devices_deinit();
        instance->device = NULL;
    }
    if(instance->charge_suppressed) {
        furi_hal_power_suppress_charge_exit();
        instance->charge_suppressed = false;
    }
    if(instance->loader_locked) {
        loader_unlock(instance->loader);
        instance->loader_locked = false;
    }
    FURI_LOG_I(TAG, "RX stopped");
}

bool subghz_rx_session_is_running(SubGhzRxSession* instance) {
    furi_assert(instance);
    return instance->running;
}

void subghz_rx_session_get_state(SubGhzRxSession* instance, SubGhzRxState* state) {
    furi_assert(instance);
    furi_assert(state);
    memset(state, 0, sizeof(*state));
    state->running = instance->running;
    state->frequency = instance->preset.frequency;
    state->preset = furi_string_get_cstr(instance->preset_name);
    state->hopping = instance->hopping;
    state->external_device = instance->external_device;
    state->device_name = furi_string_get_cstr(instance->device_name);
    state->ignore_filter = instance->ignore_filter;
    state->modulation_hopping = instance->modulation_hopping;
    state->preset_b = furi_string_get_cstr(instance->preset_name_b);
    state->sound = instance->sound;
    state->tx_allowed = instance->running &&
                        furi_hal_region_is_frequency_allowed(instance->preset.frequency);
}

SubGhzRxSessionStatus subghz_rx_session_transmit(
    SubGhzRxSession* instance,
    FlipperFormat* format,
    uint32_t repeat,
    bool external_device,
    uint32_t* out_frequency) {
    furi_assert(instance);
    furi_assert(format);

    FuriString* protocol = furi_string_alloc();
    FuriString* preset_name = furi_string_alloc();
    SubGhzTransmitter* transmitter = NULL;
    SubGhzRxSessionStatus status = SubGhzRxSessionStatusErrorInvalid;
    uint32_t frequency = 0;
    bool standalone = !instance->running;
    bool charge_suppressed = false;
    bool loader_locked = false;

    do {
        flipper_format_rewind(format);
        if(!flipper_format_read_string(format, "Protocol", protocol)) break;
        furi_string_trim(protocol);
        if(furi_string_empty(protocol) || furi_string_cmp_str(protocol, "RAW") == 0) break;

        flipper_format_rewind(format);
        if(!flipper_format_read_uint32(format, "Frequency", &frequency, 1)) {
            frequency = instance->preset.frequency;
        }
        flipper_format_rewind(format);
        if(flipper_format_read_string(format, "Preset", preset_name)) {
            furi_string_trim(preset_name);
        }

        if(repeat == 0) repeat = 3;
        flipper_format_rewind(format);
        if(!flipper_format_insert_or_update_uint32(format, "Repeat", &repeat, 1)) break;

        if(standalone) {
            if(loader_is_locked(instance->loader) || !loader_lock(instance->loader)) {
                status = SubGhzRxSessionStatusErrorBusy;
                break;
            }
            loader_locked = true;
            subghz_devices_init();
            instance->device = radio_device_loader_set(
                NULL,
                external_device ? SubGhzRadioDeviceTypeExternalCC1101 :
                                  SubGhzRadioDeviceTypeInternal);
            if(!instance->device) {
                status = SubGhzRxSessionStatusErrorNoDevice;
                break;
            }
            subghz_devices_reset(instance->device);
            furi_hal_power_suppress_charge_enter();
            charge_suppressed = true;
        }

        if(!subghz_devices_is_frequency_valid(instance->device, frequency)) break;
        if(!furi_hal_region_is_frequency_allowed(frequency)) {
            status = SubGhzRxSessionStatusErrorTxForbidden;
            break;
        }

        transmitter =
            subghz_transmitter_alloc_init(instance->environment, furi_string_get_cstr(protocol));
        if(!transmitter) break;
        flipper_format_rewind(format);
        if(subghz_transmitter_deserialize(transmitter, format) != SubGhzProtocolStatusOk) break;

        subghz_rx_session_pump_pause(instance);
        furi_mutex_acquire(instance->config_mutex, FuriWaitForever);
        instance->tx_active = true;
        if(!standalone) subghz_rx_session_rx_end(instance);
        subghz_devices_idle(instance->device);
        const int preset_index = subghz_rx_session_find_preset(
            instance,
            furi_string_empty(preset_name) ? SUBGHZ_RX_DEFAULT_PRESET :
                                             furi_string_get_cstr(preset_name));
        if(preset_index >= 0) {
            subghz_devices_load_preset(
                instance->device,
                FuriHalSubGhzPresetCustom,
                subghz_setting_get_preset_data(instance->setting, preset_index));
        } else {
            subghz_devices_load_preset(instance->device, FuriHalSubGhzPresetOok650Async, NULL);
        }
        frequency = subghz_devices_set_frequency(instance->device, frequency);

        if(subghz_devices_start_async_tx(instance->device, subghz_transmitter_yield, transmitter)) {
            while(!subghz_devices_is_async_complete_tx(instance->device)) {
                furi_delay_ms(20);
            }
            subghz_devices_stop_async_tx(instance->device);
            status = SubGhzRxSessionStatusOk;
        } else {
            status = SubGhzRxSessionStatusErrorTxForbidden;
        }

        if(!standalone) subghz_rx_session_rx_begin(instance, instance->preset.frequency);
        instance->tx_active = false;
        furi_mutex_release(instance->config_mutex);
        subghz_rx_session_pump_resume(instance);
    } while(false);

    subghz_rx_session_pump_resume(instance);
    if(transmitter) subghz_transmitter_free(transmitter);
    if(standalone) {
        if(charge_suppressed) furi_hal_power_suppress_charge_exit();
        if(instance->device) {
            subghz_devices_idle(instance->device);
            subghz_devices_sleep(instance->device);
            radio_device_loader_end(instance->device);
            subghz_devices_deinit();
            instance->device = NULL;
        }
        if(loader_locked) loader_unlock(instance->loader);
    }
    instance->tx_active = false;

    if(out_frequency) *out_frequency = frequency;
    furi_string_free(preset_name);
    furi_string_free(protocol);
    return status;
}
