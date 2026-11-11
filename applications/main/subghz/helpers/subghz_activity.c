#include "subghz_activity.h"
#include <math.h>
#include <string.h>

static void subghz_activity_background(SubGhzActivityStream* stream, uint32_t end) {
    if(stream->hopping || (uint32_t)(end - stream->context_since) < 500u) return;
    if((uint32_t)(end - stream->last_sample_at) > SUBGHZ_ACTIVITY_WINDOW_MS) return;
    // One-dB bins for the background only; peaks retain half-dB resolution.
    uint16_t histogram[128] = {0};
    unsigned count = 0;
    for(unsigned i = 0; i < stream->sample_count; ++i) {
        if((uint16_t)(end - stream->samples[i].at_ms) > SUBGHZ_ACTIVITY_WINDOW_MS) continue;
        int bin = (stream->samples[i].level + 256) / 2;
        if(bin < 0) bin = 0;
        if(bin > 127) bin = 127;
        histogram[bin]++;
        count++;
    }
    if(count < 40) return;
    unsigned rank = (count + 4) / 5;
    for(unsigned i = 0; i < 128; ++i) {
        if(rank <= histogram[i]) {
            stream->pending.background = (int16_t)(2 * (int)i - 256);
            stream->pending.flags |= SubGhzActivityBackground;
            return;
        }
        rank -= histogram[i];
    }
}

void subghz_activity_reset(SubGhzActivityStream* stream, uint32_t now, uint8_t source) {
    memset(stream, 0, sizeof(*stream));
    stream->source = source;
    stream->context_since = now;
    stream->pending.at_ms = now;
    stream->pending.source = source;
    stream->break_pending = true;
}

void subghz_activity_advance(SubGhzActivityStream* stream, uint32_t now) {
    // Bound work after a long pause, while preserving wall-clock bucket spacing.
    uint32_t elapsed = now - stream->pending.at_ms;
    if(elapsed / SUBGHZ_ACTIVITY_BUCKET_MS > SUBGHZ_ACTIVITY_COLUMNS) {
        memset(&stream->history, 0, sizeof(stream->history));
        stream->pending.flags = 0;
        stream->pending.at_ms += (elapsed / SUBGHZ_ACTIVITY_BUCKET_MS - SUBGHZ_ACTIVITY_COLUMNS) *
                                 SUBGHZ_ACTIVITY_BUCKET_MS;
        stream->break_pending = true;
    }
    while((uint32_t)(now - stream->pending.at_ms) >= SUBGHZ_ACTIVITY_BUCKET_MS) {
        if(stream->pending.flags & SubGhzActivityValid) {
            subghz_activity_background(stream, stream->pending.at_ms + SUBGHZ_ACTIVITY_BUCKET_MS);
        }
        stream->history.buckets[stream->history.next] = stream->pending;
        stream->history.next = (stream->history.next + 1) % SUBGHZ_ACTIVITY_COLUMNS;
        const uint32_t next = stream->pending.at_ms + SUBGHZ_ACTIVITY_BUCKET_MS;
        stream->pending = (SubGhzActivityBucket){
            .at_ms = next, .context = stream->context, .source = stream->source};
    }
}

void subghz_activity_tune(SubGhzActivityStream* stream, uint32_t now, bool hopping) {
    subghz_activity_advance(stream, now);
    stream->context++;
    stream->context_since = now;
    stream->hopping = hopping;
    stream->sample_count = 0;
    stream->sample_next = 0;
    stream->break_pending = true;
    stream->pending.flags |= SubGhzActivityBreak;
    stream->pending.flags &= ~SubGhzActivityBackground;
}

void subghz_activity_sample(SubGhzActivityStream* stream, uint32_t now, float dbm) {
    subghz_activity_advance(stream, now);
    // -128/-127 are device failure/unavailable sentinels, not a quiet channel.
    if(!isfinite(dbm) || dbm <= -127.0f || dbm > 0.0f) return;
    int16_t level = (int16_t)lroundf(dbm * 2.0f);
    if(!(stream->pending.flags & SubGhzActivityValid) || level > stream->pending.peak) {
        stream->pending.peak = level;
        stream->pending.context = stream->context;
    }
    stream->pending.flags |= SubGhzActivityValid;
    if(stream->break_pending) {
        stream->pending.flags |= SubGhzActivityBreak;
        stream->break_pending = false;
    }
    if(!stream->hopping) {
        if((uint32_t)(now - stream->last_sample_at) > SUBGHZ_ACTIVITY_WINDOW_MS) {
            stream->sample_count = stream->sample_next = 0;
        }
        stream->last_sample_at = now;
        stream->samples[stream->sample_next].at_ms = now;
        stream->samples[stream->sample_next].level = level;
        stream->sample_next = (stream->sample_next + 1) % SUBGHZ_ACTIVITY_SAMPLES;
        if(stream->sample_count < SUBGHZ_ACTIVITY_SAMPLES) stream->sample_count++;
    }
}

void subghz_activity_decode(SubGhzActivityStream* stream, uint32_t now) {
    subghz_activity_advance(stream, now);
    stream->pending.flags |= SubGhzActivityDecoded;
}
