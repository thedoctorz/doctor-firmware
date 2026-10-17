#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAP_SCAN_NAME_MAX 24

typedef struct {
    /* As received over the air, least significant byte first */
    uint8_t address[6];
    uint8_t address_type;
    uint8_t event_type;
    int8_t rssi;
    /* Local name from the advertisement or scan response, "" if not advertised.
     * Sanitized to printable ASCII - this is remote-controlled data. */
    char name[GAP_SCAN_NAME_MAX];
} GapScanReport;

/** Called once per advertising report, on the BLE event thread.
 * Must not block: hand the report off to your own thread.
 */
typedef void (*GapScanCallback)(const GapScanReport* report, void* context);

/** Start scanning for nearby advertisers.
 *
 * Requires the "ble_full" radio stack - the "ble_light" stack is peripheral-only
 * and has no GAP central/observer role, so this fails there.
 *
 * @param   callback    invoked per advertising report
 * @param   context     passed back to the callback, must not be NULL
 * @return  true if scanning started
 */
bool gap_scan_start(GapScanCallback callback, void* context);

/** Stop scanning. Safe to call when not scanning. */
void gap_scan_stop(void);

/** HCI status from the last failed gap_scan_start(), 0 if it succeeded.
 * 0x0C (command disallowed) usually means GAP was initialized without the
 * observer role - see gap_init() in gap.c.
 */
uint8_t gap_scan_get_last_error(void);

/** Whether a scan is currently running. */
bool gap_scan_is_active(void);

#ifdef __cplusplus
}
#endif
