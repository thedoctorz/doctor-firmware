#pragma once

#include "subghz_txrx.h"
#include "subghz_activity.h"

typedef struct SubGhzTxRxActivity SubGhzTxRxActivity;

void subghz_txrx_activity_prepare(SubGhzTxRx* instance, bool dual);
void subghz_txrx_activity_start(SubGhzTxRx* instance);
void subghz_txrx_activity_stop(SubGhzTxRx* instance);
void subghz_txrx_activity_free(SubGhzTxRx* instance);
void subghz_txrx_activity_read(
    SubGhzTxRx* instance,
    uint8_t source,
    SubGhzActivityHistory* history);
void subghz_txrx_activity_note_decode(SubGhzTxRx* instance, uint8_t source);

void subghz_txrx_activity_pause(SubGhzTxRx* instance);
void subghz_txrx_activity_rx(SubGhzTxRx* instance, bool hop);
void subghz_txrx_activity_hopper_sample(SubGhzTxRx* instance, float rssi);
