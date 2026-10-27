#pragma once

#include "subghz_types.h"

#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/protocols/raw.h>
#include <lib/subghz/devices/devices.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SubGhzTxRx SubGhzTxRx;

typedef void (*SubGhzTxRxNeedSaveCallback)(void* context);

typedef enum {
    SubGhzTxRxStartTxStateOk,
    SubGhzTxRxStartTxStateErrorOnlyRx,
    SubGhzTxRxStartTxStateErrorParserOthers,
} SubGhzTxRxStartTxState;

/**
 * Allocate SubGhzTxRx
 * 
 * @return SubGhzTxRx* pointer to SubGhzTxRx
 */
SubGhzTxRx* subghz_txrx_alloc(void);

/**
 * Free SubGhzTxRx
 * 
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_free(SubGhzTxRx* instance);

/**
 * Check if the database is loaded
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @return bool True if the database is loaded
 */
bool subghz_txrx_is_database_loaded(SubGhzTxRx* instance);

/**
 * Set preset 
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param preset_name Name of preset
 * @param frequency Frequency in Hz
 * @param preset_data Data of preset
 * @param preset_data_size Size of preset data
 */
void subghz_txrx_set_preset(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency,
    uint8_t* preset_data,
    size_t preset_data_size);

/**
 * Set TX Power
 * 
 * @param preset_data Data of preset
 * @param preset_data_size Size of preset data
 * @param tx_power Menu Index of TX Power Setting. (Saves iterating in Config enter)
 */
uint8_t* subghz_txrx_set_tx_power(uint8_t* preset_data, size_t preset_data_size, uint8_t tx_power);

/**
 * Get name of preset
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param preset String of preset 
 * @return const char*  Name of preset
 */
const char* subghz_txrx_get_preset_name(SubGhzTxRx* instance, const char* preset);

/**
 * Get of preset
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @return SubGhzRadioPreset Preset
 */
SubGhzRadioPreset subghz_txrx_get_preset(SubGhzTxRx* instance);

SubGhzRadioPreset subghz_txrx_get_rx_preset(SubGhzTxRx* instance);

/**
 * Get string frequency and modulation
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param frequency Pointer to a string frequency
 * @param modulation Pointer to a string modulation
 */
void subghz_txrx_get_frequency_and_modulation(
    SubGhzTxRx* instance,
    FuriString* frequency,
    FuriString* modulation,
    bool long_name);

/**
 * Start TX CC1101
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param flipper_format Pointer to a FlipperFormat
 * @return SubGhzTxRxStartTxState 
 */
SubGhzTxRxStartTxState subghz_txrx_tx_start(SubGhzTxRx* instance, FlipperFormat* flipper_format);

/**
 * Start RX CC1101
 * 
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_rx_start(SubGhzTxRx* instance);
void subghz_txrx_rssi_start(SubGhzTxRx* instance);

void subghz_txrx_raw_si_gate_set(SubGhzTxRx* instance, bool on);

bool subghz_txrx_raw_si_gate_is_on(SubGhzTxRx* instance);

typedef enum {
    SubGhzRawAutoAirUnknown = 0,
    SubGhzRawAutoAirAsk,
    SubGhzRawAutoAirFsk,
} SubGhzRawAutoAir;

bool subghz_txrx_raw_auto_available(const char* radio_name);

SubGhzRawAutoAir subghz_txrx_raw_auto_listen(SubGhzTxRx* instance, float arm_level_dbm);

const char* subghz_txrx_raw_auto_capture(
    SubGhzTxRx* instance,
    const char* file_name,
    uint32_t timeout_ms,
    void (*start_tx)(void* context),
    void* context,
    bool* file_error);

/**
 * Retune RX to a new frequency without reloading the modulation preset.
 *
 * @param instance Pointer to a SubGhzTxRx
 * @param frequency Frequency in Hz
 */
void subghz_txrx_rx_retune(SubGhzTxRx* instance, uint32_t frequency);

/**
 * Stop TX/RX CC1101
 * 
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_stop(SubGhzTxRx* instance);

/**
 * Set sleep mode CC1101
 * 
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_sleep(SubGhzTxRx* instance);

/**
 * Update frequency CC1101 in automatic mode (hopper)
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param stay_threshold RSSI theshold over which to stay before hopping
 */
void subghz_txrx_hopper_update(SubGhzTxRx* instance, float stay_threshold);

bool subghz_txrx_hopper_get_level(SubGhzTxRx* instance, float* rssi, float* floor);

/**
 * Notify the hopper that a decode has occurred
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_hopper_note_decode(SubGhzTxRx* instance);

/**
 * Get state hopper
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @return SubGhzHopperState 
 */
SubGhzHopperState subghz_txrx_hopper_get_state(SubGhzTxRx* instance);

/**
 * Set state hopper
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param state State hopper
 */
void subghz_txrx_hopper_set_state(SubGhzTxRx* instance, SubGhzHopperState state);

/**
 * Unpause hopper
 * 
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_hopper_unpause(SubGhzTxRx* instance);

/**
 * Set pause hopper
 * 
 * @param instance Pointer to a SubGhzTxRx
 */
void subghz_txrx_hopper_pause(SubGhzTxRx* instance);

SubGhzHopperMode subghz_txrx_hopper_get_mode(SubGhzTxRx* instance);

bool subghz_txrx_hopper_owns_frequency(SubGhzTxRx* instance);

void subghz_txrx_set_frequency(SubGhzTxRx* instance, uint32_t frequency);

uint32_t subghz_txrx_get_frequency_hz(SubGhzTxRx* instance);

void subghz_txrx_hopper_set_mode(SubGhzTxRx* instance, SubGhzHopperMode mode);

void subghz_txrx_hopper_set_presets(SubGhzTxRx* instance, size_t preset_a, size_t preset_b);

void subghz_txrx_hopper_resume(SubGhzTxRx* instance);

void subghz_txrx_hopper_suspend(SubGhzTxRx* instance);

void subghz_txrx_hopper_lock(SubGhzTxRx* instance);

void subghz_txrx_hopper_unlock(SubGhzTxRx* instance);

/**
 * Speaker on
 * 
 * @param instance Pointer to a SubGhzTxRx 
 */
void subghz_txrx_speaker_on(SubGhzTxRx* instance);

/**
 * Speaker off
 * 
 * @param instance Pointer to a SubGhzTxRx 
 */
void subghz_txrx_speaker_off(SubGhzTxRx* instance);

/**
 * Speaker mute
 * 
 * @param instance Pointer to a SubGhzTxRx 
 */
void subghz_txrx_speaker_mute(SubGhzTxRx* instance);

/**
 * Speaker unmute
 * 
 * @param instance Pointer to a SubGhzTxRx 
 */
void subghz_txrx_speaker_unmute(SubGhzTxRx* instance);

/**
 * Set state speaker
 * 
 * @param instance Pointer to a SubGhzTxRx 
 * @param state State speaker
 */
void subghz_txrx_speaker_set_state(SubGhzTxRx* instance, SubGhzSpeakerState state);

/**
 * Get state speaker
 * 
 * @param instance Pointer to a SubGhzTxRx 
 * @return SubGhzSpeakerState 
 */
SubGhzSpeakerState subghz_txrx_speaker_get_state(SubGhzTxRx* instance);

/**
 * load decoder by name protocol
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param name_protocol Name protocol
 * @return bool True if the decoder is loaded 
 */
bool subghz_txrx_load_decoder_by_name_protocol(SubGhzTxRx* instance, const char* name_protocol);

/**
 * Get decoder
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @return SubGhzProtocolDecoderBase* Pointer to a SubGhzProtocolDecoderBase
 */
SubGhzProtocolDecoderBase* subghz_txrx_get_decoder(SubGhzTxRx* instance);

/**
 * Set callback for save data
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param callback Callback for save data
 * @param context Context for callback
 */
void subghz_txrx_set_need_save_callback(
    SubGhzTxRx* instance,
    SubGhzTxRxNeedSaveCallback callback,
    void* context);

/**
 * Get pointer to a load data key
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @return FlipperFormat* 
 */
FlipperFormat* subghz_txrx_get_fff_data(SubGhzTxRx* instance);

/**
 * Get pointer to a SugGhzSetting
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @return SubGhzSetting* 
 */
SubGhzSetting* subghz_txrx_get_setting(SubGhzTxRx* instance);

/**
 * Is it possible to save this protocol
 * 
 * @param instance Pointer to a SubGhzTxRx 
 * @return bool True if it is possible to save this protocol
 */
bool subghz_txrx_protocol_is_serializable(SubGhzTxRx* instance);

/**
 * Is it possible to send this protocol
 * 
 * @param instance Pointer to a SubGhzTxRx 
 * @return bool True if it is possible to send this protocol
 */
bool subghz_txrx_protocol_is_transmittable(SubGhzTxRx* instance, bool check_type);

/**
 * Set filter, what types of decoder to use 
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param filter Filter
 */
void subghz_txrx_receiver_set_filter(SubGhzTxRx* instance, SubGhzProtocolFlag filter);

/**
 * Set callback for receive data
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param callback Callback for receive data
 * @param context Context for callback
 */
void subghz_txrx_set_rx_callback(
    SubGhzTxRx* instance,
    SubGhzReceiverCallback callback,
    void* context);

/**
 * Set callback for Raw decoder, end of data transfer  
 * 
 * @param instance Pointer to a SubGhzTxRx
 * @param callback Callback for Raw decoder, end of data transfer 
 * @param context Context for callback
 */
void subghz_txrx_set_raw_file_encoder_worker_callback_end(
    SubGhzTxRx* instance,
    SubGhzProtocolEncoderRAWCallbackEnd callback,
    void* context);

/* Checking if an external radio device is connected
* 
* @param instance Pointer to a SubGhzTxRx
* @param name Name of external radio device
* @return bool True if is connected to the external radio device
*/
bool subghz_txrx_radio_device_is_external_connected(SubGhzTxRx* instance, const char* name);

const char* subghz_txrx_radio_device_detect_external(SubGhzTxRx* instance);

/* Set the selected radio device to use
*
* @param instance Pointer to a SubGhzTxRx
* @param radio_device_type Radio device type
* @return SubGhzRadioDeviceType Type of installed radio device
*/
SubGhzRadioDeviceType
    subghz_txrx_radio_device_set(SubGhzTxRx* instance, SubGhzRadioDeviceType radio_device_type);

/* Get the selected radio device to use
*
* @param instance Pointer to a SubGhzTxRx
* @return SubGhzRadioDeviceType Type of installed radio device
*/
SubGhzRadioDeviceType subghz_txrx_radio_device_get(SubGhzTxRx* instance);

/** The radio currently selected, for helpers that drive the device directly
 * rather than through TxRx - the Auto Tune worker owns the radio for the
 * lifetime of its scene.
 *
 * @param instance Pointer to a SubGhzTxRx instance
 * @return Pointer to the active SubGhzDevice
 */
const SubGhzDevice* subghz_txrx_radio_device_get_ptr(SubGhzTxRx* instance);

/* Get RSSI the selected radio device to use
*
* @param instance Pointer to a SubGhzTxRx
* @return float RSSI
*/
float subghz_txrx_radio_device_get_rssi(SubGhzTxRx* instance);

/* Get name the selected radio device to use
*
* @param instance Pointer to a SubGhzTxRx
* @return const char* Name of installed radio device
*/
const char* subghz_txrx_radio_device_get_name(SubGhzTxRx* instance);

/* Get intelligence whether frequency the selected radio device to use
*
* @param instance Pointer to a SubGhzTxRx
* @return bool True if the frequency is valid
*/
bool subghz_txrx_radio_device_is_frequency_valid(SubGhzTxRx* instance, uint32_t frequency);

bool subghz_txrx_preset_fits_radio(SubGhzTxRx* instance, size_t preset_idx);

bool subghz_txrx_preset_fits_kind(SubGhzTxRx* instance, size_t preset_idx, bool si4463);

uint8_t subghz_txrx_preset_make_valid(SubGhzTxRx* instance, uint8_t tx_power);

#define SUBGHZ_TXRX_FALLBACK_FREQUENCY 433920000UL

size_t subghz_txrx_frequency_valid_index(SubGhzTxRx* instance, size_t index, bool next);

uint32_t subghz_txrx_frequency_make_valid(SubGhzTxRx* instance, uint32_t frequency);

bool subghz_txrx_radio_device_is_tx_allowed(SubGhzTxRx* instance, uint32_t frequency);

void subghz_txrx_set_debug_pin_state(SubGhzTxRx* instance, bool state);
bool subghz_txrx_get_debug_pin_state(SubGhzTxRx* instance);

void subghz_txrx_reset_dynamic_and_custom_btns(SubGhzTxRx* instance);

SubGhzReceiver* subghz_txrx_get_receiver(SubGhzTxRx* instance); // TODO use only in DecodeRaw

SubGhzEnvironment* subghz_txrx_get_environment(SubGhzTxRx* instance);

/**
 * @brief Set current preset AM650 without additional params
 * 
 * @param instance - instance Pointer to a SubGhzTxRx
 * @param frequency - frequency of preset, if pass 0 then taking default frequency 433.92MHz
 */
void subghz_txrx_set_default_preset(SubGhzTxRx* instance, uint32_t frequency);

/**
 * @brief Set current preset by index
 * 
 * @param instance  - instance Pointer to a SubGhzTxRx
 * @param frequency - frequency of new preset
 * @param index - index of preset taken from SubGhzSetting
 * @param tx_power - index of TX Power menu index option to use.
 * @return const char* -  name of preset
 */
const char* subghz_txrx_set_preset_internal(
    SubGhzTxRx* instance,
    uint32_t frequency,
    uint8_t index,
    uint8_t tx_power);

/**
 * @brief Set the internal-radio preset used by dual RX (persists for session).
 */
void subghz_txrx_dual_set_preset_int(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency);

/**
 * @brief Set the external-radio preset used by dual RX (persists for session).
 */
void subghz_txrx_dual_set_preset_ext(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency);

/** @brief Get a copy of the internal-radio dual preset. */
SubGhzRadioPreset subghz_txrx_dual_get_preset_int(SubGhzTxRx* instance);

/** @brief Get a copy of the external-radio dual preset. */
SubGhzRadioPreset subghz_txrx_dual_get_preset_ext(SubGhzTxRx* instance);

/** @brief Format the "frequency" / "modulation" strings for one dual radio. */
void subghz_txrx_dual_get_frequency_and_modulation(
    SubGhzTxRx* instance,
    bool external,
    FuriString* frequency,
    FuriString* modulation,
    bool long_name);

/**
 * @brief Start simultaneous RX on both radios using the current dual presets.
 *        if dual RX is already running it is restarted with the
 *        current presets.
 * @return true if both radios started (external requires a connected module).
 */
bool subghz_txrx_dual_rx_start(SubGhzTxRx* instance);

/** @brief Stop dual RX and release the external radio + secondary chain. */
void subghz_txrx_dual_rx_stop(SubGhzTxRx* instance);

/** @brief Whether dual RX is currently running. */
bool subghz_txrx_dual_is_active(SubGhzTxRx* instance);

/** @brief Whether an external CC1101 module is connected (probes hardware). */
bool subghz_txrx_dual_is_ext_connected(SubGhzTxRx* instance);

/** @brief Whether the connected external module is an SI4463. */
bool subghz_txrx_dual_ext_is_si4463(SubGhzTxRx* instance);

/**
 * @brief Map each dual preset onto one that radio can load.
 *        Internal stays on CC1101 presets. External uses SI4463 presets only
 *        when ext_si4463 is true, same rule as single-radio Read.
 */
void subghz_txrx_dual_coerce_presets(SubGhzTxRx* instance, bool ext_si4463);

/**
 * @brief Install per-radio decode callbacks for dual RX.
 * @param callback_int Called when the internal radio decodes a protocol.
 * @param callback_ext Called when the external radio decodes a protocol.
 * @param context Shared context passed to both callbacks.
 */
void subghz_txrx_dual_set_rx_callback(
    SubGhzTxRx* instance,
    SubGhzReceiverCallback callback_int,
    SubGhzReceiverCallback callback_ext,
    void* context);

/** @brief Apply a protocol filter to both dual receivers. */
void subghz_txrx_dual_set_filter(SubGhzTxRx* instance, SubGhzProtocolFlag filter);

/**
 * @brief Acquire/release the shared dual decode lock. The decode callbacks run
 * under this lock, so GUI-thread code must take it before reading or mutating
 * the shared history while dual RX is running. No-op if dual RX is not active.
 */
void subghz_txrx_dual_lock(SubGhzTxRx* instance);
void subghz_txrx_dual_unlock(SubGhzTxRx* instance);

/** @brief Current RSSI of the internal radio (dual mode). */
float subghz_txrx_dual_get_rssi_int(SubGhzTxRx* instance);

/** @brief Current RSSI of the external radio (dual mode). */
float subghz_txrx_dual_get_rssi_ext(SubGhzTxRx* instance);

/**
 * @brief Look up a decoder on the external receiver by protocol name.
 */
SubGhzProtocolDecoderBase*
    subghz_txrx_dual_get_decoder_ext_by_name(SubGhzTxRx* instance, const char* name_protocol);

/**
 * Encode key file data to a RAW .sub file.
 * @param instance Pointer to a SubGhzTxRx
 * @param flipper_format Pointer to a FlipperFormat with key data
 * @param file_path Full path to output file
 * @return true On success
 */
bool subghz_txrx_save_raw_from_key(
    SubGhzTxRx* instance,
    FlipperFormat* flipper_format,
    const char* file_path);

#ifdef __cplusplus
}
#endif

bool subghz_txrx_dual_begin(SubGhzTxRx* instance);
