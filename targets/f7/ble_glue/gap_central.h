#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UUIDs are either 16-bit or 128-bit, kept in the raw little-endian ATT form */
#define GAP_CENTRAL_UUID_MAX 16

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    uint8_t uuid_len; /* 2 or 16 */
    uint8_t uuid[GAP_CENTRAL_UUID_MAX];
} GapCentralService;

typedef struct {
    uint16_t value_handle;
    uint8_t properties; /* GATT characteristic properties bitfield */
    uint8_t uuid_len; /* 2 or 16 */
    uint8_t uuid[GAP_CENTRAL_UUID_MAX];
} GapCentralCharacteristic;

/** Open an outgoing connection as GAP central.
 *
 * Requires the "ble_full" radio stack and a GAP initialized with the central
 * role - see gap_init_svc() in gap.c. Blocks until connected or timed out.
 *
 * @param   address         peer address, least significant byte first
 * @param   address_type    0 public, 1 random
 * @param   timeout_ms      how long to wait for the connection to come up
 * @return  true if connected
 */
bool gap_central_connect(const uint8_t* address, uint8_t address_type, uint32_t timeout_ms);

/** Close the outgoing connection. Safe to call when not connected. */
void gap_central_disconnect(void);

/** Whether an outgoing central connection is currently up. */
bool gap_central_is_connected(void);

/** Discover all primary services on the connected peer.
 * @return  number of services written to out, or -1 on failure
 */
int32_t
    gap_central_discover_services(GapCentralService* out, size_t max_count, uint32_t timeout_ms);

/** Discover all characteristics within a service's handle range.
 * @return  number of characteristics written to out, or -1 on failure
 */
int32_t gap_central_discover_characteristics(
    const GapCentralService* service,
    GapCentralCharacteristic* out,
    size_t max_count,
    uint32_t timeout_ms);

typedef struct {
    uint16_t handle;
    uint8_t uuid_len; /* 2 or 16 */
    uint8_t uuid[GAP_CENTRAL_UUID_MAX];
} GapCentralDescriptor;

/** Discover the descriptors of one characteristic, e.g. to find its CCCD.
 * @param   value_handle    characteristic value handle to start from
 * @param   end_handle      end of the owning service's handle range
 * @return  number of descriptors written to out, or -1 on failure
 */
int32_t gap_central_discover_descriptors(
    uint16_t value_handle,
    uint16_t end_handle,
    GapCentralDescriptor* out,
    size_t max_count,
    uint32_t timeout_ms);

/** Read a characteristic or descriptor value.
 * @return  number of bytes written to out, or -1 on failure
 */
int32_t
    gap_central_read(uint16_t value_handle, uint8_t* out, size_t max_length, uint32_t timeout_ms);

/** Write a characteristic value.
 * @param   with_response   true for a Write Request (waits for the ack), false
 *                          for a Write Command, which returns as soon as it is queued
 */
bool gap_central_write(
    uint16_t value_handle,
    const uint8_t* data,
    uint8_t length,
    bool with_response,
    uint32_t timeout_ms);

/** Called from the BLE event thread for every notification and indication.
 * Must not block - hand the payload to your own thread.
 */
typedef void (*GapCentralNotifyCallback)(
    uint16_t value_handle,
    const uint8_t* data,
    uint8_t length,
    void* context);

/** Install the notification callback. Pass NULL to remove it. */
void gap_central_set_notify_callback(GapCentralNotifyCallback callback, void* context);

/** Enable or disable notifications by writing the characteristic's CCCD. */
bool gap_central_subscribe(uint16_t cccd_handle, bool enable, uint32_t timeout_ms);

/** Negotiate a larger ATT MTU. Without this the payload is limited to 20 bytes. */
bool gap_central_exchange_mtu(uint32_t timeout_ms);

/** Negotiated ATT MTU, or 23 (the default) if never exchanged. */
uint16_t gap_central_get_mtu(void);

/** HCI/ATT status from the last failed operation, 0 if it succeeded. */
uint8_t gap_central_get_last_error(void);

#ifdef __cplusplus
}
#endif
