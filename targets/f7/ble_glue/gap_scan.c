#include "gap_scan.h"

#include "app_common.h"
#include "furi_ble/event_dispatcher.h"
#include <ble/ble.h>

#include <furi.h>

#define TAG "BleGapScan"

/* AD types, Bluetooth spec v5.4 */
#define GAP_SCAN_AD_TYPE_NAME_SHORT    0x08
#define GAP_SCAN_AD_TYPE_NAME_COMPLETE 0x09

/* Scan interval/window are in 0.625 ms units. Scanning continuously (window ==
 * interval) starves advertising, so leave a gap for the peripheral role. */
#define GAP_SCAN_INTERVAL 0x00A0 /* 100 ms */
#define GAP_SCAN_WINDOW   0x0060 /* 60 ms */

#define GAP_SCAN_OWN_ADDR_TYPE   0x00 /* public */
#define GAP_SCAN_FILTER_DUPS_OFF 0x00

typedef struct {
    GapScanCallback callback;
    void* context;
    GapSvcEventHandler* event_handler;
    uint8_t last_error;
} GapScan;

static GapScan gap_scan = {0};

static void gap_scan_extract_name(const uint8_t* data, uint8_t data_len, char* out) {
    out[0] = '\0';
    uint8_t pos = 0;
    /* Each AD structure is [length][type][payload], where length covers type+payload */
    while(pos < data_len) {
        uint8_t ad_len = data[pos];
        if(ad_len == 0 || (pos + 1 + ad_len) > data_len) break;

        uint8_t ad_type = data[pos + 1];
        if(ad_type == GAP_SCAN_AD_TYPE_NAME_COMPLETE || ad_type == GAP_SCAN_AD_TYPE_NAME_SHORT) {
            uint8_t name_len = ad_len - 1;
            if(name_len > GAP_SCAN_NAME_MAX - 1) name_len = GAP_SCAN_NAME_MAX - 1;
            for(uint8_t i = 0; i < name_len; i++) {
                uint8_t ch = data[pos + 2 + i];
                out[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
            }
            out[name_len] = '\0';
            return;
        }
        pos += ad_len + 1;
    }
}

static BleEventAckStatus gap_scan_event_handler(void* event, void* context) {
    UNUSED(context);

    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)event)->data;
    if(event_pckt->evt != HCI_LE_META_EVT_CODE) return BleEventNotAck;

    evt_le_meta_event* meta_evt = (evt_le_meta_event*)event_pckt->data;
    if(meta_evt->subevent != HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE) return BleEventNotAck;

    /* Per-report wire layout: event type, address type, address[6], data length,
     * data[length], rssi. Mirrors hci_le_advertising_report_event_process()
     * in lib/stm32wb_copro/wpan/ble/core/auto/ble_events.c. */
    const uint8_t* in = meta_evt->data;
    uint8_t num_reports = in[0];

    for(uint8_t i = 0; i < num_reports; i++) {
        in += 1;

        GapScanReport report = {
            .event_type = in[0],
            .address_type = in[1],
        };
        memcpy(report.address, &in[2], sizeof(report.address));

        uint8_t data_len = in[8];
        gap_scan_extract_name(&in[9], data_len, report.name);

        in += 9 + data_len;
        report.rssi = (int8_t)in[0];

        if(gap_scan.callback) {
            gap_scan.callback(&report, gap_scan.context);
        }
    }

    /* Consume the event - nothing else in the firmware handles advertising reports */
    return BleEventAckFlowEnable;
}

bool gap_scan_start(GapScanCallback callback, void* context) {
    furi_check(callback);
    furi_check(context);

    if(gap_scan.event_handler) {
        FURI_LOG_W(TAG, "Already scanning");
        return false;
    }

    gap_scan.callback = callback;
    gap_scan.context = context;
    gap_scan.last_error = 0;
    gap_scan.event_handler =
        ble_event_dispatcher_register_svc_handler(gap_scan_event_handler, &gap_scan);

    /* The stack runs in LL+Host mode (SHCI_C2_BLE_INIT_OPTIONS_LL_HOST), where the
     * host owns the scan state machine and raw hci_le_set_scan_* is rejected with
     * "unknown command". Discovery has to go through the GAP procedure instead.
     * Duplicate filtering is left off so RSSI keeps updating. */
    tBleStatus status = aci_gap_start_general_discovery_proc(
        GAP_SCAN_INTERVAL, GAP_SCAN_WINDOW, GAP_SCAN_OWN_ADDR_TYPE, GAP_SCAN_FILTER_DUPS_OFF);
    if(status) {
        FURI_LOG_E(TAG, "Failed to start discovery: 0x%02X", status);
        gap_scan_stop();
        gap_scan.last_error = status;
        return false;
    }

    FURI_LOG_I(TAG, "Scanning");
    return true;
}

uint8_t gap_scan_get_last_error(void) {
    return gap_scan.last_error;
}

void gap_scan_stop(void) {
    if(!gap_scan.event_handler) return;

    tBleStatus status = aci_gap_terminate_gap_proc(GAP_GENERAL_DISCOVERY_PROC);
    /* 0x42 (command disallowed) just means the procedure already ended */
    if(status) {
        FURI_LOG_D(TAG, "Terminate discovery returned 0x%02X", status);
    }

    ble_event_dispatcher_unregister_svc_handler(gap_scan.event_handler);
    gap_scan.event_handler = NULL;
    gap_scan.callback = NULL;
    gap_scan.context = NULL;
}

bool gap_scan_is_active(void) {
    return gap_scan.event_handler != NULL;
}
