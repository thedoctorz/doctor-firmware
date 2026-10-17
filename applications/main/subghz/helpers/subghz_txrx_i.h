#pragma once

#include "subghz_txrx.h"

#define SUBGHZ_HOPPER_FREQ_FLOOR_MAX 16

typedef struct {
    uint8_t samples[128];
    uint16_t hist[256];
} SubGhzHopperClassifyBuf;

typedef struct {
    SubGhzReceiver* receiver;
    FuriMutex* mutex;
} SubGhzDualChain;

struct SubGhzTxRx {
    SubGhzWorker* worker;

    SubGhzEnvironment* environment;
    SubGhzReceiver* receiver;
    SubGhzTransmitter* transmitter;
    SubGhzProtocolDecoderBase* decoder_result;
    FlipperFormat* fff_data;

    SubGhzRadioPreset* preset;
    SubGhzSetting* setting;

    uint8_t hopper_timeout;
    uint8_t hopper_idx_frequency;
    uint8_t hopper_idx_preset;

    SubGhzHopperMode hopper_mode;
    uint8_t hopper_preset_a;
    uint8_t hopper_preset_b;
    FuriThread* hopper_thread;
    volatile bool hopper_thread_running;
    FuriMutex* hopper_mutex;

    float hopper_threshold;

    uint32_t hopper_busy_until[2];

    uint8_t hopper_rssi_run[2];

    uint32_t hopper_air_fsk_until;
    bool hopper_slot_classified;
    bool hopper_slot_decoded;
    bool hopper_slot_activity;

    float hopper_ch_floor[SUBGHZ_HOPPER_FREQ_FLOOR_MAX];
    bool hopper_ch_floor_valid[SUBGHZ_HOPPER_FREQ_FLOOR_MAX];

    float hopper_rssi_mad[2];
    float hopper_ch_mad[SUBGHZ_HOPPER_FREQ_FLOOR_MAX];

    float hopper_level_rssi;
    float hopper_level_floor;
    bool hopper_level_valid;

    float hopper_freq_hold_threshold;

    uint32_t hopper_freq_quiet_since;

    uint32_t hopper_decoded_until;
    SubGhzHopperClassifyBuf* hopper_classify_buf;
    bool hopper_energy_hot[2];

    uint32_t hopper_classify_next;
    float hopper_rssi_floor[2];
    bool hopper_rssi_floor_valid[2];

    volatile uint32_t hopper_act_edges;
    volatile uint32_t hopper_act_match;
    volatile uint32_t hopper_act_long;
    volatile uint32_t hopper_act_prev_us;

    bool is_database_loaded;
    SubGhzHopperState hopper_state;

    SubGhzTxRxState txrx_state;
    SubGhzSpeakerState speaker_state;
    const SubGhzDevice* radio_device;
    SubGhzRadioDeviceType radio_device_type;
    //An external module was asked for and answered; radio_device_type is what is
    //actually driving right now, and the two part company when one is unplugged
    bool radio_device_external_wanted;
    //When the last probe of any kind ran, see the probe period
    uint32_t radio_device_probe_tick;

    SubGhzTxRxNeedSaveCallback need_save_callback;
    void* need_save_context;
    bool tx_from_internal_fff;

    SubGhzReceiverCallback rx_callback;
    void* rx_callback_context;

    bool debug_pin_state;

    bool dual_active;
    SubGhzWorker* worker_ext;
    SubGhzReceiver* receiver_ext;
    SubGhzRadioPreset* preset_ext;
    const SubGhzDevice* radio_device_int;
    const SubGhzDevice* radio_device_ext;
    FuriMutex* dual_mutex;
    SubGhzDualChain dual_chain_int;
    SubGhzDualChain dual_chain_ext;
    SubGhzRadioDeviceType dual_saved_device_type;

    bool raw_gate_on;
    bool raw_gate_open;
    bool raw_gate_have_floor;
    int raw_gate_floor_db;
    uint8_t raw_gate_low_hits;
    uint32_t raw_gate_last_rssi;
    uint32_t* raw_gate_ring;
    uint16_t raw_gate_ring_n;
    uint16_t raw_gate_ring_i;
};
