#include "subghz_txrx_i.h" // IWYU pragma: keep

#include <lib/subghz/protocols/protocol_items.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/drivers/subghz/si4463_ext/si4463_ext_interconnect.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/registry.h>
#include <string.h>
#include <lib/subghz/blocks/custom_btn.h>
#include <lib/drivers/cc1101_regs.h>
#include <math.h>
#include <furi_hal_gpio.h>
#include <furi_hal_cortex.h>

const char* radio_device_loader_external_name(void);

#define TAG "SubGhzTxRx"

static void subghz_txrx_hopper_thread_stop(SubGhzTxRx* instance);

#define SUBGHZ_HOPPER_GRID_MS 4

#define SUBGHZ_HOPPER_DWELL_MS 8

#define SUBGHZ_HOPPER_HOLD_MS 400

#define SUBGHZ_HOPPER_HANGOVER_MS 60

#define SUBGHZ_HOPPER_HYSTERESIS_DB 6.0f

#define SUBGHZ_HOPPER_FREQ_DWELL_MS       8
#define SUBGHZ_HOPPER_FREQ_HOLD_MS        1000
#define SUBGHZ_HOPPER_FREQ_DECODE_HOLD_MS 150
#define SUBGHZ_HOPPER_FREQ_MARGIN_DB      8.0f

#define SUBGHZ_HOPPER_LATCH_SAMPLES 2

#define SUBGHZ_HOPPER_BUSY_MARGIN_DB 5.0f
#define SUBGHZ_HOPPER_FLOOR_ALPHA    0.05f

#define SUBGHZ_HOPPER_MAD_ALPHA       0.05f
#define SUBGHZ_HOPPER_MAD_K           4.0f
#define SUBGHZ_HOPPER_MARGIN_MAX_DB   18.0f
#define SUBGHZ_HOPPER_HYSTERESIS_FRAC 0.35f

#define SUBGHZ_HOPPER_FLOOR_TRACK_K      2.0f
#define SUBGHZ_HOPPER_FLOOR_TRACK_MIN_DB 3.0f
#define SUBGHZ_HOPPER_FLOOR_TRACK_MAX_DB 6.0f

#define SUBGHZ_HOPPER_FLOOR_NOISE_ALPHA 0.5f

#define SUBGHZ_HOPPER_FLOOR_NOISE_STEP_DB 3.0f

#define SUBGHZ_HOPPER_CLASSIFY_SAMPLES   128
#define SUBGHZ_HOPPER_CLASSIFY_PERIOD_US 100

#define SUBGHZ_HOPPER_CLASSIFY_SNR_DB 10.0f

#define SUBGHZ_HOPPER_ASK_CARRIER_DB       8.0f
#define SUBGHZ_HOPPER_FSK_CARRIER_DB       3.0f
#define SUBGHZ_HOPPER_CLASSIFY_COOLDOWN_MS 50
#define SUBGHZ_HOPPER_CLASSIFY_ARM_DB      8.0f

#define SUBGHZ_HOPPER_AIR_MEMORY_MS 400

#define SUBGHZ_HOPPER_CLASSIFIED_HOLD_MS 900

#define SUBGHZ_HOPPER_FSK_STRICT_DB     1.0f
#define SUBGHZ_HOPPER_FSK_STRICT_SNR_DB 30.0f

#define SUBGHZ_HOPPER_FSK_CEILING_DBM (-25.0f)

#define SUBGHZ_HOPPER_ACT_MATCH_MIN 5

#define SUBGHZ_HOPPER_ACT_LONG_US 16000

#define SUBGHZ_HOPPER_ACT_MIN_EDGES 8

static float subghz_txrx_hopper_margin(float mad) {
    float margin = SUBGHZ_HOPPER_MAD_K * mad;
    if(margin < SUBGHZ_HOPPER_BUSY_MARGIN_DB) margin = SUBGHZ_HOPPER_BUSY_MARGIN_DB;
    if(margin > SUBGHZ_HOPPER_MARGIN_MAX_DB) margin = SUBGHZ_HOPPER_MARGIN_MAX_DB;
    return margin;
}

static float subghz_txrx_hopper_hysteresis(float margin) {
    return margin * SUBGHZ_HOPPER_HYSTERESIS_FRAC;
}

static float subghz_txrx_hopper_track_gate(float mad) {
    float gate = SUBGHZ_HOPPER_FLOOR_TRACK_K * mad;
    if(gate < SUBGHZ_HOPPER_FLOOR_TRACK_MIN_DB) gate = SUBGHZ_HOPPER_FLOOR_TRACK_MIN_DB;
    if(gate > SUBGHZ_HOPPER_FLOOR_TRACK_MAX_DB) gate = SUBGHZ_HOPPER_FLOOR_TRACK_MAX_DB;
    return gate;
}

static void subghz_txrx_capture_tap(bool level, uint32_t duration, void* context) {
    SubGhzTxRx* instance = context;

    if(instance->hopper_thread_running) {
        instance->hopper_act_edges++;
        if(duration >= SUBGHZ_HOPPER_ACT_LONG_US) instance->hopper_act_long++;

        const uint32_t prev = instance->hopper_act_prev_us;
        if(prev && duration) {
            const uint32_t hi = (prev > duration) ? prev : duration;
            const uint32_t lo = (prev > duration) ? duration : prev;
            if(hi <= lo + (lo >> 2)) instance->hopper_act_match++;
        }
        instance->hopper_act_prev_us = duration;
    }

    subghz_worker_rx_callback(level, duration, instance->worker);
}

static void subghz_txrx_hopper_activity_reset(SubGhzTxRx* instance) {
    instance->hopper_act_edges = 0;
    instance->hopper_act_match = 0;
    instance->hopper_act_long = 0;
    instance->hopper_act_prev_us = 0;
}

static bool subghz_txrx_hopper_activity_seen(SubGhzTxRx* instance) {
    if(instance->hopper_act_edges < SUBGHZ_HOPPER_ACT_MIN_EDGES) return false;
    if(instance->hopper_act_long) return true;
    return instance->hopper_act_match >= SUBGHZ_HOPPER_ACT_MATCH_MIN;
}

static SubGhzProtocolFlag subghz_txrx_modulation_mask(const uint8_t* preset_data) {
    if(preset_data) {
        for(size_t i = 0; preset_data[i]; i += 2) {
            if(preset_data[i] == CC1101_MDMCFG2) {
                switch((preset_data[i + 1] >> 4) & 0x07) {
                case 3: // ASK/OOK
                    return SubGhzProtocolFlag_AM;
                case 0: // 2-FSK
                case 1: // GFSK
                case 4: // 4-FSK
                case 7: // MSK
                    return SubGhzProtocolFlag_FM;
                default:
                    break;
                }
                break;
            }
        }
    }
    // Unknown modulation must not exclude a decoder.
    return SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM;
}

static SubGhzProtocolFlag subghz_txrx_band_flag(uint32_t frequency) {
    if(frequency >= 280000000UL && frequency <= 480000000UL)
        return SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433;
    if(frequency >= 750000000UL && frequency <= 950000000UL) return SubGhzProtocolFlag_868;
    return SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868;
}

#define SUBGHZ_GATE_ALL                                                     \
    (SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_315 | \
     SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868)

static bool subghz_txrx_preset_is_heavy(const uint8_t* preset_data) {
    if(!preset_data) return false;
    for(size_t i = 0; preset_data[i]; i += 2) {
        if(preset_data[i] == CC1101_MDMCFG2) {
            return ((preset_data[i + 1] >> 4) & 0x07) != 3;
        }
    }
    return false;
}

static SubGhzProtocolFlag subghz_txrx_gate_mask(uint32_t frequency, const uint8_t* preset_data) {
    if(!subghz_txrx_preset_is_heavy(preset_data)) return SUBGHZ_GATE_ALL;
    return subghz_txrx_modulation_mask(preset_data) | subghz_txrx_band_flag(frequency);
}

static bool subghz_txrx_preset_is_ask(const uint8_t* preset_data) {
    return subghz_txrx_modulation_mask(preset_data) == SubGhzProtocolFlag_AM;
}

static void subghz_txrx_commit_rx_preset(SubGhzTxRx* instance) {
    const SubGhzRadioPreset* preset = instance->preset;
    furi_string_set(instance->rx_preset.name, preset->name);
    instance->rx_preset.frequency = preset->frequency;
    instance->rx_preset.data_size = preset->data ? preset->data_size : 0;
    furi_check(instance->rx_preset.data_size <= sizeof(instance->rx_preset_data));
    if(instance->rx_preset.data_size) {
        memcpy(instance->rx_preset_data, preset->data, instance->rx_preset.data_size);
    }
    instance->rx_preset.data = instance->rx_preset.data_size ? instance->rx_preset_data : NULL;
}

static void subghz_txrx_worker_reset(void* context) {
    subghz_receiver_reset(((SubGhzTxRx*)context)->receiver);
}

static void subghz_txrx_worker_decode(void* context, bool level, uint32_t duration) {
    subghz_receiver_decode(((SubGhzTxRx*)context)->receiver, level, duration);
}

static void subghz_txrx_barrier_callback(void* context, uint32_t payload) {
    SubGhzTxRx* instance = context;
    subghz_txrx_hopper_lock(instance);
    subghz_txrx_commit_rx_preset(instance);
    subghz_receiver_set_gate(instance->receiver, (SubGhzProtocolFlag)payload);
    subghz_txrx_hopper_unlock(instance);
}

SubGhzRadioPreset subghz_txrx_get_rx_preset(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->rx_preset;
}

static void subghz_txrx_hopper_state_reset(SubGhzTxRx* instance) {
    instance->hopper_busy_until[0] = 0;
    instance->hopper_busy_until[1] = 0;
    instance->hopper_rssi_run[0] = 0;
    instance->hopper_rssi_run[1] = 0;
    instance->hopper_energy_hot[0] = false;
    instance->hopper_energy_hot[1] = false;
    instance->hopper_rssi_floor_valid[0] = false;
    instance->hopper_rssi_floor_valid[1] = false;
    instance->hopper_rssi_floor[0] = 0;
    instance->hopper_rssi_floor[1] = 0;
    instance->hopper_rssi_mad[0] = 0;
    instance->hopper_rssi_mad[1] = 0;
    instance->hopper_level_valid = false;
    instance->hopper_level_rssi = 0;
    instance->hopper_level_floor = 0;
    for(size_t i = 0; i < SUBGHZ_HOPPER_FREQ_FLOOR_MAX; i++) {
        instance->hopper_ch_floor[i] = 0;
        instance->hopper_ch_floor_valid[i] = false;
        instance->hopper_ch_mad[i] = 0;
    }
    instance->hopper_classify_next = 0;
    instance->hopper_air_fsk_until = 0;
    instance->hopper_slot_classified = false;
    instance->hopper_slot_decoded = false;
    instance->hopper_slot_activity = false;
    subghz_txrx_hopper_activity_reset(instance);
    instance->hopper_freq_hold_threshold = 0;
    instance->hopper_freq_quiet_since = 0;
    instance->hopper_decoded_until = 0;
}

static const char* subghz_txrx_protocol_name_alias(const char* name) {
    if(!strcmp(name, "Kia V3") || !strcmp(name, "Kia V4")) {
        return "Kia V3/V4";
    }
    return name;
}

static void subghz_txrx_radio_device_power_on(SubGhzTxRx* instance) {
    UNUSED(instance);
    uint8_t attempts = 0;
    while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
        furi_hal_power_enable_otg();
        //CC1101 power-up time
        furi_delay_ms(10);
    }
}

static void subghz_txrx_radio_device_power_off(SubGhzTxRx* instance) {
    UNUSED(instance);
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
}

SubGhzTxRx* subghz_txrx_alloc(void) {
    subghz_devices_init();
    SubGhzTxRx* instance = malloc(sizeof(SubGhzTxRx));
    instance->setting = subghz_setting_alloc();
    subghz_setting_load(instance->setting, EXT_PATH("subghz/assets/setting_user"));

    instance->preset = malloc(sizeof(SubGhzRadioPreset));
    instance->preset->name = furi_string_alloc();
    instance->rx_preset.name = furi_string_alloc();

    instance->hopper_idx_frequency = 0;
    instance->hopper_idx_preset = 0;
    instance->hopper_timeout = 0;
    instance->hopper_mode = SubGhzHopperModeFrequency;
    instance->hopper_state = SubGhzHopperStateOFF;
    instance->hopper_preset_a = 0;
    instance->hopper_preset_b = 0;
    instance->hopper_thread = NULL;
    instance->hopper_thread_running = false;
    instance->hopper_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->hopper_threshold = -90.0f;
    instance->hopper_classify_buf = NULL;
    subghz_txrx_hopper_state_reset(instance);
    instance->rx_callback = NULL;
    instance->rx_callback_context = NULL;
    instance->raw_gate_on = false;
    instance->raw_gate_ring = NULL;

    subghz_txrx_set_default_preset(instance, 0);

    instance->dual_active = false;
    instance->worker_ext = NULL;
    instance->receiver_ext = NULL;
    instance->radio_device_int = NULL;
    instance->radio_device_ext = NULL;
    instance->dual_mutex = NULL;
    instance->preset_ext = malloc(sizeof(SubGhzRadioPreset));
    instance->preset_ext->name = furi_string_alloc_set("FM476");
    instance->preset_ext->frequency = 433920000;
    instance->preset_ext->data = NULL;
    instance->preset_ext->data_size = 0;

    instance->txrx_state = SubGhzTxRxStateSleep;

    subghz_txrx_speaker_set_state(instance, SubGhzSpeakerStateDisable);
    subghz_txrx_set_debug_pin_state(instance, false);

    instance->worker = subghz_worker_alloc();
    instance->fff_data = flipper_format_string_alloc();
    instance->tx_from_internal_fff = false;

    instance->environment = subghz_environment_alloc();
    instance->is_database_loaded =
        subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_DIR_NAME);
    subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_DIR_USER_NAME);
    subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_KIA_DIR_NAME);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        instance->environment, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(
        instance->environment, SUBGHZ_NICE_FLOR_S_DIR_NAME);
    subghz_environment_set_protocol_registry(
        instance->environment, (void*)&subghz_protocol_registry);
    instance->receiver = subghz_receiver_alloc_init(instance->environment);

    subghz_worker_set_overrun_callback(instance->worker, subghz_txrx_worker_reset);
    subghz_worker_set_pair_callback(instance->worker, subghz_txrx_worker_decode);
    subghz_worker_set_barrier_callback(instance->worker, subghz_txrx_barrier_callback);
    subghz_worker_set_context(instance->worker, instance);

    //set default device External
    instance->radio_device_type = SubGhzRadioDeviceTypeInternal;
    instance->radio_device_type =
        subghz_txrx_radio_device_set(instance, SubGhzRadioDeviceTypeExternalCC1101);

    return instance;
}

void subghz_txrx_free(SubGhzTxRx* instance) {
    furi_assert(instance);

    subghz_txrx_hopper_thread_stop(instance);

    if(instance->dual_active) {
        subghz_txrx_dual_rx_stop(instance);
    }

    if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal) {
        subghz_txrx_radio_device_power_off(instance);
        subghz_devices_end(instance->radio_device);
    }

    subghz_devices_deinit();

    subghz_worker_free(instance->worker);
    subghz_receiver_free(instance->receiver);
    subghz_environment_free(instance->environment);
    free(instance->raw_gate_ring);
    flipper_format_free(instance->fff_data);
    furi_string_free(instance->rx_preset.name);
    furi_string_free(instance->preset->name);
    furi_string_free(instance->preset_ext->name);
    subghz_setting_free(instance->setting);
    furi_mutex_free(instance->hopper_mutex);

    free(instance->preset_ext);
    free(instance->preset);
    free(instance);
}

bool subghz_txrx_is_database_loaded(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->is_database_loaded;
}

void subghz_txrx_set_preset(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency,
    uint8_t* preset_data,
    size_t preset_data_size) {
    furi_assert(instance);

    const char* radio = subghz_txrx_radio_device_get_name(instance);
    if(radio && !strcmp(radio, SUBGHZ_DEVICE_SI4463_EXT_NAME)) {
        const char* si_name = subghz_setting_radio_counterpart(preset_name, true);
        const int si = si_name != preset_name ?
                           subghz_setting_get_inx_preset_by_name(instance->setting, si_name) :
                           -1;
        if(si >= 0) {
            uint8_t* si_data = subghz_setting_get_preset_data(instance->setting, si);
            const size_t si_size = subghz_setting_get_preset_data_size(instance->setting, si);
            if(preset_data && preset_data_size >= 8 && si_size >= 8)
                memcpy(&si_data[si_size - 8], &preset_data[preset_data_size - 8], 8);
            preset_name = si_name;
            preset_data = si_data;
            preset_data_size = si_size;
        }
    }

    subghz_txrx_hopper_lock(instance);

    furi_string_set(instance->preset->name, preset_name);

    SubGhzRadioPreset* preset = instance->preset;
    preset->frequency = frequency;
    preset->data = preset_data;
    preset->data_size = preset_data_size;

    subghz_txrx_hopper_unlock(instance);
}

void subghz_txrx_set_frequency(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);

    subghz_txrx_hopper_lock(instance);
    instance->preset->frequency = frequency;
    subghz_txrx_hopper_unlock(instance);
}

uint32_t subghz_txrx_get_frequency_hz(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->preset->frequency;
}

uint8_t*
    subghz_txrx_set_tx_power(uint8_t* preset_data, size_t preset_data_size, uint8_t tx_power) {
#define PRESET_POWER_OFFSET_FM 8
#define PRESET_POWER_OFFSET_AM 7
#define TX_PATABLE_OFFSET_AM   8
#define TX_PATABLE_COUNT       17

    //I had to skip the +10dBM and -6dBm Values, use only ones AM/FM have in common.
    //Highest Value is 12dBm for AM, 10 for FM. So Menu needs to reflect that.
    const uint8_t tx_pa_table[TX_PATABLE_COUNT] = {
        0,
        0xC0, //12dBm
        0xCD, //7dBm
        0x86, //5dBm
        0x50, //0dBm
        0x26, // -10dBm
        0x1D, // -15dBm
        0x17, //-20dBm
        0x03, //-30dBm
        0xC0, // 10dBm
        0xC8, //7dBm
        0x84, //5dBm
        0x60, //0dBm
        0x34, //-10dBm
        0x1D, //-15dBm
        0x0E, // -20dBm
        0x12, //-30dBm
    };

    //Grab the AM and FM byte now, so we can do proper checks.
    uint8_t fm_byte = preset_data[preset_data_size - PRESET_POWER_OFFSET_FM];
    uint8_t am_byte = preset_data[preset_data_size - PRESET_POWER_OFFSET_AM];

    //Set the TX Power Here in the CC1101 register...

    //If we have both bytes 1st bytes set or none, this isnt a preset we can deal with here.
    if(fm_byte && !am_byte) {
        //Use FM Table
        if(tx_power) {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_FM] =
                tx_pa_table[TX_PATABLE_OFFSET_AM + tx_power];
        } else {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_FM] =
                tx_pa_table[1]; //Max Power 0xC0 10dBm
        }
    } else if(am_byte && !fm_byte) {
        //Use AM Table
        if(tx_power) {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_AM] = tx_pa_table[tx_power];
        } else {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_AM] =
                tx_pa_table[1]; //Max Power 0xC0 12dBm
        }
    }

    //Pass back the preset_so we can call one liners.
    return preset_data;
}

const char* subghz_txrx_get_preset_name(SubGhzTxRx* instance, const char* preset) {
    UNUSED(instance);
    const char* preset_name = "";
    // Accept short names already stored in some .sub files
    if(!strcmp(preset, "AM270") || !strcmp(preset, "AM650") || !strcmp(preset, "AM101") ||
       !strcmp(preset, "FM238") || !strcmp(preset, "FM12K") || !strcmp(preset, "FM20K") ||
       !strcmp(preset, "FM20KR") || !strcmp(preset, "FM14K") || !strcmp(preset, "FM476") ||
       !strcmp(preset, "CUSTOM")) {
        preset_name = preset;
    } else if(!strcmp(preset, "FuriHalSubGhzPresetOok270Async")) {
        preset_name = "AM270";
    } else if(!strcmp(preset, "FuriHalSubGhzPresetOok650Async")) {
        preset_name = "AM650";
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev238Async")) {
        preset_name = "FM238";
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev12KAsync")) {
        preset_name = "FM12K";
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev476Async")) {
        preset_name = "FM476";
    } else if(!strcmp(preset, "FuriHalSubGhzPresetCustom")) {
        preset_name = "CUSTOM";
    } else {
        FURI_LOG_E(TAG, "Unknown preset");
    }
    return preset_name;
}

SubGhzRadioPreset subghz_txrx_get_preset(SubGhzTxRx* instance) {
    furi_assert(instance);
    return *instance->preset;
}

void subghz_txrx_get_frequency_and_modulation(
    SubGhzTxRx* instance,
    FuriString* frequency,
    FuriString* modulation,
    bool long_name) {
    furi_assert(instance);

    subghz_txrx_hopper_lock(instance);

    SubGhzRadioPreset* preset = instance->preset;
    if(frequency != NULL) {
        furi_string_printf(
            frequency,
            "%03ld.%02ld",
            preset->frequency / 1000000 % 1000,
            preset->frequency / 10000 % 100);
    }
    if(modulation != NULL) {
        if(long_name) {
            furi_string_printf(modulation, "%s", furi_string_get_cstr(preset->name));
        } else {
            furi_string_printf(modulation, "%.2s", furi_string_get_cstr(preset->name));
        }
    }

    subghz_txrx_hopper_unlock(instance);
}

static void subghz_txrx_begin(SubGhzTxRx* instance, uint8_t* preset_data) {
    furi_assert(instance);
    subghz_devices_reset(instance->radio_device);
    subghz_devices_idle(instance->radio_device);
    subghz_devices_load_preset(instance->radio_device, FuriHalSubGhzPresetCustom, preset_data);
    instance->txrx_state = SubGhzTxRxStateIDLE;
}

#define SUBGHZ_RAW_GATE_RING       512u
#define SUBGHZ_RAW_GATE_OPEN_DB    10
#define SUBGHZ_RAW_GATE_CLOSE_DB   6
#define SUBGHZ_RAW_GATE_PREROLL_US 2000u

#define SUBGHZ_RAW_GATE_LEVEL    (1UL << 31)
#define SUBGHZ_RAW_GATE_DURATION (SUBGHZ_RAW_GATE_LEVEL - 1UL)

static void subghz_txrx_worker_bind_receiver(SubGhzTxRx* instance) {
    subghz_worker_set_overrun_callback(instance->worker, subghz_txrx_worker_reset);
    subghz_worker_set_pair_callback(instance->worker, subghz_txrx_worker_decode);
    subghz_worker_set_barrier_callback(instance->worker, subghz_txrx_barrier_callback);
    subghz_worker_set_context(instance->worker, instance);
}

static void subghz_txrx_raw_gate_overrun(void* context) {
    subghz_receiver_reset(((SubGhzTxRx*)context)->receiver);
}

static void subghz_txrx_raw_gate_barrier(void* context, uint32_t payload) {
    subghz_txrx_barrier_callback(context, payload);
}

static void subghz_txrx_raw_gate_open(SubGhzTxRx* instance) {
    if(instance->raw_gate_open) return;
    instance->raw_gate_open = true;
    instance->raw_gate_low_hits = 0;
    uint16_t count = instance->raw_gate_ring_n;
    instance->raw_gate_ring_n = 0;
    if(!count || !instance->raw_gate_ring) {
        instance->raw_gate_ring_i = 0;
        return;
    }
    const uint16_t newest =
        (uint16_t)((instance->raw_gate_ring_i + SUBGHZ_RAW_GATE_RING - 1u) % SUBGHZ_RAW_GATE_RING);
    uint16_t keep = 0;
    uint32_t sum_us = 0;
    for(uint16_t i = 0; i < count; i++) {
        const uint16_t idx =
            (uint16_t)((newest + SUBGHZ_RAW_GATE_RING - i) % SUBGHZ_RAW_GATE_RING);
        const uint32_t edge = instance->raw_gate_ring[idx];
        keep++;
        sum_us += edge & SUBGHZ_RAW_GATE_DURATION;
        if(sum_us >= SUBGHZ_RAW_GATE_PREROLL_US) break;
    }
    const uint16_t first =
        (uint16_t)((newest + SUBGHZ_RAW_GATE_RING - (keep - 1u)) % SUBGHZ_RAW_GATE_RING);
    for(uint16_t i = 0; i < keep; i++) {
        const uint32_t edge = instance->raw_gate_ring[(first + i) % SUBGHZ_RAW_GATE_RING];
        subghz_receiver_decode(
            instance->receiver, edge & SUBGHZ_RAW_GATE_LEVEL, edge & SUBGHZ_RAW_GATE_DURATION);
    }
    instance->raw_gate_ring_i = 0;
}

static void subghz_txrx_raw_gate_consider(SubGhzTxRx* instance) {
    uint32_t now = furi_get_tick();
    uint32_t period = furi_ms_to_ticks(instance->raw_gate_open ? 100u : 15u);
    if((uint32_t)(now - instance->raw_gate_last_rssi) < period) return;
    instance->raw_gate_last_rssi = now;

    float rssi = subghz_devices_get_rssi(instance->radio_device);
    if(rssi <= -127.0f) return;
    int db = (int)rssi;
    if(!instance->raw_gate_have_floor) {
        instance->raw_gate_floor_db = db;
        instance->raw_gate_have_floor = true;
        return;
    }
    if(!instance->raw_gate_open) {
        if(db < instance->raw_gate_floor_db) instance->raw_gate_floor_db = db;
        if(db > instance->raw_gate_floor_db + SUBGHZ_RAW_GATE_OPEN_DB) {
            subghz_txrx_raw_gate_open(instance);
        }
    } else if(db < instance->raw_gate_floor_db + SUBGHZ_RAW_GATE_CLOSE_DB) {
        if(instance->raw_gate_low_hits < 2) instance->raw_gate_low_hits++;
        if(instance->raw_gate_low_hits >= 2) {
            instance->raw_gate_open = false;
            instance->raw_gate_low_hits = 0;
            instance->raw_gate_ring_n = 0;
            instance->raw_gate_ring_i = 0;
            subghz_receiver_decode(instance->receiver, false, 50000);
        }
    } else {
        instance->raw_gate_low_hits = 0;
    }
}

static void subghz_txrx_raw_gate_pair(void* context, bool level, uint32_t duration) {
    SubGhzTxRx* instance = context;
    if(!instance->raw_gate_on) {
        subghz_receiver_decode(instance->receiver, level, duration);
        return;
    }
    subghz_txrx_raw_gate_consider(instance);
    if(!instance->raw_gate_open) {
        if(instance->raw_gate_ring_n < SUBGHZ_RAW_GATE_RING) instance->raw_gate_ring_n++;
        instance->raw_gate_ring[instance->raw_gate_ring_i] =
            (level ? SUBGHZ_RAW_GATE_LEVEL : 0) | MIN(duration, SUBGHZ_RAW_GATE_DURATION);
        instance->raw_gate_ring_i =
            (uint16_t)((instance->raw_gate_ring_i + 1u) % SUBGHZ_RAW_GATE_RING);
        return;
    }
    subghz_receiver_decode(instance->receiver, level, duration);
}

static void subghz_txrx_worker_bind_raw_gate(SubGhzTxRx* instance) {
    subghz_worker_set_overrun_callback(instance->worker, subghz_txrx_raw_gate_overrun);
    subghz_worker_set_pair_callback(instance->worker, subghz_txrx_raw_gate_pair);
    subghz_worker_set_barrier_callback(instance->worker, subghz_txrx_raw_gate_barrier);
    subghz_worker_set_context(instance->worker, instance);
}

void subghz_txrx_raw_si_gate_set(SubGhzTxRx* instance, bool on) {
    furi_assert(instance);
    if(on && !instance->raw_gate_ring) {
        instance->raw_gate_ring = malloc(sizeof(uint32_t) * SUBGHZ_RAW_GATE_RING);
    } else if(!on && instance->raw_gate_ring && !subghz_worker_is_running(instance->worker)) {
        free(instance->raw_gate_ring);
        instance->raw_gate_ring = NULL;
    }
    instance->raw_gate_on = on;
    instance->raw_gate_open = false;
    instance->raw_gate_have_floor = false;
    instance->raw_gate_floor_db = 0;
    instance->raw_gate_low_hits = 0;
    instance->raw_gate_last_rssi = 0;
    instance->raw_gate_ring_n = 0;
    instance->raw_gate_ring_i = 0;
}

bool subghz_txrx_raw_si_gate_is_on(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->raw_gate_on;
}

static uint32_t subghz_txrx_rx(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    furi_assert(
        instance->txrx_state != SubGhzTxRxStateRx && instance->txrx_state != SubGhzTxRxStateSleep);

    subghz_devices_idle(instance->radio_device);

    uint32_t value = subghz_devices_set_frequency(instance->radio_device, frequency);
    subghz_devices_flush_rx(instance->radio_device);
    subghz_txrx_speaker_on(instance);

    subghz_txrx_commit_rx_preset(instance);
    subghz_receiver_set_gate(
        instance->receiver, subghz_txrx_gate_mask(frequency, instance->preset->data));

    subghz_txrx_hopper_activity_reset(instance);
    if(instance->raw_gate_on)
        subghz_txrx_worker_bind_raw_gate(instance);
    else
        subghz_txrx_worker_bind_receiver(instance);
    subghz_worker_set_priority(
        instance->worker,
        subghz_txrx_preset_is_heavy(instance->preset->data) ? FuriThreadPriorityNormal :
                                                              FuriThreadPriorityHigh);
    subghz_worker_start(instance->worker);
    subghz_devices_start_async_rx(instance->radio_device, subghz_txrx_capture_tap, instance);
    instance->txrx_state = SubGhzTxRxStateRx;
    return value;
}

static void subghz_txrx_idle(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->txrx_state != SubGhzTxRxStateSleep) {
        subghz_devices_idle(instance->radio_device);
        subghz_txrx_speaker_off(instance);
        instance->txrx_state = SubGhzTxRxStateIDLE;
    }
}

static void subghz_txrx_rx_end(SubGhzTxRx* instance) {
    furi_assert(instance);
    furi_assert(instance->txrx_state == SubGhzTxRxStateRx);

    const bool worker_running = subghz_worker_is_running(instance->worker);
    if(worker_running) subghz_devices_stop_async_rx(instance->radio_device);
    subghz_devices_idle(instance->radio_device);
    subghz_txrx_speaker_off(instance);
    instance->txrx_state = SubGhzTxRxStateIDLE;

    if(worker_running) {
        subghz_txrx_hopper_unlock(instance);
        subghz_worker_stop(instance->worker);
        subghz_txrx_hopper_lock(instance);
    }
    subghz_txrx_worker_bind_receiver(instance);
}

void subghz_txrx_sleep(SubGhzTxRx* instance) {
    furi_assert(instance);
    subghz_devices_sleep(instance->radio_device);
    instance->txrx_state = SubGhzTxRxStateSleep;
}

static bool subghz_txrx_tx(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    furi_assert(instance->txrx_state != SubGhzTxRxStateSleep);

    subghz_devices_idle(instance->radio_device);
    subghz_devices_set_frequency(instance->radio_device, frequency);

    bool ret = subghz_devices_set_tx(instance->radio_device);
    if(ret) {
        subghz_txrx_speaker_on(instance);
        instance->txrx_state = SubGhzTxRxStateTx;
    }

    return ret;
}

SubGhzTxRxStartTxState subghz_txrx_tx_start(SubGhzTxRx* instance, FlipperFormat* flipper_format) {
    furi_assert(instance);
    furi_assert(flipper_format);

    subghz_txrx_stop(instance);

    instance->tx_from_internal_fff = (flipper_format == instance->fff_data);

    SubGhzTxRxStartTxState ret = SubGhzTxRxStartTxStateErrorParserOthers;
    FuriString* temp_str = furi_string_alloc();
    do {
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            break;
        }
        ret = SubGhzTxRxStartTxStateOk;

        SubGhzRadioPreset* preset = instance->preset;
        instance->transmitter = subghz_transmitter_alloc_init(
            instance->environment,
            subghz_txrx_protocol_name_alias(furi_string_get_cstr(temp_str)));

        if(instance->transmitter) {
            flipper_format_rewind(flipper_format);
            if(subghz_transmitter_deserialize(instance->transmitter, flipper_format) ==
               SubGhzProtocolStatusOk) {
                if(strcmp(furi_string_get_cstr(preset->name), "") != 0) {
                    subghz_txrx_begin(instance, preset->data);
                    if(preset->frequency) {
                        if(!subghz_txrx_tx(instance, preset->frequency)) {
                            FURI_LOG_E(TAG, "Only Rx");
                            ret = SubGhzTxRxStartTxStateErrorOnlyRx;
                        }
                    } else {
                        ret = SubGhzTxRxStartTxStateErrorParserOthers;
                    }

                } else {
                    FURI_LOG_E(
                        TAG, "Unknown name preset \" %s \"", furi_string_get_cstr(preset->name));
                    ret = SubGhzTxRxStartTxStateErrorParserOthers;
                }

                if(ret == SubGhzTxRxStartTxStateOk) {
                    //Start TX
                    if(!subghz_devices_start_async_tx(
                           instance->radio_device, subghz_transmitter_yield, instance->transmitter)) {
                        ret = SubGhzTxRxStartTxStateErrorParserOthers;
                    }
                }
            } else {
                ret = SubGhzTxRxStartTxStateErrorParserOthers;
            }
        } else {
            ret = SubGhzTxRxStartTxStateErrorParserOthers;
        }
        if(ret != SubGhzTxRxStartTxStateOk) {
            if(instance->transmitter) {
                subghz_transmitter_free(instance->transmitter);
                instance->transmitter = NULL;
            }
            if(instance->txrx_state != SubGhzTxRxStateIDLE) {
                subghz_txrx_idle(instance);
            }
        }

    } while(false);
    furi_string_free(temp_str);
    return ret;
}

void subghz_txrx_rssi_start(SubGhzTxRx* instance) {
    subghz_txrx_stop(instance);
    subghz_txrx_hopper_lock(instance);
    subghz_txrx_begin(instance, instance->preset->data);
    subghz_devices_set_frequency(instance->radio_device, instance->preset->frequency);
    subghz_devices_flush_rx(instance->radio_device);
    subghz_devices_set_rx(instance->radio_device);
    instance->txrx_state = SubGhzTxRxStateRx;
    subghz_txrx_hopper_unlock(instance);
}

void subghz_txrx_rx_start(SubGhzTxRx* instance) {
    furi_assert(instance);
    subghz_txrx_stop(instance);

    subghz_txrx_hopper_lock(instance);
    subghz_receiver_reset(instance->receiver);
    subghz_txrx_begin(
        instance,
        subghz_setting_get_preset_data_by_name(
            subghz_txrx_get_setting(instance), furi_string_get_cstr(instance->preset->name)));
    subghz_txrx_rx(instance, instance->preset->frequency);
    subghz_txrx_hopper_unlock(instance);
}

void subghz_txrx_rx_retune(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);

    subghz_txrx_hopper_lock(instance);

    if(instance->txrx_state == SubGhzTxRxStateRx) {
        subghz_txrx_rx_end(instance);
    }
    if(instance->txrx_state == SubGhzTxRxStateIDLE) {
        subghz_receiver_reset(instance->receiver);
        instance->preset->frequency = frequency;
        subghz_txrx_rx(instance, frequency);
    }

    subghz_txrx_hopper_unlock(instance);
}

void subghz_txrx_set_need_save_callback(
    SubGhzTxRx* instance,
    SubGhzTxRxNeedSaveCallback callback,
    void* context) {
    furi_assert(instance);
    instance->need_save_callback = callback;
    instance->need_save_context = context;
}

static void subghz_txrx_tx_stop(SubGhzTxRx* instance) {
    furi_assert(instance);
    furi_assert(instance->txrx_state == SubGhzTxRxStateTx);
    //Stop TX
    subghz_devices_stop_async_tx(instance->radio_device);
    subghz_transmitter_stop(instance->transmitter);
    subghz_transmitter_free(instance->transmitter);
    instance->transmitter = NULL;

    //if protocol dynamic then we save the last upload
    if(instance->tx_from_internal_fff && instance->decoder_result &&
       instance->decoder_result->protocol->type == SubGhzProtocolTypeDynamic) {
        if(instance->need_save_callback) {
            instance->need_save_callback(instance->need_save_context);
        }
    }
    subghz_txrx_idle(instance);
    subghz_txrx_speaker_off(instance);
}

FlipperFormat* subghz_txrx_get_fff_data(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->fff_data;
}

SubGhzSetting* subghz_txrx_get_setting(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->setting;
}

void subghz_txrx_stop(SubGhzTxRx* instance) {
    furi_assert(instance);

    if(instance->dual_active) {
        subghz_txrx_dual_rx_stop(instance);
        return;
    }

    subghz_txrx_hopper_lock(instance);

    switch(instance->txrx_state) {
    case SubGhzTxRxStateTx:
        subghz_txrx_tx_stop(instance);
        subghz_txrx_speaker_unmute(instance);
        break;
    case SubGhzTxRxStateRx:
        subghz_txrx_rx_end(instance);
        subghz_txrx_speaker_mute(instance);
        break;

    default:
        break;
    }

    subghz_txrx_hopper_unlock(instance);
}

static void subghz_txrx_hopper_apply(
    SubGhzTxRx* instance,
    uint32_t frequency,
    uint8_t* preset_data,
    bool preset_changed) {
    const bool can_hop = subghz_devices_hop_is_supported(instance->radio_device);

    const SubGhzProtocolFlag gate = subghz_txrx_gate_mask(frequency, preset_data);
    subghz_worker_set_priority(
        instance->worker,
        subghz_txrx_preset_is_heavy(preset_data) ? FuriThreadPriorityNormal :
                                                   FuriThreadPriorityHigh);

    if(can_hop && instance->txrx_state == SubGhzTxRxStateRx &&
       subghz_devices_hop_goto(instance->radio_device, frequency, preset_data)) {
        subghz_worker_flush(instance->worker, gate);
        subghz_devices_hop_resume(instance->radio_device);
        instance->preset->frequency = frequency;
        return;
    }

    if(instance->txrx_state == SubGhzTxRxStateRx && subghz_worker_is_running(instance->worker)) {
        subghz_devices_stop_async_rx(instance->radio_device);
        if(preset_changed) {
            subghz_txrx_begin(instance, preset_data);
        } else {
            subghz_devices_idle(instance->radio_device);
        }
        subghz_devices_set_frequency(instance->radio_device, frequency);
        subghz_devices_flush_rx(instance->radio_device);
        subghz_worker_retune(instance->worker, gate);
        subghz_txrx_hopper_activity_reset(instance);
        subghz_devices_start_async_rx(instance->radio_device, subghz_txrx_capture_tap, instance);
        instance->txrx_state = SubGhzTxRxStateRx;
        instance->preset->frequency = frequency;
        return;
    }

    if(instance->txrx_state == SubGhzTxRxStateRx) {
        subghz_txrx_rx_end(instance);
    }
    if(instance->txrx_state == SubGhzTxRxStateIDLE) {
        subghz_receiver_reset(instance->receiver);

        subghz_receiver_set_gate(instance->receiver, gate);
        subghz_txrx_begin(instance, preset_data);
        instance->preset->frequency = frequency;
        subghz_txrx_rx(instance, instance->preset->frequency);
    }
}

static void subghz_txrx_hopper_use_preset(SubGhzTxRx* instance, uint8_t slot) {
    const size_t idx = slot ? instance->hopper_preset_b : instance->hopper_preset_a;
    if(idx >= subghz_setting_get_preset_count(instance->setting)) return;
    instance->hopper_idx_preset = slot;

    uint8_t* preset_data = subghz_setting_get_preset_data(instance->setting, idx);

    furi_string_set(
        instance->preset->name, subghz_setting_get_preset_name(instance->setting, idx));
    instance->preset->data = preset_data;
    instance->preset->data_size = subghz_setting_get_preset_data_size(instance->setting, idx);

    subghz_txrx_hopper_apply(instance, instance->preset->frequency, preset_data, true);
}

static void subghz_txrx_hopper_step_preset(SubGhzTxRx* instance) {
    subghz_txrx_hopper_use_preset(instance, instance->hopper_idx_preset ? 0 : 1);
}

static void subghz_txrx_hopper_step_frequency(SubGhzTxRx* instance) {
    const size_t count = subghz_setting_get_hopper_frequency_count(instance->setting);
    if(count == 0) return;

    uint32_t frequency = 0;
    for(size_t tries = 0; tries < count; tries++) {
        instance->hopper_idx_frequency = (instance->hopper_idx_frequency + 1) % count;
        const uint32_t candidate =
            subghz_setting_get_hopper_frequency(instance->setting, instance->hopper_idx_frequency);
        if(subghz_devices_is_frequency_valid(instance->radio_device, candidate)) {
            frequency = candidate;
            break;
        }
    }
    if(frequency == 0) return;
    uint8_t* preset_data = subghz_setting_get_preset_data_by_name(
        instance->setting, furi_string_get_cstr(instance->preset->name));

    subghz_txrx_hopper_apply(instance, frequency, preset_data, false);
}

static float subghz_txrx_hopper_freq_margin(SubGhzTxRx* instance, size_t ch) {
    float margin = SUBGHZ_HOPPER_MAD_K * instance->hopper_ch_mad[ch];
    if(margin < SUBGHZ_HOPPER_FREQ_MARGIN_DB) margin = SUBGHZ_HOPPER_FREQ_MARGIN_DB;
    if(margin > SUBGHZ_HOPPER_MARGIN_MAX_DB) margin = SUBGHZ_HOPPER_MARGIN_MAX_DB;
    return margin;
}

static float subghz_txrx_hopper_freq_threshold(SubGhzTxRx* instance) {
    float threshold = instance->hopper_threshold;
    const size_t ch = instance->hopper_idx_frequency;
    if(ch < SUBGHZ_HOPPER_FREQ_FLOOR_MAX && instance->hopper_ch_floor_valid[ch]) {
        const float relative =
            instance->hopper_ch_floor[ch] + subghz_txrx_hopper_freq_margin(instance, ch);
        if(relative > threshold) threshold = relative;
    }
    return threshold;
}

static void subghz_txrx_hopper_freq_release(SubGhzTxRx* instance, uint32_t* slot_start) {
    instance->hopper_state = SubGhzHopperStateRunning;
    instance->hopper_freq_quiet_since = 0;
    instance->hopper_decoded_until = 0;
    subghz_txrx_hopper_step_frequency(instance);
    *slot_start = furi_get_tick();
}

void subghz_txrx_hopper_note_decode(SubGhzTxRx* instance) {
    furi_assert(instance);
    furi_mutex_acquire(instance->hopper_mutex, FuriWaitForever);
    instance->hopper_decoded_until =
        furi_get_tick() + furi_ms_to_ticks(SUBGHZ_HOPPER_FREQ_DECODE_HOLD_MS);
    instance->hopper_slot_decoded = true;
    furi_mutex_release(instance->hopper_mutex);
}

bool subghz_txrx_hopper_get_level(SubGhzTxRx* instance, float* rssi, float* floor) {
    furi_assert(instance);
    if(instance->hopper_state == SubGhzHopperStateOFF) return false;
    if(instance->hopper_thread == NULL) return false;
    if(!instance->hopper_level_valid) return false;

    if(rssi) *rssi = instance->hopper_level_rssi;
    if(floor) *floor = instance->hopper_level_floor;
    return true;
}

void subghz_txrx_hopper_update(SubGhzTxRx* instance, float stay_threshold) {
    furi_assert(instance);
    instance->hopper_threshold = stay_threshold;
}

typedef enum {
    SubGhzHopperAirUnknown,
    SubGhzHopperAirAsk,
    SubGhzHopperAirFsk,
} SubGhzHopperAir;

static int8_t
    subghz_txrx_probe_percentile(const uint16_t* hist, uint32_t total, uint32_t permille) {
    const uint32_t want = (total * permille) / 1000;
    uint32_t seen = 0;
    for(int32_t v = -128; v <= 127; v++) {
        seen += hist[(uint8_t)(int8_t)v];
        if(seen > want) return (int8_t)v;
    }
    return 127;
}

static SubGhzHopperAir subghz_txrx_hopper_classify(
    SubGhzTxRx* instance,
    float arm_level,
    float* carrier_db_out,
    int8_t* p90_out) {
    SubGhzHopperClassifyBuf* buf = instance->hopper_classify_buf;
    if(carrier_db_out) *carrier_db_out = 0.0f;
    if(p90_out) *p90_out = -128;
    if(!buf) return SubGhzHopperAirUnknown;

    const size_t count =
        subghz_txrx_raw_auto_available(subghz_devices_get_name(instance->radio_device)) ?
            SUBGHZ_HOPPER_CLASSIFY_SAMPLES / 4 :
            SUBGHZ_HOPPER_CLASSIFY_SAMPLES;
    const uint32_t span_us = subghz_devices_rssi_burst(
        instance->radio_device, buf->samples, count, SUBGHZ_HOPPER_CLASSIFY_PERIOD_US);
    if(span_us == 0) return SubGhzHopperAirUnknown;

    memset(buf->hist, 0, sizeof(buf->hist));
    for(size_t i = 0; i < count; i++)
        buf->hist[buf->samples[i]]++;

    const int8_t p10 = subghz_txrx_probe_percentile(buf->hist, count, 100);
    const int8_t p90 = subghz_txrx_probe_percentile(buf->hist, count, 900);

    if(p90_out) *p90_out = p90;

    const float p90_dbm = (float)p90 / 2.0f - 74.0f;
    if(p90_dbm < arm_level + SUBGHZ_HOPPER_CLASSIFY_SNR_DB) return SubGhzHopperAirUnknown;

    const int8_t mid = (int8_t)(((int32_t)p10 + (int32_t)p90) / 2);
    uint32_t above = 0;
    memset(buf->hist, 0, sizeof(buf->hist));
    for(size_t i = 0; i < count; i++) {
        if((int8_t)buf->samples[i] > mid) {
            buf->hist[buf->samples[i]]++;
            above++;
        }
    }
    if(!above) return SubGhzHopperAirFsk;

    const int8_t c10 = subghz_txrx_probe_percentile(buf->hist, above, 100);
    const int8_t c90 = subghz_txrx_probe_percentile(buf->hist, above, 900);
    const float carrier_db = ((float)c90 - (float)c10) / 2.0f;
    if(carrier_db_out) *carrier_db_out = carrier_db;

    if(carrier_db >= SUBGHZ_HOPPER_ASK_CARRIER_DB) return SubGhzHopperAirAsk;
    if(carrier_db <= SUBGHZ_HOPPER_FSK_CARRIER_DB) return SubGhzHopperAirFsk;
    return SubGhzHopperAirUnknown;
}

static SubGhzHopperAir subghz_txrx_raw_auto_decide(
    const uint8_t* rssi,
    size_t rssi_count,
    const uint8_t* pin,
    size_t pin_count,
    float arm_dbm) {
    if(!rssi || !pin || rssi_count < 16 || pin_count < 16) return SubGhzHopperAirUnknown;

    uint16_t* hist = malloc(sizeof(uint16_t) * 256);
    for(size_t i = 0; i < rssi_count; i++)
        hist[rssi[i]]++;
    const int8_t p10 = subghz_txrx_probe_percentile(hist, (uint32_t)rssi_count, 100);
    const int8_t p90 = subghz_txrx_probe_percentile(hist, (uint32_t)rssi_count, 900);
    free(hist);
    if((float)p90 / 2.0f - 74.0f < arm_dbm + 20.0f) return SubGhzHopperAirUnknown;

    const int16_t on_at = (int16_t)p90 - 8;
    const int16_t off_at = (int16_t)p90 - 16;
    bool on = (int8_t)rssi[0] >= on_at;
    uint16_t offs = 0;
    uint16_t keyed = 0;
    for(size_t i = 0; i < rssi_count; i++) {
        const int16_t v = (int8_t)rssi[i];
        if(v <= off_at) {
            offs++;
            if(on) keyed++;
            on = false;
        } else if(v >= on_at) {
            on = true;
        }
    }
    if(keyed >= 3 && offs * 10u >= rssi_count) return SubGhzHopperAirAsk;

    if((int16_t)p10 < (int16_t)p90 - 10) return SubGhzHopperAirUnknown;
    uint16_t edges = 0;
    uint16_t glitches = 0;
    size_t run = 0;
    for(size_t i = 1; i < pin_count; i++) {
        run++;
        if(pin[i] == pin[i - 1]) continue;
        if(edges && run == 1) glitches++;
        edges++;
        run = 0;
    }
    if(edges >= 4 && glitches * 5u <= edges) return SubGhzHopperAirFsk;
    return SubGhzHopperAirUnknown;
}

bool subghz_txrx_raw_auto_available(const char* radio_name) {
    return radio_name && strcmp(radio_name, SUBGHZ_DEVICE_SI4463_EXT_NAME) == 0;
}

#define SUBGHZ_RAW_AUTO_RSSI_HALF 20
#define SUBGHZ_RAW_AUTO_RSSI_US   1000
#define SUBGHZ_RAW_AUTO_PIN       128

SubGhzRawAutoAir subghz_txrx_raw_auto_listen(SubGhzTxRx* instance, float arm_level_dbm) {
    furi_assert(instance);
    if(!instance->radio_device) return SubGhzRawAutoAirUnknown;

    static uint8_t rssi[SUBGHZ_RAW_AUTO_RSSI_HALF * 2];
    static uint8_t pin[SUBGHZ_RAW_AUTO_PIN];
    const GpioPin* gpio = subghz_devices_get_data_gpio(instance->radio_device);
    if(!gpio) return SubGhzRawAutoAirUnknown;

    if(!subghz_devices_rssi_burst(
           instance->radio_device, rssi, SUBGHZ_RAW_AUTO_RSSI_HALF, SUBGHZ_RAW_AUTO_RSSI_US))
        return SubGhzRawAutoAirUnknown;
    const uint32_t pin_cycles = 50u * furi_hal_cortex_instructions_per_microsecond();
    const uint32_t pin_t0 = DWT->CYCCNT;
    for(size_t i = 0; i < SUBGHZ_RAW_AUTO_PIN; i++) {
        while((int32_t)(DWT->CYCCNT - (pin_t0 + (uint32_t)i * pin_cycles)) < 0) {
        }
        pin[i] = furi_hal_gpio_read(gpio) ? 1u : 0u;
    }
    if(!subghz_devices_rssi_burst(
           instance->radio_device,
           rssi + SUBGHZ_RAW_AUTO_RSSI_HALF,
           SUBGHZ_RAW_AUTO_RSSI_HALF,
           SUBGHZ_RAW_AUTO_RSSI_US))
        return SubGhzRawAutoAirUnknown;

    const SubGhzHopperAir air =
        subghz_txrx_raw_auto_decide(rssi, sizeof(rssi), pin, SUBGHZ_RAW_AUTO_PIN, arm_level_dbm);
    if(air == SubGhzHopperAirAsk) return SubGhzRawAutoAirAsk;
    if(air == SubGhzHopperAirFsk) return SubGhzRawAutoAirFsk;
    return SubGhzRawAutoAirUnknown;
}

static bool subghz_txrx_raw_auto_park(SubGhzTxRx* instance, const char* name, uint32_t frequency) {
    if(!frequency) frequency = instance->preset->frequency;
    if(!frequency) frequency = 433920000;
    const int index = subghz_setting_get_inx_preset_by_name(instance->setting, name);
    if(index < 0) return false;
    uint8_t* data = subghz_setting_get_preset_data(instance->setting, (size_t)index);
    const size_t size = subghz_setting_get_preset_data_size(instance->setting, (size_t)index);
    if(!data || !size) return false;
    subghz_txrx_set_preset(instance, name, frequency, data, size);
    subghz_txrx_rx_start(instance);
    return instance->txrx_state == SubGhzTxRxStateRx;
}

#define SUBGHZ_RAW_AUTO_SWEEP_BINS    25
#define SUBGHZ_RAW_AUTO_SWEEP_STEP_HZ 10000
#define SUBGHZ_RAW_AUTO_SWEEP_PASSES  8
#define SUBGHZ_RAW_AUTO_SWEEP_PRESET  "FM476"
#define SUBGHZ_RAW_AUTO_PROBE         "FM476"
#define SUBGHZ_RAW_AUTO_AM            "AM650"
#define SUBGHZ_RAW_AUTO_FM_WIDE       "FM476"
#define SUBGHZ_RAW_AUTO_FM_MID        "FM14K"
#define SUBGHZ_RAW_AUTO_FM_NARROW     "FM12K"
#define SUBGHZ_RAW_AUTO_FM_FALLBACK   "FM476"

typedef struct {
    uint32_t carrier_hz;
    uint32_t separation_hz;
    uint8_t max;
} SubGhzRawAutoSurvey;

static bool subghz_txrx_raw_auto_sweep_pass(
    SubGhzTxRx* instance,
    uint32_t center_hz,
    int8_t* hold,
    int8_t floor_raw) {
    const SubGhzDevice* dev = instance->radio_device;
    bool hot = false;
    for(size_t i = 0; i < SUBGHZ_RAW_AUTO_SWEEP_BINS; i++) {
        const int32_t offset =
            ((int32_t)i - SUBGHZ_RAW_AUTO_SWEEP_BINS / 2) * SUBGHZ_RAW_AUTO_SWEEP_STEP_HZ;
        const uint32_t f = (uint32_t)((int32_t)center_hz + offset);
        if(!subghz_devices_is_frequency_valid(dev, f)) continue;
        subghz_devices_idle(dev);
        subghz_devices_set_frequency(dev, f);
        subghz_devices_set_rx(dev);
        furi_delay_ms(2);
        uint8_t s[3] = {0};
        if(!subghz_devices_rssi_burst(dev, s, sizeof(s), 1000)) return false;
        int8_t best = INT8_MIN;
        for(size_t k = 0; k < sizeof(s); k++)
            if((int8_t)s[k] > best) best = (int8_t)s[k];
        if(best > hold[i]) hold[i] = best;
        if(best >= floor_raw + 2 * SUBGHZ_RAW_GATE_OPEN_DB) hot = true;
    }
    subghz_devices_idle(dev);
    return hot;
}

static void
    subghz_txrx_raw_auto_survey(const int8_t* hold, uint32_t center_hz, SubGhzRawAutoSurvey* out) {
    memset(out, 0, sizeof(*out));
    int8_t max = INT8_MIN;
    for(size_t i = 0; i < SUBGHZ_RAW_AUTO_SWEEP_BINS; i++)
        if(hold[i] > max) max = hold[i];
    if(max == INT8_MIN) return;
    out->max = (uint8_t)max;
    const int8_t active = (int8_t)(max - 12);

    int first = -1, second = -1;
    for(size_t i = 0; i < SUBGHZ_RAW_AUTO_SWEEP_BINS; i++) {
        if(hold[i] < max - 6) continue;
        if(i > 0 && hold[i - 1] > hold[i]) continue;
        if(i + 1 < SUBGHZ_RAW_AUTO_SWEEP_BINS && hold[i + 1] > hold[i]) continue;
        if(first < 0) {
            first = (int)i;
        } else if((int)i - first >= 3) {
            second = (int)i;
            break;
        }
    }
    if(first >= 0 && second >= 0) {
        int8_t dip = max;
        for(int i = first + 1; i < second; i++)
            if(hold[i] < dip) dip = hold[i];
        if(dip <= max - 6) {
            out->separation_hz = (uint32_t)(second - first) * SUBGHZ_RAW_AUTO_SWEEP_STEP_HZ;
            out->carrier_hz =
                (uint32_t)((int32_t)center_hz +
                           ((first + second) - 2 * (SUBGHZ_RAW_AUTO_SWEEP_BINS / 2)) *
                               (SUBGHZ_RAW_AUTO_SWEEP_STEP_HZ / 2));
            return;
        }
    }
    int32_t weight = 0, sum = 0;
    for(size_t i = 0; i < SUBGHZ_RAW_AUTO_SWEEP_BINS; i++) {
        if(hold[i] < active) continue;
        const int32_t w = hold[i] - active + 1;
        weight += w;
        sum += w * ((int32_t)i - SUBGHZ_RAW_AUTO_SWEEP_BINS / 2);
    }
    if(weight) {
        out->carrier_hz =
            (uint32_t)((int32_t)center_hz + sum * SUBGHZ_RAW_AUTO_SWEEP_STEP_HZ / weight);
    }
}

static bool subghz_txrx_raw_auto_device_preset(SubGhzTxRx* instance, const char* name) {
    uint8_t* data = subghz_setting_get_preset_data_by_name(instance->setting, name);
    if(!data) return false;
    subghz_devices_idle(instance->radio_device);
    subghz_devices_load_preset(instance->radio_device, FuriHalSubGhzPresetCustom, data);
    return true;
}

const char* subghz_txrx_raw_auto_capture(
    SubGhzTxRx* instance,
    const char* file_name,
    uint32_t timeout_ms,
    void (*start_tx)(void* context),
    void* context,
    bool* file_error) {
    furi_assert(instance);
    furi_assert(file_name);
    if(file_error) *file_error = false;

    subghz_txrx_raw_si_gate_set(instance, false);
    const uint32_t tuned_hz = instance->preset->frequency ? instance->preset->frequency :
                                                            433920000;
    if(!subghz_txrx_raw_auto_park(instance, SUBGHZ_RAW_AUTO_PROBE, tuned_hz)) return NULL;

    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);
    uint32_t settle_until = furi_get_tick() + furi_ms_to_ticks(30);
    bool have_floor = false;
    float floor = -127.0f;
    uint16_t samples = 0;
    bool tx_started = false;
    SubGhzRawAutoAir air = SubGhzRawAutoAirUnknown;
    SubGhzRawAutoAir last_air = SubGhzRawAutoAirUnknown;

    while((int32_t)(deadline - furi_get_tick()) > 0 && air == SubGhzRawAutoAirUnknown) {
        if((int32_t)(settle_until - furi_get_tick()) > 0) {
            furi_delay_ms(10);
            continue;
        }
        const float rssi = subghz_devices_get_rssi(instance->radio_device);
        if(rssi > -126.0f) {
            if(!have_floor || rssi < floor) {
                floor = rssi;
                have_floor = true;
            }
        }
        if(samples < 1000) samples++;
        if(start_tx && !tx_started) {
            if(samples >= 6) {
                tx_started = true;
                start_tx(context);
            }
            furi_delay_ms(15);
            continue;
        }
        if(!start_tx && !tx_started && samples >= 6) tx_started = true;

        const bool hot = have_floor && rssi > floor + (float)SUBGHZ_RAW_GATE_OPEN_DB;
        if(samples >= 2 && hot) {
            const SubGhzRawAutoAir now = subghz_txrx_raw_auto_listen(instance, floor);
            if(now != SubGhzRawAutoAirUnknown && now == last_air) {
                air = now;
            } else {
                last_air = now;
                furi_delay_ms(5);
            }
        } else {
            last_air = SubGhzRawAutoAirUnknown;
            furi_delay_ms(15);
        }
    }
    if(air == SubGhzRawAutoAirUnknown) {
        subghz_txrx_stop(instance);
        FURI_LOG_I(TAG, "RAW AUTO none");
        return NULL;
    }

    subghz_txrx_stop(instance);
    SubGhzRawAutoSurvey survey = {0};
    if(subghz_txrx_raw_auto_device_preset(instance, SUBGHZ_RAW_AUTO_SWEEP_PRESET)) {
        static int8_t hold[SUBGHZ_RAW_AUTO_SWEEP_BINS];
        memset(hold, INT8_MIN, sizeof(hold));
        const int8_t floor_raw = (int8_t)((floor + 74.0f) * 2.0f);
        for(uint8_t pass = 0; pass < SUBGHZ_RAW_AUTO_SWEEP_PASSES; pass++) {
            const bool hot = subghz_txrx_raw_auto_sweep_pass(instance, tuned_hz, hold, floor_raw);
            if(!hot && pass) break;
            if((int32_t)(deadline - furi_get_tick()) <= 0) break;
        }
        subghz_txrx_raw_auto_survey(hold, tuned_hz, &survey);
    }
    uint32_t carrier_hz = survey.carrier_hz ? survey.carrier_hz : tuned_hz;
    if(!subghz_devices_is_frequency_valid(instance->radio_device, carrier_hz))
        carrier_hz = tuned_hz;
    FURI_LOG_I(
        TAG,
        "RAW AUTO %s carrier %lu (tuned %lu) tones %lu Hz apart",
        air == SubGhzRawAutoAirAsk ? "AM" : "FM",
        carrier_hz,
        tuned_hz,
        survey.separation_hz);

    const char* candidates[2] = {NULL, NULL};
    if(air == SubGhzRawAutoAirAsk) {
        candidates[0] = SUBGHZ_RAW_AUTO_AM;
    } else if(survey.separation_hz >= 60000u) {
        candidates[0] = SUBGHZ_RAW_AUTO_FM_WIDE;
    } else if(survey.separation_hz >= 30000u) {
        candidates[0] = SUBGHZ_RAW_AUTO_FM_MID;
    } else {
        candidates[0] = SUBGHZ_RAW_AUTO_FM_NARROW;
        candidates[1] = SUBGHZ_RAW_AUTO_FM_FALLBACK;
    }
    const char* locked = candidates[0];
    if(candidates[1]) {
        if(!subghz_txrx_raw_auto_park(instance, candidates[0], carrier_hz)) return NULL;
        const uint32_t verify_until = furi_get_tick() + furi_ms_to_ticks(2500);
        locked = candidates[1];
        while((int32_t)(verify_until - furi_get_tick()) > 0) {
            const float rssi = subghz_devices_get_rssi(instance->radio_device);
            if(rssi > floor + (float)SUBGHZ_RAW_GATE_OPEN_DB &&
               subghz_txrx_raw_auto_listen(instance, floor) == SubGhzRawAutoAirFsk) {
                locked = candidates[0];
                break;
            }
            furi_delay_ms(10);
        }
        subghz_txrx_stop(instance);
    }

    const int index = subghz_setting_get_inx_preset_by_name(instance->setting, locked);
    if(index < 0) return NULL;
    subghz_txrx_set_preset(
        instance,
        locked,
        carrier_hz,
        subghz_setting_get_preset_data(instance->setting, (size_t)index),
        subghz_setting_get_preset_data_size(instance->setting, (size_t)index));
    SubGhzProtocolDecoderRAW* decoder =
        (SubGhzProtocolDecoderRAW*)subghz_txrx_get_decoder(instance);
    if(!decoder) return NULL;
    SubGhzRadioPreset preset = subghz_txrx_get_preset(instance);
    if(!subghz_protocol_raw_save_to_file_init(decoder, file_name, &preset)) {
        if(file_error) *file_error = true;
        FURI_LOG_I(TAG, "RAW AUTO file");
        return NULL;
    }
    subghz_txrx_raw_si_gate_set(
        instance, subghz_txrx_raw_auto_available(subghz_txrx_radio_device_get_name(instance)));
    if(instance->raw_gate_on && have_floor) {
        instance->raw_gate_floor_db = (int)floor;
        instance->raw_gate_have_floor = true;
        instance->raw_gate_open = true;
    }
    subghz_txrx_rx_start(instance);
    FURI_LOG_I(TAG, "RAW AUTO %s @ %lu", locked, carrier_hz);
    return locked;
}

static int32_t subghz_txrx_hopper_thread(void* context) {
    SubGhzTxRx* instance = context;

    instance->hopper_classify_buf = malloc(sizeof(SubGhzHopperClassifyBuf));
    uint32_t slot_start = furi_get_tick();

    while(instance->hopper_thread_running) {
        furi_delay_ms(SUBGHZ_HOPPER_GRID_MS);

        furi_mutex_acquire(instance->hopper_mutex, FuriWaitForever);

        const bool sweep_presets = instance->hopper_mode == SubGhzHopperModePreset &&
                                   instance->hopper_preset_a != instance->hopper_preset_b;
        const bool sweep_channels = instance->hopper_mode == SubGhzHopperModeFrequency;
        const bool active = (instance->hopper_state == SubGhzHopperStateRunning ||
                             instance->hopper_state == SubGhzHopperStateRSSITimeOut) &&
                            instance->txrx_state == SubGhzTxRxStateRx && !instance->dual_active &&
                            (sweep_presets || sweep_channels);

        if(!active) {
            slot_start = furi_get_tick();
            instance->hopper_busy_until[0] = 0;
            instance->hopper_busy_until[1] = 0;
            instance->hopper_rssi_run[0] = 0;
            instance->hopper_rssi_run[1] = 0;
            instance->hopper_energy_hot[0] = false;
            instance->hopper_energy_hot[1] = false;
            furi_mutex_release(instance->hopper_mutex);
            continue;
        }

        const uint32_t now = furi_get_tick();
        const uint32_t elapsed_ticks = now - slot_start;

        if(sweep_channels) {
            if(instance->hopper_state != SubGhzHopperStateRSSITimeOut &&
               instance->hopper_decoded_until != 0) {
                instance->hopper_state = SubGhzHopperStateRSSITimeOut;
                instance->hopper_freq_hold_threshold = subghz_txrx_hopper_freq_threshold(instance);
                instance->hopper_freq_quiet_since = 0;
                slot_start = now;
            }

            if(instance->hopper_state == SubGhzHopperStateRSSITimeOut) {
                bool leave = false;
                if(elapsed_ticks >= furi_ms_to_ticks(SUBGHZ_HOPPER_FREQ_HOLD_MS)) {
                    leave = true;
                } else if(
                    instance->hopper_decoded_until != 0 &&
                    (int32_t)(now - instance->hopper_decoded_until) >= 0) {
                    leave = true;
                } else {
                    const float rssi = subghz_devices_get_rssi(instance->radio_device);
                    const size_t hold_ch = instance->hopper_idx_frequency;
                    const float still_hot =
                        instance->hopper_freq_hold_threshold -
                        subghz_txrx_hopper_hysteresis(
                            hold_ch < SUBGHZ_HOPPER_FREQ_FLOOR_MAX ?
                                subghz_txrx_hopper_freq_margin(instance, hold_ch) :
                                SUBGHZ_HOPPER_FREQ_MARGIN_DB);
                    if(rssi > still_hot) {
                        instance->hopper_freq_quiet_since = 0;
                    } else {
                        if(instance->hopper_freq_quiet_since == 0) {
                            instance->hopper_freq_quiet_since = now;
                        }
                        if((now - instance->hopper_freq_quiet_since) >=
                           furi_ms_to_ticks(SUBGHZ_HOPPER_HANGOVER_MS)) {
                            leave = true;
                        }
                    }
                }
                if(leave) {
                    subghz_txrx_hopper_freq_release(instance, &slot_start);
                }
            } else if(elapsed_ticks >= furi_ms_to_ticks(SUBGHZ_HOPPER_FREQ_DWELL_MS)) {
                const float rssi = subghz_devices_get_rssi(instance->radio_device);
                const size_t ch = instance->hopper_idx_frequency;

                const float threshold = subghz_txrx_hopper_freq_threshold(instance);

                instance->hopper_level_rssi = rssi;
                instance->hopper_level_floor =
                    (ch < SUBGHZ_HOPPER_FREQ_FLOOR_MAX && instance->hopper_ch_floor_valid[ch]) ?
                        instance->hopper_ch_floor[ch] :
                        rssi;
                instance->hopper_level_valid = true;

                if(ch < SUBGHZ_HOPPER_FREQ_FLOOR_MAX) {
                    if(!instance->hopper_ch_floor_valid[ch]) {
                        instance->hopper_ch_floor[ch] = rssi;
                        instance->hopper_ch_mad[ch] = 0.0f;
                        instance->hopper_ch_floor_valid[ch] = true;
                    } else if(
                        rssi <= instance->hopper_ch_floor[ch] +
                                    subghz_txrx_hopper_track_gate(instance->hopper_ch_mad[ch])) {
                        const float dev = fabsf(rssi - instance->hopper_ch_floor[ch]);
                        instance->hopper_ch_floor[ch] =
                            instance->hopper_ch_floor[ch] * (1.0f - SUBGHZ_HOPPER_FLOOR_ALPHA) +
                            rssi * SUBGHZ_HOPPER_FLOOR_ALPHA;
                        instance->hopper_ch_mad[ch] =
                            instance->hopper_ch_mad[ch] * (1.0f - SUBGHZ_HOPPER_MAD_ALPHA) +
                            dev * SUBGHZ_HOPPER_MAD_ALPHA;
                    }
                }

                if(rssi > threshold) {
                    instance->hopper_state = SubGhzHopperStateRSSITimeOut;
                    instance->hopper_freq_hold_threshold = threshold;
                    instance->hopper_freq_quiet_since = 0;
                    slot_start = now;
                } else {
                    subghz_txrx_hopper_step_frequency(instance);
                    slot_start = furi_get_tick();
                }
            }
            furi_mutex_release(instance->hopper_mutex);
            continue;
        }

        const uint8_t slot = instance->hopper_idx_preset ? 1 : 0;
        const float rssi = subghz_devices_get_rssi(instance->radio_device);
        const bool was_busy = (int32_t)(instance->hopper_busy_until[slot] - now) > 0;

        if(!instance->hopper_rssi_floor_valid[slot]) {
            instance->hopper_rssi_floor[slot] = rssi;
            instance->hopper_rssi_mad[slot] = 0.0f;
            instance->hopper_rssi_floor_valid[slot] = true;
        }

        const float margin = subghz_txrx_hopper_margin(instance->hopper_rssi_mad[slot]);
        const float base = instance->hopper_rssi_floor[slot] + margin;
        const float threshold = was_busy ? (base - subghz_txrx_hopper_hysteresis(margin)) : base;

        const float track_gate = subghz_txrx_hopper_track_gate(instance->hopper_rssi_mad[slot]);
        if(rssi <= instance->hopper_rssi_floor[slot] + track_gate) {
            const float dev = fabsf(rssi - instance->hopper_rssi_floor[slot]);
            instance->hopper_rssi_floor[slot] =
                instance->hopper_rssi_floor[slot] * (1.0f - SUBGHZ_HOPPER_FLOOR_ALPHA) +
                rssi * SUBGHZ_HOPPER_FLOOR_ALPHA;
            instance->hopper_rssi_mad[slot] =
                instance->hopper_rssi_mad[slot] * (1.0f - SUBGHZ_HOPPER_MAD_ALPHA) +
                dev * SUBGHZ_HOPPER_MAD_ALPHA;
        }

        instance->hopper_level_rssi = rssi;
        instance->hopper_level_floor = instance->hopper_rssi_floor[slot];
        instance->hopper_level_valid = true;

        bool energy_edge = false;
        float arm_level = 0.0f;
        if(instance->hopper_rssi_floor_valid[slot]) {
            arm_level = instance->hopper_rssi_floor[slot] + SUBGHZ_HOPPER_CLASSIFY_ARM_DB;
            const bool hot = rssi > arm_level;
            energy_edge = hot && !instance->hopper_energy_hot[slot];
            instance->hopper_energy_hot[slot] = hot;
        }

        const bool activity = subghz_txrx_hopper_activity_seen(instance);

        if(rssi > threshold) {
            if(was_busy) {
                instance->hopper_busy_until[slot] =
                    now + furi_ms_to_ticks(SUBGHZ_HOPPER_HANGOVER_MS);
            } else {
                if(instance->hopper_rssi_run[slot] < UINT8_MAX) instance->hopper_rssi_run[slot]++;
                if(instance->hopper_rssi_run[slot] >= SUBGHZ_HOPPER_LATCH_SAMPLES) {
                    instance->hopper_busy_until[slot] =
                        now + furi_ms_to_ticks(SUBGHZ_HOPPER_HANGOVER_MS);
                } else {
                }
            }
        } else {
            instance->hopper_rssi_run[slot] = 0;
        }

        if(activity) {
            instance->hopper_busy_until[slot] = now + furi_ms_to_ticks(SUBGHZ_HOPPER_HANGOVER_MS);
            instance->hopper_slot_activity = true;
        }
        const bool busy = (int32_t)(instance->hopper_busy_until[slot] - now) > 0;

        const bool classify_worth_it = energy_edge &&
                                       rssi >= arm_level + SUBGHZ_HOPPER_CLASSIFY_SNR_DB &&
                                       (int32_t)(now - instance->hopper_classify_next) >= 0;
        if(classify_worth_it) {
            instance->hopper_classify_next =
                now + furi_ms_to_ticks(SUBGHZ_HOPPER_CLASSIFY_COOLDOWN_MS);
            const size_t loaded_idx = slot ? instance->hopper_preset_b : instance->hopper_preset_a;
            const bool loaded_is_ask = subghz_txrx_preset_is_ask(
                subghz_setting_get_preset_data(instance->setting, loaded_idx));

            if(!loaded_is_ask) {
                float carrier_db = 0.0f;
                int8_t p90 = -128;
                const SubGhzHopperAir air =
                    subghz_txrx_hopper_classify(instance, arm_level, &carrier_db, &p90);

                if(air == SubGhzHopperAirAsk) {
                } else if(air == SubGhzHopperAirFsk) {
                } else {
                }

                const float p90_dbm = (float)p90 / 2.0f - 74.0f;
                if(air == SubGhzHopperAirFsk && carrier_db <= SUBGHZ_HOPPER_FSK_STRICT_DB &&
                   p90_dbm >= arm_level + SUBGHZ_HOPPER_FSK_STRICT_SNR_DB &&
                   p90_dbm <= SUBGHZ_HOPPER_FSK_CEILING_DBM) {
                    const bool a_ask = subghz_txrx_preset_is_ask(subghz_setting_get_preset_data(
                        instance->setting, instance->hopper_preset_a));
                    const bool b_ask = subghz_txrx_preset_is_ask(subghz_setting_get_preset_data(
                        instance->setting, instance->hopper_preset_b));

                    if(a_ask != b_ask) {
                        instance->hopper_air_fsk_until =
                            now + furi_ms_to_ticks(SUBGHZ_HOPPER_AIR_MEMORY_MS);
                    }
                }

                if(air == SubGhzHopperAirAsk) {
                    const bool a_is_ask = subghz_txrx_preset_is_ask(subghz_setting_get_preset_data(
                        instance->setting, instance->hopper_preset_a));
                    const bool b_is_ask = subghz_txrx_preset_is_ask(subghz_setting_get_preset_data(
                        instance->setting, instance->hopper_preset_b));

                    if(a_is_ask != b_is_ask) {
                        const uint8_t want = a_is_ask ? 0 : 1;
                        if(want != instance->hopper_idx_preset) {
                            subghz_txrx_hopper_use_preset(instance, want);
                            instance->hopper_slot_classified = true;
                            instance->hopper_slot_decoded = false;

                            slot_start = furi_get_tick();
                        }
                    }
                }
            }
        }

        const size_t slot_preset_idx = slot ? instance->hopper_preset_b :
                                              instance->hopper_preset_a;
        const bool slot_is_ask = subghz_txrx_preset_is_ask(
            subghz_setting_get_preset_data(instance->setting, slot_preset_idx));

        uint32_t target_ms = busy ? SUBGHZ_HOPPER_HOLD_MS : SUBGHZ_HOPPER_DWELL_MS;

        if(busy && instance->hopper_slot_classified &&
           SUBGHZ_HOPPER_CLASSIFIED_HOLD_MS > target_ms) {
            target_ms = SUBGHZ_HOPPER_CLASSIFIED_HOLD_MS;
        }

        if(busy && slot_is_ask && (int32_t)(instance->hopper_air_fsk_until - now) > 0) {
            target_ms = SUBGHZ_HOPPER_DWELL_MS;
        }

        if(elapsed_ticks >= furi_ms_to_ticks(target_ms)) {
            const bool evidence = instance->hopper_slot_decoded ||
                                  instance->hopper_slot_classified ||
                                  instance->hopper_slot_activity;
            if(busy && !evidence) {
                if(!instance->hopper_rssi_floor_valid[slot]) {
                    instance->hopper_rssi_floor[slot] = rssi;
                    instance->hopper_rssi_floor_valid[slot] = true;
                } else if(rssi > instance->hopper_rssi_floor[slot]) {
                    const float dev = rssi - instance->hopper_rssi_floor[slot];
                    if(dev > instance->hopper_rssi_mad[slot]) {
                        instance->hopper_rssi_mad[slot] =
                            instance->hopper_rssi_mad[slot] *
                                (1.0f - SUBGHZ_HOPPER_FLOOR_NOISE_ALPHA) +
                            dev * SUBGHZ_HOPPER_FLOOR_NOISE_ALPHA;
                    }
                    float rise = dev * SUBGHZ_HOPPER_FLOOR_NOISE_ALPHA;
                    if(rise > SUBGHZ_HOPPER_FLOOR_NOISE_STEP_DB) {
                        rise = SUBGHZ_HOPPER_FLOOR_NOISE_STEP_DB;
                    }
                    instance->hopper_rssi_floor[slot] += rise;
                }
            }

            subghz_txrx_hopper_step_preset(instance);

            instance->hopper_slot_classified = false;
            instance->hopper_slot_decoded = false;
            instance->hopper_slot_activity = false;
            subghz_txrx_hopper_activity_reset(instance);

            slot_start = furi_get_tick();
        }

        furi_mutex_release(instance->hopper_mutex);
    }

    free(instance->hopper_classify_buf);
    instance->hopper_classify_buf = NULL;

    return 0;
}

static void subghz_txrx_hopper_thread_stop(SubGhzTxRx* instance) {
    if(instance->hopper_thread == NULL) return;

    instance->hopper_thread_running = false;
    furi_thread_join(instance->hopper_thread);
    furi_thread_free(instance->hopper_thread);
    instance->hopper_thread = NULL;
}

void subghz_txrx_hopper_resume(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->hopper_state == SubGhzHopperStateOFF) return;
    if(instance->hopper_thread != NULL) return;

    instance->hopper_busy_until[0] = 0;
    instance->hopper_busy_until[1] = 0;
    instance->hopper_rssi_run[0] = 0;
    instance->hopper_rssi_run[1] = 0;
    instance->hopper_rssi_floor_valid[0] = false;
    instance->hopper_rssi_floor_valid[1] = false;
    instance->hopper_rssi_floor[0] = 0;
    instance->hopper_rssi_floor[1] = 0;

    subghz_txrx_hopper_state_reset(instance);
    instance->hopper_thread_running = true;
    instance->hopper_thread =
        furi_thread_alloc_ex("SubGhzHopper", 2048, subghz_txrx_hopper_thread, instance);

    furi_thread_set_priority(instance->hopper_thread, FuriThreadPriorityHigh);

    furi_thread_start(instance->hopper_thread);
}

void subghz_txrx_hopper_suspend(SubGhzTxRx* instance) {
    furi_assert(instance);

    subghz_txrx_hopper_thread_stop(instance);
}

SubGhzHopperMode subghz_txrx_hopper_get_mode(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->hopper_mode;
}

bool subghz_txrx_hopper_owns_frequency(SubGhzTxRx* instance) {
    furi_assert(instance);

    return (instance->hopper_state != SubGhzHopperStateOFF) &&
           (instance->hopper_mode == SubGhzHopperModeFrequency);
}

void subghz_txrx_hopper_set_mode(SubGhzTxRx* instance, SubGhzHopperMode mode) {
    furi_assert(instance);

    subghz_txrx_hopper_thread_stop(instance);
    instance->hopper_mode = mode;
    instance->hopper_idx_preset = 0;
}

void subghz_txrx_hopper_set_presets(SubGhzTxRx* instance, size_t preset_a, size_t preset_b) {
    furi_assert(instance);
    subghz_txrx_hopper_lock(instance);
    instance->hopper_preset_a = (uint8_t)preset_a;
    instance->hopper_preset_b = (uint8_t)preset_b;
    instance->hopper_idx_preset = 0;
    subghz_txrx_hopper_unlock(instance);
}

void subghz_txrx_hopper_lock(SubGhzTxRx* instance) {
    furi_assert(instance);

    furi_mutex_acquire(instance->hopper_mutex, FuriWaitForever);
}

void subghz_txrx_hopper_unlock(SubGhzTxRx* instance) {
    furi_assert(instance);
    furi_mutex_release(instance->hopper_mutex);
}

SubGhzHopperState subghz_txrx_hopper_get_state(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->hopper_state;
}

void subghz_txrx_hopper_set_state(SubGhzTxRx* instance, SubGhzHopperState state) {
    furi_assert(instance);
    if(state == SubGhzHopperStateOFF) {
        subghz_txrx_hopper_thread_stop(instance);
    }
    instance->hopper_state = state;
}

void subghz_txrx_hopper_unpause(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->hopper_state == SubGhzHopperStatePause) {
        instance->hopper_state = SubGhzHopperStateRunning;
    }
}

void subghz_txrx_hopper_pause(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->hopper_state == SubGhzHopperStateRunning) {
        instance->hopper_state = SubGhzHopperStatePause;
    }
}

void subghz_txrx_speaker_on(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->debug_pin_state) {
        subghz_devices_set_async_mirror_pin(instance->radio_device, &gpio_ibutton);
    }

    if(instance->speaker_state == SubGhzSpeakerStateEnable) {
        if(furi_hal_speaker_acquire(30)) {
            if(!instance->debug_pin_state) {
                subghz_devices_set_async_mirror_pin(instance->radio_device, &gpio_speaker);
            }
        } else {
            instance->speaker_state = SubGhzSpeakerStateDisable;
        }
    }
}

void subghz_txrx_speaker_off(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->debug_pin_state) {
        subghz_devices_set_async_mirror_pin(instance->radio_device, NULL);
    }
    if(instance->speaker_state != SubGhzSpeakerStateDisable) {
        if(furi_hal_speaker_is_mine()) {
            if(!instance->debug_pin_state) {
                subghz_devices_set_async_mirror_pin(instance->radio_device, NULL);
            }
            furi_hal_speaker_release();
            if(instance->speaker_state == SubGhzSpeakerStateShutdown)
                instance->speaker_state = SubGhzSpeakerStateDisable;
        }
    }
}

void subghz_txrx_speaker_mute(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->debug_pin_state) {
        subghz_devices_set_async_mirror_pin(instance->radio_device, NULL);
    }
    if(instance->speaker_state == SubGhzSpeakerStateEnable) {
        if(furi_hal_speaker_is_mine()) {
            if(!instance->debug_pin_state) {
                subghz_devices_set_async_mirror_pin(instance->radio_device, NULL);
            }
        }
    }
}

void subghz_txrx_speaker_unmute(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->debug_pin_state) {
        subghz_devices_set_async_mirror_pin(instance->radio_device, &gpio_ibutton);
    }
    if(instance->speaker_state == SubGhzSpeakerStateEnable) {
        if(furi_hal_speaker_is_mine()) {
            if(!instance->debug_pin_state) {
                subghz_devices_set_async_mirror_pin(instance->radio_device, &gpio_speaker);
            }
        }
    }
}

void subghz_txrx_speaker_set_state(SubGhzTxRx* instance, SubGhzSpeakerState state) {
    furi_assert(instance);
    instance->speaker_state = state;
}

SubGhzSpeakerState subghz_txrx_speaker_get_state(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->speaker_state;
}

bool subghz_txrx_load_decoder_by_name_protocol(SubGhzTxRx* instance, const char* name_protocol) {
    furi_assert(instance);
    furi_assert(name_protocol);
    bool res = false;
    instance->decoder_result = subghz_receiver_search_decoder_base_by_name(
        instance->receiver, subghz_txrx_protocol_name_alias(name_protocol));
    if(instance->decoder_result) {
        res = true;
    }
    return res;
}

SubGhzProtocolDecoderBase* subghz_txrx_get_decoder(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->decoder_result;
}

bool subghz_txrx_protocol_is_serializable(SubGhzTxRx* instance) {
    furi_assert(instance);
    return (instance->decoder_result->protocol->flag & SubGhzProtocolFlag_Save) ==
           SubGhzProtocolFlag_Save;
}

bool subghz_txrx_protocol_is_transmittable(SubGhzTxRx* instance, bool check_type) {
    furi_assert(instance);
    const SubGhzProtocol* protocol = instance->decoder_result->protocol;
    if(check_type) {
        return ((protocol->flag & SubGhzProtocolFlag_Send) == SubGhzProtocolFlag_Send) &&
               protocol->encoder->deserialize && protocol->type == SubGhzProtocolTypeStatic;
    }
    return ((protocol->flag & SubGhzProtocolFlag_Send) == SubGhzProtocolFlag_Send) &&
           protocol->encoder->deserialize;
}

void subghz_txrx_receiver_set_filter(SubGhzTxRx* instance, SubGhzProtocolFlag filter) {
    furi_assert(instance);
    subghz_receiver_set_filter(instance->receiver, filter);
}

static void subghz_txrx_rx_callback_trampoline(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    SubGhzTxRx* instance = context;

    if(instance->rx_callback) {
        instance->rx_callback(receiver, decoder_base, instance->rx_callback_context);
    }
}

void subghz_txrx_set_rx_callback(
    SubGhzTxRx* instance,
    SubGhzReceiverCallback callback,
    void* context) {
    instance->rx_callback = callback;
    instance->rx_callback_context = context;
    subghz_receiver_set_rx_callback(
        instance->receiver, subghz_txrx_rx_callback_trampoline, instance);
}

void subghz_txrx_set_raw_file_encoder_worker_callback_end(
    SubGhzTxRx* instance,
    SubGhzProtocolEncoderRAWCallbackEnd callback,
    void* context) {
    subghz_protocol_raw_file_encoder_worker_set_callback_end(
        (SubGhzProtocolEncoderRAW*)subghz_transmitter_get_protocol_instance(instance->transmitter),
        callback,
        context);
}

bool subghz_txrx_radio_device_is_external_connected(SubGhzTxRx* instance, const char* name) {
    furi_assert(instance);
    if(!name) return false;

    const char* current_name =
        instance->radio_device ? subghz_devices_get_name(instance->radio_device) : NULL;
    if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal && current_name &&
       strcmp(current_name, name) == 0) {
        return subghz_devices_is_connect(instance->radio_device);
    }

    bool is_connect = false;
    bool is_otg_enabled = furi_hal_power_is_otg_enabled();

    if(!is_otg_enabled) {
        subghz_txrx_radio_device_power_on(instance);
    }

    is_connect = subghz_device_registry_is_connect(name);

    if(!is_otg_enabled) {
        subghz_txrx_radio_device_power_off(instance);
    }
    return is_connect;
}

const char* subghz_txrx_radio_device_detect_external(SubGhzTxRx* instance) {
    furi_assert(instance);

    if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal && instance->radio_device) {
        return subghz_devices_get_name(instance->radio_device);
    }

    bool is_otg_enabled = furi_hal_power_is_otg_enabled();
    if(!is_otg_enabled) {
        subghz_txrx_radio_device_power_on(instance);
    }

    const char* name = radio_device_loader_external_name();

    if(!is_otg_enabled) {
        subghz_txrx_radio_device_power_off(instance);
    }
    return name;
}

SubGhzRadioDeviceType
    subghz_txrx_radio_device_set(SubGhzTxRx* instance, SubGhzRadioDeviceType radio_device_type) {
    furi_assert(instance);

    if(radio_device_type == SubGhzRadioDeviceTypeExternalCC1101) {
        if(instance->radio_device_type == SubGhzRadioDeviceTypeExternalCC1101 &&
           instance->radio_device && subghz_devices_is_connect(instance->radio_device)) {
            return instance->radio_device_type;
        }

        bool is_otg_enabled = furi_hal_power_is_otg_enabled();
        if(!is_otg_enabled) {
            subghz_txrx_radio_device_power_on(instance);
        }

        const char* name = radio_device_loader_external_name();
        if(name) {
            if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal &&
               instance->radio_device) {
                subghz_devices_end(instance->radio_device);
            }
            instance->radio_device = subghz_devices_get_by_name(name);
            if(instance->radio_device && subghz_devices_begin(instance->radio_device)) {
                instance->radio_device_type = SubGhzRadioDeviceTypeExternalCC1101;
                return instance->radio_device_type;
            }
            if(instance->radio_device) {
                subghz_devices_end(instance->radio_device);
                instance->radio_device = NULL;
            }
        }

        if(!is_otg_enabled) {
            subghz_txrx_radio_device_power_off(instance);
        }
    }

    subghz_txrx_radio_device_power_off(instance);
    if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal && instance->radio_device) {
        subghz_devices_end(instance->radio_device);
    }
    instance->radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    instance->radio_device_type = SubGhzRadioDeviceTypeInternal;
    return instance->radio_device_type;
}

SubGhzRadioDeviceType subghz_txrx_radio_device_get(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->radio_device_type;
}

const SubGhzDevice* subghz_txrx_radio_device_get_ptr(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->radio_device;
}

float subghz_txrx_radio_device_get_rssi(SubGhzTxRx* instance) {
    furi_assert(instance);
    return subghz_devices_get_rssi(instance->radio_device);
}

const char* subghz_txrx_radio_device_get_name(SubGhzTxRx* instance) {
    furi_assert(instance);
    return subghz_devices_get_name(instance->radio_device);
}

bool subghz_txrx_radio_device_is_frequency_valid(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    return subghz_devices_is_frequency_valid(instance->radio_device, frequency);
}

size_t subghz_txrx_frequency_valid_index(SubGhzTxRx* instance, size_t index, bool next) {
    furi_assert(instance);
    const size_t count = subghz_setting_get_frequency_count(instance->setting);
    if(count == 0) return index;
    for(size_t i = 0; i < count; i++) {
        const size_t candidate = next ? (index + i) % count : (index + count - i) % count;
        if(subghz_devices_is_frequency_valid(
               instance->radio_device, subghz_setting_get_frequency(instance->setting, candidate)))
            return candidate;
    }
    return index;
}

uint32_t subghz_txrx_frequency_make_valid(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    if(subghz_devices_is_frequency_valid(instance->radio_device, frequency)) return frequency;
    if(subghz_devices_is_frequency_valid(instance->radio_device, SUBGHZ_TXRX_FALLBACK_FREQUENCY))
        return SUBGHZ_TXRX_FALLBACK_FREQUENCY;
    if(subghz_setting_get_frequency_count(instance->setting) == 0) return frequency;
    const size_t index = subghz_txrx_frequency_valid_index(instance, 0, true);
    const uint32_t candidate = subghz_setting_get_frequency(instance->setting, index);
    return subghz_devices_is_frequency_valid(instance->radio_device, candidate) ? candidate :
                                                                                  frequency;
}

bool subghz_txrx_radio_device_is_tx_allowed(SubGhzTxRx* instance, uint32_t frequency) {
    // TODO: Remake this function to check if the frequency is allowed on specific module - for modules not based on CC1101
    furi_assert(instance);
    UNUSED(frequency);
    /*
    furi_assert(instance->txrx_state != SubGhzTxRxStateSleep);

    subghz_devices_idle(instance->radio_device);
    subghz_devices_set_frequency(instance->radio_device, frequency);

    bool ret = subghz_devices_set_tx(instance->radio_device);
    subghz_devices_idle(instance->radio_device);

    return ret;
    */
    return true;
}

void subghz_txrx_set_debug_pin_state(SubGhzTxRx* instance, bool state) {
    furi_assert(instance);
    instance->debug_pin_state = state;
}

bool subghz_txrx_get_debug_pin_state(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->debug_pin_state;
}

void subghz_txrx_reset_dynamic_and_custom_btns(SubGhzTxRx* instance) {
    furi_assert(instance);
    subghz_environment_reset_keeloq(instance->environment);

    faac_slh_reset_prog_mode();

    subghz_custom_btns_reset();
}

SubGhzReceiver* subghz_txrx_get_receiver(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->receiver;
}

SubGhzEnvironment* subghz_txrx_get_environment(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->environment;
}

void subghz_txrx_set_default_preset(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);

    const char* default_modulation = "AM650";
    if(frequency == 0) {
        frequency = subghz_setting_get_default_frequency(subghz_txrx_get_setting(instance));
    }
    subghz_txrx_set_preset(instance, default_modulation, frequency, NULL, 0);
}

bool subghz_txrx_preset_fits_kind(SubGhzTxRx* instance, size_t preset_idx, bool si4463) {
    furi_assert(instance);
    const bool si_preset = subghz_setting_preset_is_si4463(instance->setting, preset_idx);
    if(!si4463) return !si_preset;
    return si_preset || !subghz_setting_has_si4463_preset(instance->setting);
}

bool subghz_txrx_preset_fits_radio(SubGhzTxRx* instance, size_t preset_idx) {
    furi_assert(instance);
    const char* name = subghz_txrx_radio_device_get_name(instance);
    const bool si4463 = name && strcmp(name, SUBGHZ_DEVICE_SI4463_EXT_NAME) == 0;
    return subghz_txrx_preset_fits_kind(instance, preset_idx, si4463);
}

static size_t subghz_txrx_preset_first_for_radio(SubGhzTxRx* instance) {
    for(size_t i = 0; i < subghz_setting_get_preset_count(instance->setting); i++) {
        if(subghz_txrx_preset_fits_radio(instance, i)) return i;
    }
    return 0;
}

uint8_t subghz_txrx_preset_make_valid(SubGhzTxRx* instance, uint8_t tx_power) {
    furi_assert(instance);
    SubGhzRadioPreset preset = subghz_txrx_get_preset(instance);
    int index = subghz_setting_get_inx_preset_by_name(
        instance->setting, furi_string_get_cstr(preset.name));
    const char* name = subghz_txrx_set_preset_internal(
        instance, preset.frequency, index < 0 ? UINT8_MAX : (uint8_t)index, tx_power);
    return (uint8_t)subghz_setting_get_inx_preset_by_name(instance->setting, name);
}

const char* subghz_txrx_set_preset_internal(
    SubGhzTxRx* instance,
    uint32_t frequency,
    uint8_t index,
    uint8_t tx_power) {
    furi_assert(instance);

    //Grab the prset name.
    SubGhzSetting* setting = subghz_txrx_get_setting(instance);
    if(index < subghz_setting_get_preset_count(setting) &&
       !subghz_txrx_preset_fits_radio(instance, index)) {
        const char* radio = subghz_txrx_radio_device_get_name(instance);
        const bool si4463 = radio && !strcmp(radio, SUBGHZ_DEVICE_SI4463_EXT_NAME);
        const int other = subghz_setting_get_inx_preset_by_name(
            setting,
            subghz_setting_radio_counterpart(
                subghz_setting_get_preset_name(setting, index), si4463));
        if(other >= 0 && subghz_txrx_preset_fits_radio(instance, (size_t)other))
            index = (uint8_t)other;
    }
    if(index >= subghz_setting_get_preset_count(setting) ||
       !subghz_txrx_preset_fits_radio(instance, index)) {
        index = (uint8_t)subghz_txrx_preset_first_for_radio(instance);
    }
    const char* preset_name = subghz_setting_get_preset_name(setting, index);
    subghz_setting_set_default_frequency(setting, frequency);

    //Get the preset data now so we can set TX power.
    uint8_t* preset_data = subghz_setting_get_preset_data(setting, index);
    size_t preset_data_size = subghz_setting_get_preset_data_size(setting, index);

    //Edit TX power, if necessary.
    subghz_txrx_set_tx_power(preset_data, preset_data_size, tx_power);

    //Set the Updated Preset.
    subghz_txrx_set_preset(instance, preset_name, frequency, preset_data, preset_data_size);

    return preset_name;
}

static void subghz_txrx_dual_fill_preset(
    SubGhzTxRx* instance,
    SubGhzRadioPreset* preset,
    const char* preset_name,
    uint32_t frequency) {
    furi_string_set(preset->name, preset_name);
    preset->frequency = frequency;
    int index = subghz_setting_get_inx_preset_by_name(instance->setting, preset_name);
    if(index >= 0) {
        preset->data = subghz_setting_get_preset_data(instance->setting, index);
        preset->data_size = subghz_setting_get_preset_data_size(instance->setting, index);
    } else {
        preset->data = NULL;
        preset->data_size = 0;
    }
}

static void subghz_txrx_dual_decode(void* context, bool level, uint32_t duration) {
    SubGhzDualChain* chain = context;
    furi_mutex_acquire(chain->mutex, FuriWaitForever);
    subghz_receiver_decode(chain->receiver, level, duration);
    furi_mutex_release(chain->mutex);
}

static void subghz_txrx_dual_overrun(void* context) {
    SubGhzDualChain* chain = context;
    furi_mutex_acquire(chain->mutex, FuriWaitForever);
    subghz_receiver_reset(chain->receiver);
    furi_mutex_release(chain->mutex);
}

void subghz_txrx_dual_set_preset_int(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency) {
    furi_assert(instance);
    subghz_txrx_dual_fill_preset(instance, instance->preset, preset_name, frequency);
}

void subghz_txrx_dual_set_preset_ext(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency) {
    furi_assert(instance);
    subghz_txrx_dual_fill_preset(instance, instance->preset_ext, preset_name, frequency);
}

SubGhzRadioPreset subghz_txrx_dual_get_preset_int(SubGhzTxRx* instance) {
    furi_assert(instance);
    return *instance->preset;
}

SubGhzRadioPreset subghz_txrx_dual_get_preset_ext(SubGhzTxRx* instance) {
    furi_assert(instance);
    return *instance->preset_ext;
}

void subghz_txrx_dual_get_frequency_and_modulation(
    SubGhzTxRx* instance,
    bool external,
    FuriString* frequency,
    FuriString* modulation,
    bool long_name) {
    furi_assert(instance);
    SubGhzRadioPreset* preset = external ? instance->preset_ext : instance->preset;
    if(frequency != NULL) {
        furi_string_printf(
            frequency,
            "%03ld.%02ld",
            preset->frequency / 1000000 % 1000,
            preset->frequency / 10000 % 100);
    }
    if(modulation != NULL) {
        if(long_name) {
            furi_string_printf(modulation, "%s", furi_string_get_cstr(preset->name));
        } else {
            furi_string_printf(modulation, "%.2s", furi_string_get_cstr(preset->name));
        }
    }
}

bool subghz_txrx_dual_is_active(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->dual_active;
}

bool subghz_txrx_dual_is_ext_connected(SubGhzTxRx* instance) {
    furi_assert(instance);
    return subghz_txrx_radio_device_detect_external(instance) != NULL;
}

bool subghz_txrx_dual_ext_is_si4463(SubGhzTxRx* instance) {
    furi_assert(instance);
    const char* name = subghz_txrx_radio_device_detect_external(instance);
    return name && strcmp(name, SUBGHZ_DEVICE_SI4463_EXT_NAME) == 0;
}

static const char* subghz_txrx_preset_resolve_for_kind(
    SubGhzTxRx* instance,
    const char* preset_name,
    bool si4463) {
    SubGhzSetting* setting = instance->setting;
    const int index = preset_name ? subghz_setting_get_inx_preset_by_name(setting, preset_name) :
                                    -1;
    if(index >= 0 && subghz_txrx_preset_fits_kind(instance, (size_t)index, si4463)) {
        return subghz_setting_get_preset_name(setting, (size_t)index);
    }
    if(preset_name) {
        const char* other_name = subghz_setting_radio_counterpart(preset_name, si4463);
        if(other_name && other_name != preset_name) {
            const int other = subghz_setting_get_inx_preset_by_name(setting, other_name);
            if(other >= 0 && subghz_txrx_preset_fits_kind(instance, (size_t)other, si4463)) {
                return subghz_setting_get_preset_name(setting, (size_t)other);
            }
        }
    }
    for(size_t i = 0; i < subghz_setting_get_preset_count(setting); i++) {
        if(subghz_txrx_preset_fits_kind(instance, i, si4463)) {
            return subghz_setting_get_preset_name(setting, i);
        }
    }
    return preset_name;
}

static void subghz_txrx_dual_coerce_one(SubGhzTxRx* instance, bool external, bool si4463) {
    SubGhzRadioPreset* preset = external ? instance->preset_ext : instance->preset;
    const char* resolved = subghz_txrx_preset_resolve_for_kind(
        instance, furi_string_get_cstr(preset->name), external && si4463);
    if(!resolved || !strcmp(resolved, furi_string_get_cstr(preset->name))) return;
    if(external) {
        subghz_txrx_dual_set_preset_ext(instance, resolved, preset->frequency);
    } else {
        subghz_txrx_dual_set_preset_int(instance, resolved, preset->frequency);
    }
}

void subghz_txrx_dual_coerce_presets(SubGhzTxRx* instance, bool ext_si4463) {
    furi_assert(instance);
    subghz_txrx_dual_coerce_one(instance, false, false);
    subghz_txrx_dual_coerce_one(instance, true, ext_si4463);
}

static void subghz_txrx_dual_start_chain(
    const SubGhzDevice* device,
    SubGhzRadioPreset* preset,
    SubGhzWorker* worker) {
    subghz_devices_reset(device);
    subghz_devices_idle(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, preset->data);
    subghz_devices_idle(device);
    subghz_devices_set_frequency(device, preset->frequency);
    subghz_devices_flush_rx(device);
    subghz_worker_set_priority(
        worker,
        subghz_txrx_preset_is_heavy(preset->data) ? FuriThreadPriorityNormal :
                                                    FuriThreadPriorityHigh);
    subghz_worker_start(worker);
    subghz_devices_start_async_rx(device, subghz_worker_rx_callback, worker);
}

static SubGhzReceiver* subghz_txrx_dual_alloc_receiver(
    SubGhzEnvironment* environment,
    const SubGhzRadioPreset* preset) {
    const SubGhzProtocolRegistry* registry = subghz_environment_get_protocol_registry(environment);
    const size_t total = subghz_protocol_registry_count(registry);
    const SubGhzProtocol** protocols = malloc(total * sizeof(*protocols));
    const SubGhzProtocolFlag gate = subghz_txrx_gate_mask(preset->frequency, preset->data);
    const SubGhzProtocolFlag modulation = gate & (SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM);
    const SubGhzProtocolFlag band =
        gate & (SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868);
    size_t count = 0;
    for(size_t i = 0; i < total; i++) {
        const SubGhzProtocol* protocol = subghz_protocol_registry_get_by_index(registry, i);
        if((protocol->flag & SubGhzProtocolFlag_Decodable) &&
           !(protocol->flag & (SubGhzProtocolFlag_RAW | SubGhzProtocolFlag_BinRAW)) &&
           (protocol->flag & modulation) && (protocol->flag & band) && protocol->decoder &&
           protocol->decoder->alloc) {
            protocols[count++] = protocol;
        }
    }
    const SubGhzProtocolRegistry filtered = {.items = protocols, .size = count};
    subghz_environment_set_protocol_registry(environment, &filtered);
    SubGhzReceiver* receiver = subghz_receiver_alloc_init_ex(
        environment, SubGhzProtocolFlag_RAW | SubGhzProtocolFlag_BinRAW);
    subghz_environment_set_protocol_registry(environment, registry);
    free(protocols);
    FURI_LOG_I(TAG, "Dual RX: %zu matching decoders, heap %zu", count, memmgr_get_free_heap());
    return receiver;
}

bool subghz_txrx_dual_rx_start(SubGhzTxRx* instance) {
    furi_assert(instance);

    subghz_txrx_hopper_thread_stop(instance);
    subghz_txrx_stop(instance);

    instance->dual_saved_device_type = instance->radio_device_type;

    subghz_txrx_radio_device_set(instance, SubGhzRadioDeviceTypeInternal);

    instance->radio_device_int = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);

    subghz_txrx_radio_device_power_on(instance);
    const char* ext_name = radio_device_loader_external_name();
    instance->radio_device_ext = ext_name ? subghz_devices_get_by_name(ext_name) : NULL;

    if(instance->radio_device_ext == NULL || !subghz_devices_begin(instance->radio_device_ext)) {
        FURI_LOG_E(TAG, "Dual RX: external radio not connected");
        if(instance->radio_device_ext != NULL) subghz_devices_end(instance->radio_device_ext);
        subghz_txrx_radio_device_power_off(instance);
        instance->radio_device_ext = NULL;
        return false;
    }

    subghz_txrx_dual_coerce_presets(
        instance, ext_name && strcmp(ext_name, SUBGHZ_DEVICE_SI4463_EXT_NAME) == 0);

    subghz_worker_free(instance->worker);
    subghz_receiver_free(instance->receiver);
    instance->worker = NULL;
    instance->receiver = NULL;
    instance->decoder_result = NULL;
    instance->dual_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->dual_active = true;
    instance->worker = subghz_worker_alloc_ex(2048);
    instance->receiver =
        subghz_txrx_dual_alloc_receiver(instance->environment, instance->preset);
    instance->receiver_ext =
        subghz_txrx_dual_alloc_receiver(instance->environment, instance->preset_ext);
    instance->worker_ext = subghz_worker_alloc_ex(2048);

    instance->dual_chain_int.receiver = instance->receiver;
    instance->dual_chain_int.mutex = instance->dual_mutex;
    instance->dual_chain_ext.receiver = instance->receiver_ext;
    instance->dual_chain_ext.mutex = instance->dual_mutex;

    if(instance->worker) {
        subghz_worker_set_overrun_callback(instance->worker, subghz_txrx_dual_overrun);
        subghz_worker_set_pair_callback(instance->worker, subghz_txrx_dual_decode);
        subghz_worker_set_context(instance->worker, &instance->dual_chain_int);
        subghz_receiver_reset(instance->receiver);
    }
    subghz_worker_set_overrun_callback(instance->worker_ext, subghz_txrx_dual_overrun);
    subghz_worker_set_pair_callback(instance->worker_ext, subghz_txrx_dual_decode);
    subghz_worker_set_context(instance->worker_ext, &instance->dual_chain_ext);
    subghz_receiver_reset(instance->receiver_ext);

    if(instance->receiver)
        subghz_receiver_set_gate(
            instance->receiver,
            subghz_txrx_gate_mask(instance->preset->frequency, instance->preset->data));
    subghz_receiver_set_gate(
        instance->receiver_ext,
        subghz_txrx_gate_mask(instance->preset_ext->frequency, instance->preset_ext->data));
    instance->txrx_state = SubGhzTxRxStateRx;
    return true;
}

void subghz_txrx_dual_rx_stop(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(!instance->dual_active) return;

    if(instance->worker && subghz_worker_is_running(instance->worker)) {
        subghz_devices_stop_async_rx(instance->radio_device_int);
        subghz_worker_stop(instance->worker);
    }
    subghz_devices_idle(instance->radio_device_int);

    if(instance->worker_ext && subghz_worker_is_running(instance->worker_ext)) {
        subghz_devices_stop_async_rx(instance->radio_device_ext);
        subghz_worker_stop(instance->worker_ext);
    }
    subghz_devices_idle(instance->radio_device_ext);
    subghz_devices_end(instance->radio_device_ext);
    subghz_txrx_radio_device_power_off(instance);

    if(instance->worker_ext) subghz_worker_free(instance->worker_ext);
    if(instance->receiver_ext) subghz_receiver_free(instance->receiver_ext);
    furi_mutex_free(instance->dual_mutex);
    instance->worker_ext = NULL;
    instance->receiver_ext = NULL;
    instance->dual_mutex = NULL;
    instance->radio_device_ext = NULL;

    if(instance->worker) subghz_worker_free(instance->worker);
    if(instance->receiver) subghz_receiver_free(instance->receiver);
    instance->worker = subghz_worker_alloc();
    instance->receiver = subghz_receiver_alloc_init(instance->environment);
    instance->decoder_result = NULL;

    subghz_worker_set_overrun_callback(instance->worker, subghz_txrx_worker_reset);
    subghz_worker_set_pair_callback(instance->worker, subghz_txrx_worker_decode);
    subghz_worker_set_barrier_callback(instance->worker, subghz_txrx_barrier_callback);
    subghz_worker_set_context(instance->worker, instance);

    instance->dual_active = false;
    instance->txrx_state = SubGhzTxRxStateIDLE;

    subghz_txrx_radio_device_set(instance, instance->dual_saved_device_type);
}

void subghz_txrx_dual_set_rx_callback(
    SubGhzTxRx* instance,
    SubGhzReceiverCallback callback_int,
    SubGhzReceiverCallback callback_ext,
    void* context) {
    furi_assert(instance);
    if(instance->receiver)
        subghz_receiver_set_rx_callback(instance->receiver, callback_int, context);
    if(instance->receiver_ext != NULL) {
        subghz_receiver_set_rx_callback(instance->receiver_ext, callback_ext, context);
    }
}

bool subghz_txrx_dual_begin(SubGhzTxRx* instance) {
    furi_check(instance->dual_active);
    subghz_txrx_dual_start_chain(
        instance->radio_device_ext, instance->preset_ext, instance->worker_ext);
    subghz_txrx_dual_start_chain(
        instance->radio_device_int, instance->preset, instance->worker);
    FURI_LOG_I(TAG, "Dual RX running, heap %zu", memmgr_get_free_heap());
    return true;
}

void subghz_txrx_dual_set_filter(SubGhzTxRx* instance, SubGhzProtocolFlag filter) {
    furi_assert(instance);
    if(instance->receiver) subghz_receiver_set_filter(instance->receiver, filter);
    if(instance->receiver_ext != NULL) {
        subghz_receiver_set_filter(instance->receiver_ext, filter);
    }
}

void subghz_txrx_dual_lock(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->dual_mutex != NULL) {
        furi_mutex_acquire(instance->dual_mutex, FuriWaitForever);
    }
}

void subghz_txrx_dual_unlock(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->dual_mutex != NULL) {
        furi_mutex_release(instance->dual_mutex);
    }
}

float subghz_txrx_dual_get_rssi_int(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->radio_device_int == NULL) return -127.0f;
    return subghz_devices_get_rssi(instance->radio_device_int);
}

float subghz_txrx_dual_get_rssi_ext(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->radio_device_ext == NULL) return -127.0f;
    return subghz_devices_get_rssi(instance->radio_device_ext);
}

SubGhzProtocolDecoderBase*
    subghz_txrx_dual_get_decoder_ext_by_name(SubGhzTxRx* instance, const char* name_protocol) {
    furi_assert(instance);
    if(instance->receiver_ext == NULL) return NULL;
    return subghz_receiver_search_decoder_base_by_name(
        instance->receiver_ext, subghz_txrx_protocol_name_alias(name_protocol));
}

bool subghz_txrx_save_raw_from_key(
    SubGhzTxRx* instance,
    FlipperFormat* flipper_format,
    const char* file_path) {
    furi_assert(instance);
    furi_assert(flipper_format);
    furi_assert(file_path);

    bool saved = false;
    FuriString* temp_str = furi_string_alloc();
    SubGhzTransmitter* transmitter = NULL;

    do {
        if(!flipper_format_rewind(flipper_format)) {
            break;
        }
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            break;
        }

        transmitter = subghz_transmitter_alloc_init(
            instance->environment,
            subghz_txrx_protocol_name_alias(furi_string_get_cstr(temp_str)));
        if(!transmitter) {
            break;
        }

        flipper_format_rewind(flipper_format);
        if(subghz_transmitter_deserialize(transmitter, flipper_format) != SubGhzProtocolStatusOk) {
            break;
        }

        const LevelDuration* upload = NULL;
        size_t upload_size = 0;
        if(!subghz_transmitter_get_upload(transmitter, &upload, &upload_size)) {
            break;
        }

        SubGhzRadioPreset preset = subghz_txrx_get_preset(instance);
        saved = subghz_protocol_raw_save_upload_to_file(upload, upload_size, &preset, file_path);
    } while(false);

    if(transmitter) {
        subghz_transmitter_free(transmitter);
    }
    furi_string_free(temp_str);

    return saved;
}
