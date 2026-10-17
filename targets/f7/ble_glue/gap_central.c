#include "gap_central.h"

#include "app_common.h"
#include "furi_ble/event_dispatcher.h"
#include <ble/ble.h>

#include <furi.h>

#define TAG "BleGapCentral"

/* Connection interval is in 1.25 ms units, supervision timeout in 10 ms units */
#define GAP_CENTRAL_SCAN_INTERVAL  0x00A0 /* 100 ms */
#define GAP_CENTRAL_SCAN_WINDOW    0x0060 /* 60 ms */
#define GAP_CENTRAL_CONN_INT_MIN   0x0018 /* 30 ms */
#define GAP_CENTRAL_CONN_INT_MAX   0x0028 /* 50 ms */
#define GAP_CENTRAL_CONN_LATENCY   0x0000
#define GAP_CENTRAL_SUPERVISION    0x00C8 /* 2000 ms */
#define GAP_CENTRAL_CE_LENGTH      0x0000
#define GAP_CENTRAL_OWN_ADDR_TYPE  0x00 /* public */
#define GAP_CENTRAL_ROLE_IS_MASTER 0x00

/* HCI "remote user terminated connection" */
#define GAP_CENTRAL_DISCONNECT_REASON 0x13

#define GAP_CENTRAL_FLAG_CONNECTED     (1UL << 0)
#define GAP_CENTRAL_FLAG_DISCONNECTED  (1UL << 1)
#define GAP_CENTRAL_FLAG_PROC_COMPLETE (1UL << 2)
#define GAP_CENTRAL_FLAG_ALL \
    (GAP_CENTRAL_FLAG_CONNECTED | GAP_CENTRAL_FLAG_DISCONNECTED | GAP_CENTRAL_FLAG_PROC_COMPLETE)

#define GAP_CENTRAL_FLAG_MTU_UPDATED (1UL << 3)

/* ATT default, and the CCCD bit that turns notifications on */
#define GAP_CENTRAL_DEFAULT_MTU  23
#define GAP_CENTRAL_CCCD_NOTIFY  0x0001
#define GAP_CENTRAL_CCCD_DISABLE 0x0000

typedef enum {
    GapCentralCollectNone,
    GapCentralCollectServices,
    GapCentralCollectCharacteristics,
    GapCentralCollectDescriptors,
    GapCentralCollectRead,
} GapCentralCollectMode;

typedef struct {
    GapSvcEventHandler* event_handler;
    FuriEventFlag* flags;

    uint16_t conn_handle;
    bool connected;
    uint8_t last_error;
    uint16_t mtu;

    GapCentralNotifyCallback notify_callback;
    void* notify_context;

    GapCentralCollectMode mode;
    void* buffer;
    size_t capacity;
    size_t count;
} GapCentral;

static GapCentral gap_central = {0};

static void gap_central_collect_service(const uint8_t* data, uint8_t entry_len, uint8_t total) {
    /* Each entry: start handle, end handle, then a 2- or 16-byte UUID */
    if(entry_len < 6) return;
    uint8_t uuid_len = entry_len - 4;
    if(uuid_len > GAP_CENTRAL_UUID_MAX) return;

    GapCentralService* out = gap_central.buffer;
    for(uint8_t offset = 0; offset + entry_len <= total; offset += entry_len) {
        if(gap_central.count >= gap_central.capacity) return;
        const uint8_t* entry = &data[offset];
        GapCentralService* service = &out[gap_central.count++];
        service->start_handle = entry[0] | (entry[1] << 8);
        service->end_handle = entry[2] | (entry[3] << 8);
        service->uuid_len = uuid_len;
        memcpy(service->uuid, &entry[4], uuid_len);
    }
}

static void
    gap_central_collect_characteristic(const uint8_t* data, uint8_t entry_len, uint8_t total) {
    /* Each entry: declaration handle, properties, value handle, then the UUID */
    if(entry_len < 7) return;
    uint8_t uuid_len = entry_len - 5;
    if(uuid_len > GAP_CENTRAL_UUID_MAX) return;

    GapCentralCharacteristic* out = gap_central.buffer;
    for(uint8_t offset = 0; offset + entry_len <= total; offset += entry_len) {
        if(gap_central.count >= gap_central.capacity) return;
        const uint8_t* entry = &data[offset];
        GapCentralCharacteristic* characteristic = &out[gap_central.count++];
        characteristic->properties = entry[2];
        characteristic->value_handle = entry[3] | (entry[4] << 8);
        characteristic->uuid_len = uuid_len;
        memcpy(characteristic->uuid, &entry[5], uuid_len);
    }
}

static void gap_central_collect_descriptor(uint8_t format, const uint8_t* data, uint8_t total) {
    /* Format 1: handle + 16-bit UUID. Format 2: handle + 128-bit UUID. */
    uint8_t uuid_len = (format == 0x01) ? 2 : 16;
    uint8_t entry_len = uuid_len + 2;

    GapCentralDescriptor* out = gap_central.buffer;
    for(uint8_t offset = 0; offset + entry_len <= total; offset += entry_len) {
        if(gap_central.count >= gap_central.capacity) return;
        const uint8_t* entry = &data[offset];
        GapCentralDescriptor* descriptor = &out[gap_central.count++];
        descriptor->handle = entry[0] | (entry[1] << 8);
        descriptor->uuid_len = uuid_len;
        memcpy(descriptor->uuid, &entry[2], uuid_len);
    }
}

static void gap_central_collect_read(const uint8_t* data, uint8_t length) {
    uint8_t* out = gap_central.buffer;
    /* A long value arrives across several events, so append rather than overwrite */
    for(uint8_t i = 0; i < length && gap_central.count < gap_central.capacity; i++) {
        out[gap_central.count++] = data[i];
    }
}

static BleEventAckStatus gap_central_event_handler(void* event, void* context) {
    UNUSED(context);

    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)event)->data;

    if(event_pckt->evt == HCI_DISCONNECTION_COMPLETE_EVT_CODE) {
        hci_disconnection_complete_event_rp0* disconnect =
            (hci_disconnection_complete_event_rp0*)event_pckt->data;
        if(!gap_central.connected || disconnect->Connection_Handle != gap_central.conn_handle) {
            /* Not our link - let the peripheral state machine in gap.c have it */
            return BleEventNotAck;
        }
        gap_central.connected = false;
        gap_central.conn_handle = 0;
        furi_event_flag_set(gap_central.flags, GAP_CENTRAL_FLAG_DISCONNECTED);
        return BleEventAckFlowEnable;
    }

    if(event_pckt->evt == HCI_LE_META_EVT_CODE) {
        evt_le_meta_event* meta_evt = (evt_le_meta_event*)event_pckt->data;
        if(meta_evt->subevent != HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE) return BleEventNotAck;

        hci_le_connection_complete_event_rp0* connection =
            (hci_le_connection_complete_event_rp0*)meta_evt->data;
        /* Role tells us who initiated. Incoming connections (we are the peripheral)
         * must fall through to gap.c, which owns advertising and slave pairing. */
        if(connection->Role != GAP_CENTRAL_ROLE_IS_MASTER) return BleEventNotAck;

        gap_central.last_error = connection->Status;
        if(connection->Status == 0) {
            gap_central.conn_handle = connection->Connection_Handle;
            gap_central.connected = true;
        }
        furi_event_flag_set(gap_central.flags, GAP_CENTRAL_FLAG_CONNECTED);
        return BleEventAckFlowEnable;
    }

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;
    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
        aci_att_read_by_group_type_resp_event_rp0* resp =
            (aci_att_read_by_group_type_resp_event_rp0*)blue_evt->data;
        if(gap_central.mode != GapCentralCollectServices ||
           resp->Connection_Handle != gap_central.conn_handle) {
            return BleEventNotAck;
        }
        gap_central_collect_service(
            resp->Attribute_Data_List, resp->Attribute_Data_Length, resp->Data_Length);
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
        aci_att_read_by_type_resp_event_rp0* resp =
            (aci_att_read_by_type_resp_event_rp0*)blue_evt->data;
        if(gap_central.mode != GapCentralCollectCharacteristics ||
           resp->Connection_Handle != gap_central.conn_handle) {
            return BleEventNotAck;
        }
        gap_central_collect_characteristic(
            resp->Handle_Value_Pair_Data, resp->Handle_Value_Pair_Length, resp->Data_Length);
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE: {
        aci_att_find_info_resp_event_rp0* resp = (aci_att_find_info_resp_event_rp0*)blue_evt->data;
        if(gap_central.mode != GapCentralCollectDescriptors ||
           resp->Connection_Handle != gap_central.conn_handle) {
            return BleEventNotAck;
        }
        gap_central_collect_descriptor(
            resp->Format, resp->Handle_UUID_Pair, resp->Event_Data_Length);
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_READ_RESP_VSEVT_CODE: {
        aci_att_read_resp_event_rp0* resp = (aci_att_read_resp_event_rp0*)blue_evt->data;
        if(gap_central.mode != GapCentralCollectRead ||
           resp->Connection_Handle != gap_central.conn_handle) {
            return BleEventNotAck;
        }
        gap_central_collect_read(resp->Attribute_Value, resp->Event_Data_Length);
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE: {
        aci_att_exchange_mtu_resp_event_rp0* resp =
            (aci_att_exchange_mtu_resp_event_rp0*)blue_evt->data;
        if(resp->Connection_Handle != gap_central.conn_handle) return BleEventNotAck;
        gap_central.mtu = resp->Server_RX_MTU;
        furi_event_flag_set(gap_central.flags, GAP_CENTRAL_FLAG_MTU_UPDATED);
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_NOTIFICATION_VSEVT_CODE:
    case ACI_GATT_INDICATION_VSEVT_CODE: {
        aci_gatt_notification_event_rp0* notification =
            (aci_gatt_notification_event_rp0*)blue_evt->data;
        if(notification->Connection_Handle != gap_central.conn_handle) return BleEventNotAck;
        if(gap_central.notify_callback) {
            gap_central.notify_callback(
                notification->Attribute_Handle,
                notification->Attribute_Value,
                notification->Attribute_Value_Length,
                gap_central.notify_context);
        }
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        aci_gatt_proc_complete_event_rp0* complete =
            (aci_gatt_proc_complete_event_rp0*)blue_evt->data;
        if(complete->Connection_Handle != gap_central.conn_handle) return BleEventNotAck;
        gap_central.last_error = complete->Error_Code;
        furi_event_flag_set(gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE);
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_ERROR_RESP_VSEVT_CODE: {
        aci_gatt_error_resp_event_rp0* error = (aci_gatt_error_resp_event_rp0*)blue_evt->data;
        if(error->Connection_Handle != gap_central.conn_handle) return BleEventNotAck;
        /* An error response still ends with a procedure complete event; record the
         * ATT reason but let the wait below finish normally. "Attribute not found"
         * is the ordinary way a discovery reports that it ran off the end. */
        FURI_LOG_D(
            TAG, "ATT error 0x%02X on handle 0x%04X", error->Error_Code, error->Attribute_Handle);
        return BleEventAckFlowEnable;
    }

    default:
        return BleEventNotAck;
    }
}

static void gap_central_arm(GapCentralCollectMode mode, void* buffer, size_t capacity) {
    gap_central.buffer = buffer;
    gap_central.capacity = capacity;
    gap_central.count = 0;
    /* Written last: the handler gates on mode */
    gap_central.mode = mode;
}

static size_t gap_central_disarm(void) {
    /* Cleared first, so the handler stops touching the caller's buffer */
    gap_central.mode = GapCentralCollectNone;
    size_t count = gap_central.count;
    gap_central.buffer = NULL;
    gap_central.capacity = 0;
    gap_central.count = 0;
    return count;
}

bool gap_central_connect(const uint8_t* address, uint8_t address_type, uint32_t timeout_ms) {
    furi_check(address);

    if(gap_central.connected) {
        FURI_LOG_W(TAG, "Already connected");
        return false;
    }

    if(!gap_central.flags) {
        gap_central.flags = furi_event_flag_alloc();
    }
    if(!gap_central.event_handler) {
        gap_central.event_handler =
            ble_event_dispatcher_register_svc_handler(gap_central_event_handler, &gap_central);
    }
    furi_event_flag_clear(gap_central.flags, GAP_CENTRAL_FLAG_ALL | GAP_CENTRAL_FLAG_MTU_UPDATED);
    gap_central.last_error = 0;
    gap_central.mtu = GAP_CENTRAL_DEFAULT_MTU;

    tBleStatus status = aci_gap_create_connection(
        GAP_CENTRAL_SCAN_INTERVAL,
        GAP_CENTRAL_SCAN_WINDOW,
        address_type,
        address,
        GAP_CENTRAL_OWN_ADDR_TYPE,
        GAP_CENTRAL_CONN_INT_MIN,
        GAP_CENTRAL_CONN_INT_MAX,
        GAP_CENTRAL_CONN_LATENCY,
        GAP_CENTRAL_SUPERVISION,
        GAP_CENTRAL_CE_LENGTH,
        GAP_CENTRAL_CE_LENGTH);
    if(status) {
        FURI_LOG_E(TAG, "Failed to create connection: 0x%02X", status);
        gap_central.last_error = status;
        gap_central_disconnect();
        return false;
    }

    uint32_t flags = furi_event_flag_wait(
        gap_central.flags, GAP_CENTRAL_FLAG_CONNECTED, FuriFlagWaitAny, timeout_ms);

    if((flags & GAP_CENTRAL_FLAG_CONNECTED) == 0 || !gap_central.connected) {
        FURI_LOG_E(TAG, "Connection failed, status 0x%02X", gap_central.last_error);
        /* Cancel the pending attempt so the controller stops initiating */
        aci_gap_terminate_gap_proc(GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC);
        gap_central_disconnect();
        return false;
    }

    FURI_LOG_I(TAG, "Connected, handle 0x%04X", gap_central.conn_handle);
    return true;
}

void gap_central_disconnect(void) {
    if(gap_central.connected) {
        furi_event_flag_clear(gap_central.flags, GAP_CENTRAL_FLAG_DISCONNECTED);
        tBleStatus status =
            aci_gap_terminate(gap_central.conn_handle, GAP_CENTRAL_DISCONNECT_REASON);
        if(status) {
            FURI_LOG_E(TAG, "Failed to terminate: 0x%02X", status);
            gap_central.connected = false;
        } else {
            furi_event_flag_wait(
                gap_central.flags, GAP_CENTRAL_FLAG_DISCONNECTED, FuriFlagWaitAny, 2000);
        }
    }

    gap_central_disarm();
    gap_central.connected = false;
    gap_central.conn_handle = 0;
    gap_central.mtu = GAP_CENTRAL_DEFAULT_MTU;
    /* Cleared before unregistering so no late notification reaches a dead context */
    gap_central.notify_callback = NULL;
    gap_central.notify_context = NULL;

    if(gap_central.event_handler) {
        ble_event_dispatcher_unregister_svc_handler(gap_central.event_handler);
        gap_central.event_handler = NULL;
    }
}

bool gap_central_is_connected(void) {
    return gap_central.connected;
}

/* Runs one GATT discovery procedure and waits for its completion event */
static int32_t gap_central_run_discovery(
    GapCentralCollectMode mode,
    void* out,
    size_t max_count,
    uint32_t timeout_ms,
    tBleStatus (*start)(uint16_t, uint16_t, uint16_t),
    uint16_t arg1,
    uint16_t arg2) {
    if(!gap_central.connected) {
        FURI_LOG_E(TAG, "Not connected");
        return -1;
    }

    furi_event_flag_clear(gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE);
    gap_central_arm(mode, out, max_count);

    tBleStatus status = start(gap_central.conn_handle, arg1, arg2);
    if(status) {
        FURI_LOG_E(TAG, "Failed to start discovery: 0x%02X", status);
        gap_central.last_error = status;
        gap_central_disarm();
        return -1;
    }

    uint32_t flags = furi_event_flag_wait(
        gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE, FuriFlagWaitAny, timeout_ms);

    size_t count = gap_central_disarm();

    if((flags & GAP_CENTRAL_FLAG_PROC_COMPLETE) == 0) {
        FURI_LOG_E(TAG, "Discovery timed out");
        return -1;
    }

    return (int32_t)count;
}

static tBleStatus gap_central_start_services(uint16_t handle, uint16_t unused1, uint16_t unused2) {
    UNUSED(unused1);
    UNUSED(unused2);
    return aci_gatt_disc_all_primary_services(handle);
}

static tBleStatus
    gap_central_start_characteristics(uint16_t handle, uint16_t start, uint16_t end) {
    return aci_gatt_disc_all_char_of_service(handle, start, end);
}

int32_t
    gap_central_discover_services(GapCentralService* out, size_t max_count, uint32_t timeout_ms) {
    furi_check(out);
    return gap_central_run_discovery(
        GapCentralCollectServices, out, max_count, timeout_ms, gap_central_start_services, 0, 0);
}

int32_t gap_central_discover_characteristics(
    const GapCentralService* service,
    GapCentralCharacteristic* out,
    size_t max_count,
    uint32_t timeout_ms) {
    furi_check(service);
    furi_check(out);
    return gap_central_run_discovery(
        GapCentralCollectCharacteristics,
        out,
        max_count,
        timeout_ms,
        gap_central_start_characteristics,
        service->start_handle,
        service->end_handle);
}

static tBleStatus gap_central_start_descriptors(uint16_t handle, uint16_t start, uint16_t end) {
    return aci_gatt_disc_all_char_desc(handle, start, end);
}

static tBleStatus gap_central_start_read(uint16_t handle, uint16_t attr, uint16_t unused) {
    UNUSED(unused);
    return aci_gatt_read_char_value(handle, attr);
}

int32_t gap_central_discover_descriptors(
    uint16_t value_handle,
    uint16_t end_handle,
    GapCentralDescriptor* out,
    size_t max_count,
    uint32_t timeout_ms) {
    furi_check(out);
    return gap_central_run_discovery(
        GapCentralCollectDescriptors,
        out,
        max_count,
        timeout_ms,
        gap_central_start_descriptors,
        value_handle,
        end_handle);
}

int32_t
    gap_central_read(uint16_t value_handle, uint8_t* out, size_t max_length, uint32_t timeout_ms) {
    furi_check(out);
    return gap_central_run_discovery(
        GapCentralCollectRead,
        out,
        max_length,
        timeout_ms,
        gap_central_start_read,
        value_handle,
        0);
}

bool gap_central_write(
    uint16_t value_handle,
    const uint8_t* data,
    uint8_t length,
    bool with_response,
    uint32_t timeout_ms) {
    furi_check(data);

    if(!gap_central.connected) {
        FURI_LOG_E(TAG, "Not connected");
        return false;
    }

    if(!with_response) {
        /* Write Command: no ATT ack and no procedure complete event */
        tBleStatus status =
            aci_gatt_write_without_resp(gap_central.conn_handle, value_handle, length, data);
        if(status) {
            FURI_LOG_E(TAG, "Write without response failed: 0x%02X", status);
            gap_central.last_error = status;
            return false;
        }
        return true;
    }

    furi_event_flag_clear(gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE);
    tBleStatus status =
        aci_gatt_write_char_value(gap_central.conn_handle, value_handle, length, data);
    if(status) {
        FURI_LOG_E(TAG, "Write failed: 0x%02X", status);
        gap_central.last_error = status;
        return false;
    }

    uint32_t flags = furi_event_flag_wait(
        gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE, FuriFlagWaitAny, timeout_ms);
    if((flags & GAP_CENTRAL_FLAG_PROC_COMPLETE) == 0) {
        FURI_LOG_E(TAG, "Write timed out");
        return false;
    }
    return gap_central.last_error == 0;
}

void gap_central_set_notify_callback(GapCentralNotifyCallback callback, void* context) {
    gap_central.notify_context = context;
    /* Written last so the event thread never sees a callback with a stale context */
    gap_central.notify_callback = callback;
}

bool gap_central_subscribe(uint16_t cccd_handle, bool enable, uint32_t timeout_ms) {
    uint16_t value = enable ? GAP_CENTRAL_CCCD_NOTIFY : GAP_CENTRAL_CCCD_DISABLE;
    /* CCCD is little-endian on the wire */
    uint8_t payload[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};

    if(!gap_central.connected) {
        FURI_LOG_E(TAG, "Not connected");
        return false;
    }

    furi_event_flag_clear(gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE);
    tBleStatus status =
        aci_gatt_write_char_desc(gap_central.conn_handle, cccd_handle, sizeof(payload), payload);
    if(status) {
        FURI_LOG_E(TAG, "Failed to write CCCD: 0x%02X", status);
        gap_central.last_error = status;
        return false;
    }

    uint32_t flags = furi_event_flag_wait(
        gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE, FuriFlagWaitAny, timeout_ms);
    if((flags & GAP_CENTRAL_FLAG_PROC_COMPLETE) == 0) {
        FURI_LOG_E(TAG, "CCCD write timed out");
        return false;
    }
    return gap_central.last_error == 0;
}

bool gap_central_exchange_mtu(uint32_t timeout_ms) {
    if(!gap_central.connected) return false;

    furi_event_flag_clear(
        gap_central.flags, GAP_CENTRAL_FLAG_MTU_UPDATED | GAP_CENTRAL_FLAG_PROC_COMPLETE);
    tBleStatus status = aci_gatt_exchange_config(gap_central.conn_handle);
    if(status) {
        FURI_LOG_E(TAG, "Failed to exchange MTU: 0x%02X", status);
        gap_central.last_error = status;
        return false;
    }

    uint32_t flags = furi_event_flag_wait(
        gap_central.flags, GAP_CENTRAL_FLAG_PROC_COMPLETE, FuriFlagWaitAny, timeout_ms);
    if((flags & GAP_CENTRAL_FLAG_PROC_COMPLETE) == 0) {
        FURI_LOG_W(TAG, "MTU exchange timed out, staying at %u", gap_central.mtu);
        return false;
    }

    FURI_LOG_I(TAG, "MTU is now %u", gap_central.mtu);
    return true;
}

uint16_t gap_central_get_mtu(void) {
    return gap_central.mtu ? gap_central.mtu : GAP_CENTRAL_DEFAULT_MTU;
}

uint8_t gap_central_get_last_error(void) {
    return gap_central.last_error;
}
