#pragma once

#include <gui/view.h>
#include "../helpers/subghz_types.h"
#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzViewReceiver SubGhzViewReceiver;

typedef void (*SubGhzViewReceiverCallback)(SubGhzCustomEvent event, void* context);

void subghz_view_receiver_set_mode(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverMode mode);

void subghz_receiver_rssi(SubGhzViewReceiver* instance, float rssi, float floor);

void subghz_view_receiver_rssi_ext(SubGhzViewReceiver* instance, float rssi);

void subghz_view_receiver_set_lock(SubGhzViewReceiver* subghz_receiver, bool keyboard);

void subghz_view_receiver_set_callback(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverCallback callback,
    void* context);

typedef uint32_t (*SubGhzViewReceiverFrequencyCallback)(void* context);

void subghz_view_receiver_set_frequency_callback(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzViewReceiverFrequencyCallback callback);

SubGhzViewReceiver* subghz_view_receiver_alloc(void);

void subghz_view_receiver_free(SubGhzViewReceiver* subghz_receiver);

View* subghz_view_receiver_get_view(SubGhzViewReceiver* subghz_receiver);

void subghz_view_receiver_add_data_statusbar(
    SubGhzViewReceiver* subghz_receiver,
    const char* frequency_str,
    const char* preset_str,
    const char* history_stat_str,
    bool hopping_enabled,
    bool bin_raw_enabled,
    bool gps_active,
    bool gps_companion,
    uint8_t gps_sats);

void subghz_view_receiver_set_radio_device_type(
    SubGhzViewReceiver* subghz_receiver,
    SubGhzRadioDeviceType device_type);

/**
 * @param subghz_receiver Pointer to a SubGhzViewReceiver
 * @param dual_mode Whether dual mode is active
 * @param int_str Internal radio summary, e.g. "433.92 AM650"
 * @param ext_str External radio summary, e.g. "433.92 FM476"
 */
void subghz_view_receiver_set_dual(
    SubGhzViewReceiver* subghz_receiver,
    bool dual_mode,
    const char* int_str,
    const char* ext_str);

void subghz_view_receiver_set_dual_dashboard(SubGhzViewReceiver* subghz_receiver, bool dashboard);

bool subghz_view_receiver_get_dual_dashboard(SubGhzViewReceiver* subghz_receiver);

void subghz_view_receiver_add_data_progress(
    SubGhzViewReceiver* subghz_receiver,
    const char* progress_str);

void subghz_view_receiver_add_item_to_menu(
    SubGhzViewReceiver* subghz_receiver,
    const char* name,
    const char* time,
    uint8_t type,
    SubGhzHistoryButtonIcon button_icon);

uint16_t subghz_view_receiver_get_idx_menu(SubGhzViewReceiver* subghz_receiver);

void subghz_view_receiver_set_idx_menu(SubGhzViewReceiver* subghz_receiver, uint16_t idx);

void subghz_view_receiver_delete_item(SubGhzViewReceiver* subghz_receiver, uint16_t idx);

void subghz_view_receiver_clear_menu(SubGhzViewReceiver* subghz_receiver);

void subghz_view_receiver_enable_draw_callback(SubGhzViewReceiver* subghz_receiver);

void subghz_view_receiver_disable_draw_callback(SubGhzViewReceiver* subghz_receiver);

void subghz_view_receiver_exit(void* context);

void subghz_view_receiver_set_raw_rx_tx(
    SubGhzViewReceiver* receiver, bool enabled, bool transmitting, const char* filename);
