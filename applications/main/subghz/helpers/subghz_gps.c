#include "subghz_gps.h"
#include "minmea.h"

#include <gps/gps.h>

#include <math.h>
#include <string.h>

#define TAG "SubGhzGps"

// LPUART header, pins 15 (TX) / 16 (RX) - flipperzero-gps-lpuart wiring
#define SUBGHZ_GPS_SERIAL_ID FuriHalSerialIdLpuart

// Line assembly buffer; a NMEA sentence is at most 82 bytes
#define SUBGHZ_GPS_LINE_MAX           256
#define SUBGHZ_GPS_STREAM_SIZE        256
#define SUBGHZ_GPS_PROBE_PERIOD       900 // ms between auto-baud probe ticks
#define SUBGHZ_GPS_DEFAULT_BAUD       9600
// Hold default baud ~36s (typical cold-start TTFF) before trying others
#define SUBGHZ_GPS_DEFAULT_BAUD_TICKS 40
#define SUBGHZ_GPS_FIX_TIMEOUT        5000 // ms after which a fix is considered stale
// ms without any NMEA sentence before the module is treated as unplugged
#define SUBGHZ_GPS_STALE_TIMEOUT      5000
#define SUBGHZ_GPS_ACQUIRE_RETRIES    3
#define SUBGHZ_GPS_ACQUIRE_RETRY_MS   50

#define SUBGHZ_GPS_COMPANION_HZ          1
#define SUBGHZ_GPS_COMPANION_TIMEOUT     10000
#define SUBGHZ_GPS_COMPANION_RETRY_TICKS 4

static const uint32_t subghz_gps_bauds[] = {9600, 19200, 38400, 57600, 115200, 4800};
#define SUBGHZ_GPS_BAUD_COUNT (sizeof(subghz_gps_bauds) / sizeof(subghz_gps_bauds[0]))

typedef enum {
    SubGhzGpsStateProbing, // cycling baud rates, no module confirmed yet
    SubGhzGpsStatePresent, // locked onto a module's baud rate
    SubGhzGpsStateAbsent, // nothing plugged in, UART released
} SubGhzGpsState;

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRxDone = (1 << 1),
    WorkerEvtProbe = (1 << 2),
} WorkerEvtFlags;

#define WORKER_ALL_EVENTS (WorkerEvtStop | WorkerEvtRxDone | WorkerEvtProbe)

struct SubGhzGps {
    FuriThread* thread;
    FuriTimer* probe_timer;
    FuriStreamBuffer* rx_stream;
    FuriHalSerialHandle* serial_handle;
    FuriMutex* mutex;

    // Probe state - owned by the worker thread, no locking needed
    SubGhzGpsState state;
    size_t baud_idx;
    uint32_t bytes_since_probe;
    uint32_t nmea_since_probe;
    uint8_t silent_sweeps;
    uint8_t ticks_on_default_baud;

    Gps* companion;
    uint8_t companion_retry;

    // Shared snapshot - guarded by mutex
    bool present;
    uint32_t locked_baud;
    float latitude;
    float longitude;
    uint8_t satellites;
    uint8_t fix_hour;
    uint8_t fix_minute;
    uint8_t fix_second;
    uint32_t fix_tick; // furi tick of the last valid fix
    uint32_t last_rx_tick; // furi tick of the last parsed sentence (any type)
    bool companion_enabled;
    bool companion_valid;
    float companion_latitude;
    float companion_longitude;
    uint32_t companion_tick;
    GpsStatus companion_status;
    bool companion_status_seen;
    bool companion_link_up;
    bool rx_session;
    bool session_valid;
    bool session_frozen;
    float session_latitude;
    float session_longitude;
};

static bool subghz_gps_module_fix_locked(SubGhzGps* gps) {
    return gps->present && !isnan(gps->latitude) && !isnan(gps->longitude) &&
           (furi_get_tick() - gps->fix_tick) < furi_ms_to_ticks(SUBGHZ_GPS_FIX_TIMEOUT);
}

static bool subghz_gps_companion_fix_locked(SubGhzGps* gps) {
    return gps->companion_enabled && gps->companion_valid && !isnan(gps->companion_latitude) &&
           !isnan(gps->companion_longitude) &&
           (furi_get_tick() - gps->companion_tick) <
               furi_ms_to_ticks(SUBGHZ_GPS_COMPANION_TIMEOUT);
}

// Parse one NMEA line, updating the shared snapshot. Returns true when a known
// sentence was parsed (used as the baud-lock signal).
static bool subghz_gps_parse_nmea(SubGhzGps* gps, char* line) {
    bool parsed = false;

    switch(minmea_sentence_id(line, false)) {
    case MINMEA_SENTENCE_RMC: {
        struct minmea_sentence_rmc frame;
        if(minmea_parse_rmc(&frame, line)) {
            parsed = true;
            furi_mutex_acquire(gps->mutex, FuriWaitForever);
            if(frame.valid) {
                gps->latitude = minmea_tocoord(&frame.latitude);
                gps->longitude = minmea_tocoord(&frame.longitude);
                gps->fix_tick = furi_get_tick();
            }
            gps->fix_hour = frame.time.hours;
            gps->fix_minute = frame.time.minutes;
            gps->fix_second = frame.time.seconds;
            gps->present = true;
            gps->last_rx_tick = furi_get_tick();
            furi_mutex_release(gps->mutex);
        }
    } break;

    case MINMEA_SENTENCE_GGA: {
        struct minmea_sentence_gga frame;
        if(minmea_parse_gga(&frame, line)) {
            parsed = true;
            furi_mutex_acquire(gps->mutex, FuriWaitForever);
            gps->satellites = frame.satellites_tracked;
            if(frame.fix_quality > 0) {
                gps->latitude = minmea_tocoord(&frame.latitude);
                gps->longitude = minmea_tocoord(&frame.longitude);
                gps->fix_tick = furi_get_tick();
            }
            gps->fix_hour = frame.time.hours;
            gps->fix_minute = frame.time.minutes;
            gps->fix_second = frame.time.seconds;
            gps->present = true;
            gps->last_rx_tick = furi_get_tick();
            furi_mutex_release(gps->mutex);
        }
    } break;

    case MINMEA_SENTENCE_GLL: {
        struct minmea_sentence_gll frame;
        if(minmea_parse_gll(&frame, line)) {
            parsed = true;
            furi_mutex_acquire(gps->mutex, FuriWaitForever);
            if(frame.status == MINMEA_GLL_STATUS_DATA_VALID) {
                gps->latitude = minmea_tocoord(&frame.latitude);
                gps->longitude = minmea_tocoord(&frame.longitude);
                gps->fix_tick = furi_get_tick();
            }
            gps->fix_hour = frame.time.hours;
            gps->fix_minute = frame.time.minutes;
            gps->fix_second = frame.time.seconds;
            gps->present = true;
            gps->last_rx_tick = furi_get_tick();
            furi_mutex_release(gps->mutex);
        }
    } break;

    default:
        break;
    }

    return parsed;
}

static void subghz_gps_uart_on_irq_cb(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    SubGhzGps* gps = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(gps->rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(gps->thread), WorkerEvtRxDone);
    }
}

// Release the UART; expansion is re-enabled only from subghz_gps_free().
static void subghz_gps_release_serial(SubGhzGps* gps) {
    if(!gps->serial_handle) return;
    furi_hal_serial_async_rx_stop(gps->serial_handle);
    furi_hal_serial_deinit(gps->serial_handle);
    furi_hal_serial_control_release(gps->serial_handle);
    gps->serial_handle = NULL;
}

static bool subghz_gps_acquire_serial(SubGhzGps* gps) {
    for(size_t attempt = 0; attempt < SUBGHZ_GPS_ACQUIRE_RETRIES; attempt++) {
        gps->serial_handle = furi_hal_serial_control_acquire(SUBGHZ_GPS_SERIAL_ID);
        if(gps->serial_handle) return true;
        furi_delay_ms(SUBGHZ_GPS_ACQUIRE_RETRY_MS);
    }
    return false;
}

static void
    subghz_gps_on_companion_location(GpsStatus status, const GpsLocation* location, void* context) {
    SubGhzGps* gps = context;

    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool changed = !gps->companion_status_seen || gps->companion_status != status;
    gps->companion_status = status;
    gps->companion_status_seen = true;

    if(status != GpsStatusOk || !location) {
        gps->companion_valid = false;
        gps->companion_latitude = NAN;
        gps->companion_longitude = NAN;
    } else {
        gps->companion_latitude = (float)location->latitude / 1e7f;
        gps->companion_longitude = (float)location->longitude / 1e7f;
        gps->companion_valid = true;
        gps->companion_tick = furi_get_tick();
        if(gps->rx_session) {
            gps->session_latitude = gps->companion_latitude;
            gps->session_longitude = gps->companion_longitude;
            gps->session_valid = true;
        }
    }
    furi_mutex_release(gps->mutex);

    if(!changed) return;
    switch(status) {
    case GpsStatusOk:
        FURI_LOG_I(TAG, "Companion GPS streaming");
        break;
    case GpsStatusNotSupported:
        FURI_LOG_I(TAG, "Companion has no location capability");
        break;
    case GpsStatusNoPermission:
        FURI_LOG_W(TAG, "Companion denied location permission");
        break;
    case GpsStatusDisabled:
        FURI_LOG_W(TAG, "Companion location services are off");
        break;
    default:
        FURI_LOG_W(TAG, "Companion location error");
        break;
    }
}

static void subghz_gps_companion_tick(SubGhzGps* gps) {
    if(!gps->companion) return;

    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool enabled = gps->companion_enabled;
    bool fresh = subghz_gps_companion_fix_locked(gps);
    if(!fresh && gps->companion_valid) {
        gps->companion_valid = false;
        gps->companion_latitude = NAN;
        gps->companion_longitude = NAN;
    }
    furi_mutex_release(gps->mutex);

    if(!enabled || fresh) {
        gps->companion_retry = 0;
        return;
    }
    if(gps->companion_retry > 0) {
        gps->companion_retry--;
        return;
    }
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    enabled = gps->companion_enabled;
    furi_mutex_release(gps->mutex);
    if(!enabled) return;

    gps->companion_retry = SUBGHZ_GPS_COMPANION_RETRY_TICKS;
    gps_request_stream(gps->companion, SUBGHZ_GPS_COMPANION_HZ);
}

// Runs in the worker thread on each probe tick: advance/lock/abandon the search.
static void subghz_gps_probe_tick(SubGhzGps* gps) {
    subghz_gps_companion_tick(gps);

    if(gps->state != SubGhzGpsStateProbing) return;

    if(gps->nmea_since_probe > 0) {
        // Well-formed NMEA at the current baud - lock on.
        furi_mutex_acquire(gps->mutex, FuriWaitForever);
        gps->state = SubGhzGpsStatePresent;
        gps->present = true;
        gps->locked_baud = subghz_gps_bauds[gps->baud_idx];
        furi_mutex_release(gps->mutex);
        FURI_LOG_I(TAG, "GPS locked at %lu baud on LPUART", subghz_gps_bauds[gps->baud_idx]);
        return;
    }

    if(gps->baud_idx == 0 && gps->ticks_on_default_baud < SUBGHZ_GPS_DEFAULT_BAUD_TICKS) {
        gps->ticks_on_default_baud++;
        gps->bytes_since_probe = 0;
        gps->nmea_since_probe = 0;
        return;
    }
    gps->ticks_on_default_baud = 0;

    bool had_bytes = gps->bytes_since_probe > 0;
    gps->bytes_since_probe = 0;
    gps->nmea_since_probe = 0;

    // Advance to the next baud rate.
    gps->baud_idx++;
    if(gps->baud_idx >= SUBGHZ_GPS_BAUD_COUNT) {
        gps->baud_idx = 0;
        if(!had_bytes) {
            gps->silent_sweeps++;
        } else {
            gps->silent_sweeps = 0;
        }
    }

    if(gps->silent_sweeps >= 2) {
        FURI_LOG_D(TAG, "No NMEA yet, keep probing");
        gps->silent_sweeps = 0;
    }

    furi_hal_serial_set_br(gps->serial_handle, subghz_gps_bauds[gps->baud_idx]);
}

static int32_t subghz_gps_worker(void* context) {
    SubGhzGps* gps = context;
    char line[SUBGHZ_GPS_LINE_MAX];
    size_t line_len = 0;
    uint8_t buf[64];

    while(true) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_ALL_EVENTS, FuriFlagWaitAny, FuriWaitForever);
        if(events & FuriFlagError) continue;

        if(events & WorkerEvtStop) break;

        if(events & WorkerEvtRxDone) {
            size_t n;
            while((n = furi_stream_buffer_receive(gps->rx_stream, buf, sizeof(buf), 0)) > 0) {
                gps->bytes_since_probe += n;
                for(size_t i = 0; i < n; i++) {
                    char c = (char)buf[i];
                    if(c == '\r') continue;
                    if(c == '\n') {
                        line[line_len] = '\0';
                        if(line_len > 0 && subghz_gps_parse_nmea(gps, line)) {
                            gps->nmea_since_probe++;
                        }
                        line_len = 0;
                    } else if(line_len < SUBGHZ_GPS_LINE_MAX - 1) {
                        line[line_len++] = c;
                    } else {
                        line_len = 0; // overflow: drop the garbled line
                    }
                }
            }
        }

        if(events & WorkerEvtProbe) {
            subghz_gps_probe_tick(gps);
        }
    }

    return 0;
}

static void subghz_gps_probe_timer_cb(void* context) {
    SubGhzGps* gps = context;
    furi_thread_flags_set(furi_thread_get_id(gps->thread), WorkerEvtProbe);
}

SubGhzGps* subghz_gps_alloc(void) {
    SubGhzGps* gps = malloc(sizeof(SubGhzGps));
    memset(gps, 0, sizeof(SubGhzGps));
    gps->latitude = NAN;
    gps->longitude = NAN;
    gps->companion_latitude = NAN;
    gps->companion_longitude = NAN;
    gps->session_latitude = NAN;
    gps->session_longitude = NAN;
    gps->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    gps->companion = furi_record_open(RECORD_GPS);
    gps_set_location_callback(gps->companion, subghz_gps_on_companion_location, gps);

    gps->rx_stream = furi_stream_buffer_alloc(SUBGHZ_GPS_STREAM_SIZE, 1);

    gps->thread = furi_thread_alloc_ex("SubGhzGpsWorker", 2048, subghz_gps_worker, gps);
    furi_thread_start(gps->thread);

    if(subghz_gps_acquire_serial(gps)) {
        gps->state = SubGhzGpsStateProbing;
        gps->baud_idx = 0;
        furi_hal_serial_init(gps->serial_handle, SUBGHZ_GPS_DEFAULT_BAUD);
        furi_hal_serial_async_rx_start(gps->serial_handle, subghz_gps_uart_on_irq_cb, gps, true);
    } else {
        FURI_LOG_I(TAG, "No UART for GPS, companion source only");
        gps->state = SubGhzGpsStateAbsent;
    }

    gps->probe_timer = furi_timer_alloc(subghz_gps_probe_timer_cb, FuriTimerTypePeriodic, gps);
    furi_timer_start(gps->probe_timer, furi_ms_to_ticks(SUBGHZ_GPS_PROBE_PERIOD));

    return gps;
}

void subghz_gps_free(SubGhzGps* gps) {
    if(!gps) return;

    if(gps->probe_timer) {
        furi_timer_stop(gps->probe_timer);
        furi_timer_free(gps->probe_timer);
    }

    if(gps->thread) {
        furi_thread_flags_set(furi_thread_get_id(gps->thread), WorkerEvtStop);
        furi_thread_join(gps->thread);
        furi_thread_free(gps->thread);
    }

    if(gps->companion) {
        gps_stop_stream(gps->companion);
        gps_set_location_callback(gps->companion, NULL, NULL);
        furi_record_close(RECORD_GPS);
        gps->companion = NULL;
    }

    subghz_gps_release_serial(gps);

    if(gps->rx_stream) furi_stream_buffer_free(gps->rx_stream);
    furi_mutex_free(gps->mutex);
    free(gps);
}

bool subghz_gps_is_active(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool active = gps->serial_handle != NULL || subghz_gps_companion_fix_locked(gps);
    furi_mutex_release(gps->mutex);
    return active;
}

bool subghz_gps_is_present(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool present = gps->present || subghz_gps_companion_fix_locked(gps);
    furi_mutex_release(gps->mutex);
    return present;
}

bool subghz_gps_has_fix(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool fix = subghz_gps_module_fix_locked(gps) || subghz_gps_companion_fix_locked(gps);
    furi_mutex_release(gps->mutex);
    return fix;
}

static bool subghz_gps_read_live_location(SubGhzGps* gps, float* latitude, float* longitude) {
    if(subghz_gps_module_fix_locked(gps)) {
        if(latitude) *latitude = gps->latitude;
        if(longitude) *longitude = gps->longitude;
        return true;
    }
    if(subghz_gps_companion_fix_locked(gps)) {
        if(latitude) *latitude = gps->companion_latitude;
        if(longitude) *longitude = gps->companion_longitude;
        return true;
    }
    return false;
}

static bool subghz_gps_read_session_hold(SubGhzGps* gps, float* latitude, float* longitude) {
    if(!gps->rx_session || !gps->session_frozen || !gps->session_valid) return false;
    if(isnan(gps->session_latitude) || isnan(gps->session_longitude)) return false;
    if(latitude) *latitude = gps->session_latitude;
    if(longitude) *longitude = gps->session_longitude;
    return true;
}

static void subghz_gps_clear_companion_live(SubGhzGps* gps) {
    gps->companion_valid = false;
    gps->companion_latitude = NAN;
    gps->companion_longitude = NAN;
    gps->companion_status_seen = false;
}

static void subghz_gps_clear_session_hold(SubGhzGps* gps) {
    gps->session_frozen = false;
    gps->session_valid = false;
    gps->session_latitude = NAN;
    gps->session_longitude = NAN;
}

bool subghz_gps_get_location(SubGhzGps* gps, float* latitude, float* longitude) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool fix = subghz_gps_read_live_location(gps, latitude, longitude);
    furi_mutex_release(gps->mutex);
    return fix;
}

bool subghz_gps_get_geotag(SubGhzGps* gps, float* latitude, float* longitude) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool fix = subghz_gps_read_live_location(gps, latitude, longitude);
    if(!fix) fix = subghz_gps_read_session_hold(gps, latitude, longitude);
    furi_mutex_release(gps->mutex);
    return fix;
}

SubGhzGpsSource subghz_gps_get_source(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    SubGhzGpsSource source = SubGhzGpsSourceNone;
    if(subghz_gps_module_fix_locked(gps)) {
        source = SubGhzGpsSourceModule;
    } else if(subghz_gps_companion_fix_locked(gps)) {
        source = SubGhzGpsSourceCompanion;
    }
    furi_mutex_release(gps->mutex);
    return source;
}

bool subghz_gps_companion_available(SubGhzGps* gps) {
    furi_check(gps);
    return gps->companion != NULL && gps_is_available(gps->companion);
}

bool subghz_gps_get_companion_enabled(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool enabled = gps->companion_enabled;
    furi_mutex_release(gps->mutex);
    return enabled;
}

void subghz_gps_set_companion_enabled(SubGhzGps* gps, bool enabled) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool changed = gps->companion_enabled != enabled;
    gps->companion_enabled = enabled;
    if(!enabled) {
        subghz_gps_clear_companion_live(gps);
        subghz_gps_clear_session_hold(gps);
        gps->companion_link_up = false;
    } else {
        gps->companion_link_up = false;
    }
    furi_mutex_release(gps->mutex);

    if(!changed || !gps->companion) return;
    if(enabled) {
        gps->companion_retry = 0;
        gps_request_stream(gps->companion, SUBGHZ_GPS_COMPANION_HZ);
    } else {
        gps_stop_stream(gps->companion);
    }
}

void subghz_gps_set_rx_session(SubGhzGps* gps, bool active) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    gps->rx_session = active;
    furi_mutex_release(gps->mutex);
}

void subghz_gps_reset_rx_session(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    gps->rx_session = false;
    subghz_gps_clear_session_hold(gps);
    furi_mutex_release(gps->mutex);
}

void subghz_gps_rx_session_note_fix(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    if(gps->rx_session && subghz_gps_companion_fix_locked(gps)) {
        gps->session_latitude = gps->companion_latitude;
        gps->session_longitude = gps->companion_longitude;
        gps->session_valid = true;
    }
    furi_mutex_release(gps->mutex);
}

bool subghz_gps_poll_companion_link(SubGhzGps* gps) {
    furi_check(gps);
    bool available = subghz_gps_companion_available(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    bool lost = false;
    if(available) {
        gps->companion_link_up = true;
    } else if(gps->companion_link_up) {
        gps->companion_link_up = false;
        lost = true;
    }
    furi_mutex_release(gps->mutex);
    return lost;
}

void subghz_gps_companion_disconnected(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    if(gps->rx_session && gps->session_valid && !isnan(gps->session_latitude) &&
       !isnan(gps->session_longitude)) {
        gps->session_frozen = true;
    }
    gps->companion_enabled = false;
    subghz_gps_clear_companion_live(gps);
    gps->companion_link_up = false;
    furi_mutex_release(gps->mutex);

    if(gps->companion) gps_stop_stream(gps->companion);
}

uint8_t subghz_gps_get_satellites(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    uint8_t sats = gps->satellites;
    furi_mutex_release(gps->mutex);
    return sats;
}

uint32_t subghz_gps_get_baudrate(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    uint32_t baud = gps->locked_baud;
    furi_mutex_release(gps->mutex);
    return baud;
}

void subghz_gps_get_fix_time(SubGhzGps* gps, uint8_t* hour, uint8_t* minute, uint8_t* second) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    if(hour) *hour = gps->fix_hour;
    if(minute) *minute = gps->fix_minute;
    if(second) *second = gps->fix_second;
    furi_mutex_release(gps->mutex);
}

void subghz_gps_tick_stale(SubGhzGps* gps) {
    furi_check(gps);
    furi_mutex_acquire(gps->mutex, FuriWaitForever);
    if(gps->present &&
       (furi_get_tick() - gps->last_rx_tick) > furi_ms_to_ticks(SUBGHZ_GPS_STALE_TIMEOUT)) {
        gps->present = false;
        gps->latitude = NAN;
        gps->longitude = NAN;
        gps->satellites = 0;
        gps->fix_hour = 0;
        gps->fix_minute = 0;
        gps->fix_second = 0;
    }
    furi_mutex_release(gps->mutex);
}

static float subghz_gps_deg2rad(float deg) {
    return deg * (float)M_PI / 180.0f;
}

float subghz_gps_distance_km(float lat1, float lon1, float lat2, float lon2) {
    float lat1r = subghz_gps_deg2rad(lat1);
    float lon1r = subghz_gps_deg2rad(lon1);
    float lat2r = subghz_gps_deg2rad(lat2);
    float lon2r = subghz_gps_deg2rad(lon2);
    float u = sinf((lat2r - lat1r) / 2.0f);
    float v = sinf((lon2r - lon1r) / 2.0f);
    return 2.0f * 6371.0f * asinf(sqrtf(u * u + cosf(lat1r) * cosf(lat2r) * v * v));
}

const char* subghz_gps_direction(float lat1, float lon1, float lat2, float lon2) {
    float angle = atan2f(lat1 - lat2, lon1 - lon2) * 180.0f / (float)M_PI;
    if(angle > -22.5f && angle <= 22.5f) return "E";
    if(angle > 22.5f && angle <= 67.5f) return "NE";
    if(angle > 67.5f && angle <= 112.5f) return "N";
    if(angle > 112.5f && angle <= 157.5f) return "NW";
    if(angle < -22.5f && angle >= -67.5f) return "SE";
    if(angle < -67.5f && angle >= -112.5f) return "S";
    if(angle < -112.5f && angle >= -157.5f) return "SW";
    return "W";
}
