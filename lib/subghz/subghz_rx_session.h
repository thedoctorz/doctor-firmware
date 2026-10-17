#pragma once

#include <flipper_format/flipper_format.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SubGhzRxSession SubGhzRxSession;

typedef enum {
    SubGhzRxSessionStatusOk = 0,
    SubGhzRxSessionStatusErrorBusy,
    SubGhzRxSessionStatusErrorInvalid,
    SubGhzRxSessionStatusErrorNoDevice,
    SubGhzRxSessionStatusErrorTxForbidden,
    SubGhzRxSessionStatusErrorInternal,
} SubGhzRxSessionStatus;

#define SUBGHZ_RX_PULSE_LEVEL_BIT     (1UL << 31)
#define SUBGHZ_RX_PULSE_DURATION_MASK (SUBGHZ_RX_PULSE_LEVEL_BIT - 1UL)

#define SUBGHZ_RX_RSSI_BATCH_MAX    64U
#define SUBGHZ_RX_PULSE_CAPTURE_MAX 512U
#define SUBGHZ_RX_HOP_FREQUENCY_MAX 16U

typedef struct {
    const char* protocol;
    const char* flipper_format;
    uint32_t frequency;
    int32_t rssi;
    uint32_t timestamp_ms;
    bool transmittable;
    const uint32_t* pulses;
    size_t pulse_count;
} SubGhzRxHit;

typedef struct {
    void (*on_rssi)(
        const int8_t* samples,
        size_t count,
        uint32_t frequency,
        int32_t noise_floor,
        void* context);
    void (*on_hit)(const SubGhzRxHit* hit, void* context);
    void (*on_stopped)(void* context);
} SubGhzRxSessionCallbacks;

typedef struct {
    uint32_t frequency;
    const char* preset;
    uint32_t rssi_interval_ms;
    bool hopping;
    const uint32_t* hop_frequencies;
    size_t hop_count;
    bool external_device;
    bool auto_device;
    bool capture_pulses;
    uint32_t ignore_filter;
    bool modulation_hopping;
    const char* preset_b;
    bool sound;
} SubGhzRxConfig;

typedef struct {
    bool running;
    uint32_t frequency;
    const char* preset;
    bool hopping;
    bool external_device;
    bool tx_allowed;
    const char* device_name;
    uint32_t ignore_filter;
    bool modulation_hopping;
    const char* preset_b;
    bool sound;
} SubGhzRxState;

SubGhzRxSession* subghz_rx_session_alloc(const SubGhzRxSessionCallbacks* callbacks, void* context);

void subghz_rx_session_free(SubGhzRxSession* instance);

SubGhzRxSessionStatus
    subghz_rx_session_start(SubGhzRxSession* instance, const SubGhzRxConfig* config);

SubGhzRxSessionStatus
    subghz_rx_session_reconfigure(SubGhzRxSession* instance, const SubGhzRxConfig* config);

void subghz_rx_session_stop(SubGhzRxSession* instance);

bool subghz_rx_session_is_running(SubGhzRxSession* instance);

void subghz_rx_session_get_state(SubGhzRxSession* instance, SubGhzRxState* state);

SubGhzRxSessionStatus subghz_rx_session_transmit(
    SubGhzRxSession* instance,
    FlipperFormat* format,
    uint32_t repeat,
    bool external_device,
    uint32_t* out_frequency);

#ifdef __cplusplus
}
#endif
