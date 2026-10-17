/**
 * @file subghz_gps.h
 * SubGHz GPS geotagging helper.
 *
 * Reads NMEA sentences from a GPS module attached to the USART header
 * (pins 13/14). The module is auto-detected and the baud rate is
 * auto-negotiated, so no configuration is required. If a Flipper
 * expansion module is already connected the helper backs off and stays
 * inactive so it never fights the expansion service for the UART.
 */

#pragma once

#include <furi.h>
#include <furi_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SubGhzGps SubGhzGps;

typedef enum {
    SubGhzGpsSourceNone,
    SubGhzGpsSourceModule,
    SubGhzGpsSourceCompanion,
} SubGhzGpsSource;

/** Allocate and start the GPS subsystem.
 *
 * Spawns a background worker that probes the USART for NMEA data and locks
 * onto the module's baud rate automatically. Never blocks the caller and
 * never returns NULL; use subghz_gps_is_present() to learn whether a module
 * was actually found.
 *
 * @return SubGhzGps instance
 */
SubGhzGps* subghz_gps_alloc(void);

/** Stop the worker, release the UART and restore the expansion service.
 *
 * @param gps SubGhzGps instance (NULL is ignored)
 */
void subghz_gps_free(SubGhzGps* gps);

/** Whether the GPS worker owns the UART and is probing or locked.
 *
 * @param gps SubGhzGps instance
 * @return true while the USART is held for GPS use
 */
bool subghz_gps_is_active(SubGhzGps* gps);

/** Whether valid NMEA data has been seen (a module is plugged in).
 *
 * @param gps SubGhzGps instance
 * @return true if a GPS module was detected
 */
bool subghz_gps_is_present(SubGhzGps* gps);

/** Whether a recent, non-stale position fix is available.
 *
 * @param gps SubGhzGps instance
 * @return true if the last fix is valid and fresh
 */
bool subghz_gps_has_fix(SubGhzGps* gps);

/** Copy the last known location.
 *
 * @param gps        SubGhzGps instance
 * @param latitude   output latitude in decimal degrees
 * @param longitude  output longitude in decimal degrees
 * @return true if a valid fix was returned
 */
bool subghz_gps_get_location(SubGhzGps* gps, float* latitude, float* longitude);

bool subghz_gps_get_geotag(SubGhzGps* gps, float* latitude, float* longitude);

/** Number of satellites tracked (from the last GGA sentence).
 *
 * @param gps SubGhzGps instance
 * @return satellite count
 */
uint8_t subghz_gps_get_satellites(SubGhzGps* gps);

SubGhzGpsSource subghz_gps_get_source(SubGhzGps* gps);

bool subghz_gps_companion_available(SubGhzGps* gps);

bool subghz_gps_get_companion_enabled(SubGhzGps* gps);

void subghz_gps_set_companion_enabled(SubGhzGps* gps, bool enabled);

void subghz_gps_set_rx_session(SubGhzGps* gps, bool active);

void subghz_gps_reset_rx_session(SubGhzGps* gps);

void subghz_gps_rx_session_note_fix(SubGhzGps* gps);

bool subghz_gps_poll_companion_link(SubGhzGps* gps);

void subghz_gps_companion_disconnected(SubGhzGps* gps);

/** Detected baud rate, 0 while still searching or if no module is present.
 *
 * @param gps SubGhzGps instance
 * @return baud rate in bits per second
 */
uint32_t subghz_gps_get_baudrate(SubGhzGps* gps);

/** Last fix time in UTC as reported by the module.
 *
 * @param gps     SubGhzGps instance
 * @param hour    output hour   (may be NULL)
 * @param minute  output minute (may be NULL)
 * @param second  output second (may be NULL)
 */
void subghz_gps_get_fix_time(SubGhzGps* gps, uint8_t* hour, uint8_t* minute, uint8_t* second);

/** Drop stale fix data when the module stops updating (Momentum-style). */
void subghz_gps_tick_stale(SubGhzGps* gps);

/** Great-circle distance between two coordinates, in kilometres.
 *
 * @return distance in km
 */
float subghz_gps_distance_km(float lat1, float lon1, float lat2, float lon2);

/** Compass direction ("N", "NE", ...) pointing from point 1 towards point 2.
 *
 * @return static direction string
 */
const char* subghz_gps_direction(float lat1, float lon1, float lat2, float lon2);

#ifdef __cplusplus
}
#endif
