#include "analyzer_band.h"

#include <string.h>

#define ANALYZER_BAND_FINE_SPAN_HZ           \
    ((uint32_t)(ANALYZER_SURVEY_BINS - 1u) * \
     (ANALYZER_SURVEY_LSB_MHZ * ANALYZER_SURVEY_STRIDE / 1000u))

typedef struct {
    uint32_t span_hz;
    uint32_t pan_hz;
    uint32_t filter_hz;
    const char* name;
} AnalyzerBandLevel;

static const AnalyzerBandLevel analyzer_band_levels[AnalyzerZoomCount] = {
    {ANALYZER_BAND_FINE_SPAN_HZ, 10000u, 58036u, "FINE"},
    {400000u, 50000u, 58036u, "400k"},
    {2000000u, 200000u, 81250u, "2M"},
    {8000000u, 1000000u, 232143u, "8M"},
    {32000000u, 5000000u, 812500u, "32M"},
};

static const AnalyzerBandLevel* analyzer_band_level(AnalyzerZoom zoom) {
    if(zoom >= AnalyzerZoomCount) zoom = AnalyzerZoomFine;
    return &analyzer_band_levels[zoom];
}

bool analyzer_band_is_fine(AnalyzerZoom zoom) {
    return zoom == AnalyzerZoomFine;
}

uint32_t analyzer_band_span_hz(AnalyzerZoom zoom) {
    return analyzer_band_level(zoom)->span_hz;
}

uint32_t analyzer_band_bin_step_hz(AnalyzerZoom zoom) {
    return analyzer_band_level(zoom)->span_hz / (ANALYZER_BAND_BINS - 1u);
}

uint32_t analyzer_band_pan_step_hz(AnalyzerZoom zoom) {
    return analyzer_band_level(zoom)->pan_hz;
}

uint32_t analyzer_band_filter_hz(AnalyzerZoom zoom) {
    return analyzer_band_level(zoom)->filter_hz;
}

const char* analyzer_band_name(AnalyzerZoom zoom) {
    return analyzer_band_level(zoom)->name;
}

uint32_t analyzer_band_bin_frequency(AnalyzerZoom zoom, uint32_t centre_hz, uint16_t bin) {
    if(bin >= ANALYZER_BAND_BINS) bin = ANALYZER_BAND_BINS - 1u;

    const uint32_t step = analyzer_band_bin_step_hz(zoom);
    const int32_t offset = ((int32_t)bin - (int32_t)(ANALYZER_BAND_BINS / 2u)) * (int32_t)step;
    const int64_t frequency = (int64_t)centre_hz + offset;

    if(frequency < 0) return 0;
    return (uint32_t)frequency;
}

void analyzer_band_reset(AnalyzerBand* band, uint32_t centre_hz, AnalyzerZoom zoom) {
    memset(band, 0, sizeof(*band));
    band->centre_hz = centre_hz;
    band->zoom = zoom;
    band->floor_raw = INT8_MAX;
    for(uint16_t i = 0; i < ANALYZER_BAND_BINS; i++) {
        band->rssi_raw[i] = INT8_MIN;
    }
}

void analyzer_band_accumulate(AnalyzerBand* band, uint16_t bin, int8_t rssi_raw) {
    if(bin >= ANALYZER_BAND_BINS) return;
    if(!band->seen[bin] || rssi_raw > band->rssi_raw[bin]) {
        band->rssi_raw[bin] = rssi_raw;
    }
    band->seen[bin] = true;
}

void analyzer_band_finish(AnalyzerBand* band) {
    int8_t peak = INT8_MIN;
    int8_t quietest = INT8_MAX;
    uint16_t peak_bin = 0;
    bool any = false;

    for(uint16_t i = 0; i < ANALYZER_BAND_BINS; i++) {
        if(!band->seen[i]) continue;
        any = true;
        if(band->rssi_raw[i] > peak) {
            peak = band->rssi_raw[i];
            peak_bin = i;
        }
        if(band->rssi_raw[i] < quietest) quietest = band->rssi_raw[i];
    }

    if(!any) return;

    band->peak_bin = peak_bin;

    if(band->floor_raw == INT8_MAX) {
        band->floor_raw = quietest;
    } else if(quietest > band->floor_raw) {
        band->floor_raw++;
    } else if(quietest < band->floor_raw) {
        band->floor_raw--;
    }

    band->valid = true;
}
