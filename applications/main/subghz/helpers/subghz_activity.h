#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SUBGHZ_ACTIVITY_COLUMNS   128
#define SUBGHZ_ACTIVITY_BUCKET_MS 100u
#define SUBGHZ_ACTIVITY_WINDOW_MS 2000u
#define SUBGHZ_ACTIVITY_SAMPLES   200

typedef enum {
    SubGhzActivityValid = 1 << 0,
    SubGhzActivityBackground = 1 << 1,
    SubGhzActivityDecoded = 1 << 2,
    SubGhzActivityBreak = 1 << 3,
} SubGhzActivityFlags;

typedef struct {
    uint32_t at_ms;
    // Changes on every tuning transition; source identifies the radio lane.
    uint16_t context;
    int16_t peak; // Half-dBm, before display clipping.
    int16_t background;
    uint8_t flags;
    uint8_t source;
} SubGhzActivityBucket;

typedef struct {
    SubGhzActivityBucket buckets[SUBGHZ_ACTIVITY_COLUMNS];
    uint8_t next;
} SubGhzActivityHistory;

typedef struct {
    SubGhzActivityHistory history;
    SubGhzActivityBucket pending;
    struct {
        uint16_t at_ms; // Low clock bits; samples expire after two seconds.
        int16_t level;
    } samples[SUBGHZ_ACTIVITY_SAMPLES];
    uint16_t sample_next;
    uint16_t sample_count;
    uint32_t context_since;
    uint32_t last_sample_at;
    uint16_t context;
    uint8_t source;
    bool hopping;
    bool break_pending;
} SubGhzActivityStream;

// Pure aggregation, caller supplies a monotonic millisecond clock and synchronization.
void subghz_activity_reset(SubGhzActivityStream* stream, uint32_t now, uint8_t source);
void subghz_activity_advance(SubGhzActivityStream* stream, uint32_t now);
void subghz_activity_tune(SubGhzActivityStream* stream, uint32_t now, bool hopping);
void subghz_activity_sample(SubGhzActivityStream* stream, uint32_t now, float dbm);
void subghz_activity_decode(SubGhzActivityStream* stream, uint32_t now);
