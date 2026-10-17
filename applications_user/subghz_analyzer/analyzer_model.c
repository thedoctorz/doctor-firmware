#include "analyzer_model.h"

#include <cc1101_regs.h>

#define ANALYZER_FXOSC_HZ 26000000ULL

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t value;
    uint8_t reg;
} AnalyzerValue;

typedef struct {
    uint32_t baud;
    uint8_t e;
    uint8_t m;
} AnalyzerRate;

static const AnalyzerValue analyzer_bw[ANALYZER_BW_COUNT] = {
    {58036, 0xF},
    {67708, 0xE},
    {81250, 0xD},
    {101562, 0xC},
    {116071, 0xB},
    {135417, 0xA},
    {162500, 0x9},
    {203125, 0x8},
    {232143, 0x7},
    {270833, 0x6},
    {325000, 0x5},
    {406250, 0x4},
    {464286, 0x3},
    {541667, 0x2},
    {650000, 0x1},
    {812500, 0x0},
};

static const AnalyzerValue analyzer_dev[ANALYZER_DEV_COUNT] = {
    {1587, 0x00},   {1785, 0x01},   {1984, 0x02},   {2182, 0x03},   {2380, 0x04},   {2579, 0x05},
    {2777, 0x06},   {2975, 0x07},   {3174, 0x10},   {3571, 0x11},   {3967, 0x12},   {4364, 0x13},
    {4761, 0x14},   {5157, 0x15},   {5554, 0x16},   {5951, 0x17},   {6348, 0x20},   {7141, 0x21},
    {7935, 0x22},   {8728, 0x23},   {9521, 0x24},   {10315, 0x25},  {11108, 0x26},  {11902, 0x27},
    {12695, 0x30},  {14282, 0x31},  {15869, 0x32},  {17456, 0x33},  {19043, 0x34},  {20630, 0x35},
    {22217, 0x36},  {23804, 0x37},  {25391, 0x40},  {28564, 0x41},  {31738, 0x42},  {34912, 0x43},
    {38086, 0x44},  {41260, 0x45},  {44434, 0x46},  {47607, 0x47},  {50781, 0x50},  {57129, 0x51},
    {63477, 0x52},  {69824, 0x53},  {76172, 0x54},  {82520, 0x55},  {88867, 0x56},  {95215, 0x57},
    {101562, 0x60}, {114258, 0x61}, {126953, 0x62}, {139648, 0x63}, {152344, 0x64}, {165039, 0x65},
    {177734, 0x66}, {190430, 0x67}, {203125, 0x70}, {228516, 0x71}, {253906, 0x72}, {279297, 0x73},
    {304688, 0x74}, {330078, 0x75}, {355469, 0x76}, {380859, 0x77},
};

static const AnalyzerRate analyzer_rate[ANALYZER_RATE_COUNT] = {
    {601, 0x4, 0x84},    {668, 0x4, 0xAF},    {742, 0x4, 0xDF},    {824, 0x5, 0x0A},
    {917, 0x5, 0x28},    {1020, 0x5, 0x49},   {1134, 0x5, 0x6E},   {1261, 0x5, 0x97},
    {1401, 0x5, 0xC4},   {1556, 0x5, 0xF6},   {1729, 0x6, 0x17},   {1922, 0x6, 0x36},
    {2139, 0x6, 0x59},   {2374, 0x6, 0x7F},   {2641, 0x6, 0xAA},   {2932, 0x6, 0xD9},
    {3261, 0x7, 0x07},   {3620, 0x7, 0x24},   {4029, 0x7, 0x45},   {4476, 0x7, 0x69},
    {4972, 0x7, 0x91},   {5529, 0x7, 0xBE},   {6149, 0x7, 0xF0},   {6844, 0x8, 0x14},
    {7612, 0x8, 0x33},   {8455, 0x8, 0x55},   {9398, 0x8, 0x7B},   {10439, 0x8, 0xA5},
    {11604, 0x8, 0xD4},  {12894, 0x9, 0x04},  {14332, 0x9, 0x21},  {15919, 0x9, 0x41},
    {17704, 0x9, 0x65},  {19688, 0x9, 0x8D},  {21870, 0x9, 0xB9},  {24300, 0x9, 0xEA},
    {26978, 0xA, 0x10},  {29953, 0xA, 0x2E},  {33325, 0xA, 0x50},  {36995, 0xA, 0x75},
    {41161, 0xA, 0x9F},  {45723, 0xA, 0xCD},  {50781, 0xB, 0x00},  {56534, 0xB, 0x1D},
    {62881, 0xB, 0x3D},  {69824, 0xB, 0x60},  {77560, 0xB, 0x87},  {86288, 0xB, 0xB3},
    {95810, 0xB, 0xE3},  {106720, 0xC, 0x0D}, {118622, 0xC, 0x2B}, {131714, 0xC, 0x4C},
    {146393, 0xC, 0x71}, {162659, 0xC, 0x9A}, {180908, 0xC, 0xC8}, {201141, 0xC, 0xFB},
    {223755, 0xD, 0x1A}, {249146, 0xD, 0x3A}, {276917, 0xD, 0x5D}, {307861, 0xD, 0x84},
    {341980, 0xD, 0xAF}, {380066, 0xD, 0xDF}, {422119, 0xE, 0x0A}, {469727, 0xE, 0x28},
};

typedef struct {
    uint32_t low;
    uint32_t high;
} AnalyzerBand;

static const AnalyzerBand analyzer_bands[] = {
    {281000000, 361000000},
    {378000000, 481000000},
    {749000000, 962000000},
};
#define ANALYZER_BAND_COUNT (sizeof(analyzer_bands) / sizeof(analyzer_bands[0]))

#define ANALYZER_ADC_RETENTION_BW_HZ 325000u

#define ANALYZER_AGC_PIVOT_BW_HZ 100000u
#define ANALYZER_AGC_TARGET_LOW  3
#define ANALYZER_AGC_TARGET_HIGH 7

static uint8_t analyzer_agc_target(const AnalyzerModel* model) {
    if(model->magn_target != ANALYZER_AGC_AUTO) return model->magn_target & 0x07;
    return (analyzer_model_bandwidth_hz(model) <= ANALYZER_AGC_PIVOT_BW_HZ) ?
               ANALYZER_AGC_TARGET_LOW :
               ANALYZER_AGC_TARGET_HIGH;
}

uint8_t analyzer_model_agc_target_alternative(const AnalyzerModel* model) {
    AnalyzerModel probe = *model;
    probe.magn_target = ANALYZER_AGC_AUTO;
    return (analyzer_agc_target(&probe) == ANALYZER_AGC_TARGET_LOW) ? ANALYZER_AGC_TARGET_HIGH :
                                                                      ANALYZER_AGC_TARGET_LOW;
}

static uint8_t analyzer_nearest(const AnalyzerValue* table, size_t count, uint32_t value) {
    size_t best = 0;
    uint32_t best_err = UINT32_MAX;
    for(size_t i = 0; i < count; i++) {
        const uint32_t err = (table[i].value > value) ? (table[i].value - value) :
                                                        (value - table[i].value);
        if(err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return (uint8_t)best;
}

uint32_t analyzer_model_settle_ms(uint32_t bw_hz) {
    if(bw_hz == 0) return 20u;
    const uint32_t scaled = (3u * 650000u + bw_hz - 1u) / bw_hz;
    if(scaled < 3u) return 3u;
    if(scaled > 20u) return 20u;
    return scaled;
}

AnalyzerReach
    analyzer_model_reach(uint32_t deviation_hz, uint32_t baud, uint32_t* required_bw_hz) {
    const uint32_t needed = 2u * deviation_hz + baud;
    if(required_bw_hz) *required_bw_hz = needed;

    if(deviation_hz > ANALYZER_CC1101_DEV_MAX_HZ) return AnalyzerReachDevTooWide;
    if(needed > ANALYZER_CC1101_BW_MAX_HZ) return AnalyzerReachBwTooWide;
    if(deviation_hz > (ANALYZER_CC1101_DEV_MAX_HZ / 10u) * 9u ||
       needed > (ANALYZER_CC1101_BW_MAX_HZ / 10u) * 9u) {
        return AnalyzerReachTight;
    }
    return AnalyzerReachOk;
}

const char* analyzer_model_reach_name(AnalyzerReach reach) {
    switch(reach) {
    case AnalyzerReachOk:
        return "within the CC1101's range";
    case AnalyzerReachTight:
        return "near the CC1101's limit";
    case AnalyzerReachDevTooWide:
        return "deviation beyond the CC1101's maximum";
    case AnalyzerReachBwTooWide:
        return "bandwidth beyond the CC1101's maximum";
    default:
        return "unknown";
    }
}

bool analyzer_model_frequency_valid(uint32_t frequency) {
    for(size_t i = 0; i < ANALYZER_BAND_COUNT; i++) {
        if(frequency >= analyzer_bands[i].low && frequency <= analyzer_bands[i].high) {
            return true;
        }
    }
    return false;
}

void analyzer_model_init(AnalyzerModel* model) {
    memset(model, 0, sizeof(*model));
    model->frequency = 433920000;
    model->mod = AnalyzerModOok;
    model->bw_idx = analyzer_nearest(analyzer_bw, ANALYZER_BW_COUNT, 650000);
    model->dev_idx = analyzer_nearest(analyzer_dev, ANALYZER_DEV_COUNT, 47607);
    model->rate_idx = 18;
    model->if_idx = 6;
    model->close_in_rx = 0;
    model->max_lna_gain = 0;
    model->magn_target = ANALYZER_AGC_AUTO;
    model->dcfilt_off = false;
    model->cs_gate = true;
}

uint32_t analyzer_model_bandwidth_hz(const AnalyzerModel* model) {
    return analyzer_bw[model->bw_idx % ANALYZER_BW_COUNT].value;
}

uint32_t analyzer_model_deviation_hz(const AnalyzerModel* model) {
    return analyzer_dev[model->dev_idx % ANALYZER_DEV_COUNT].value;
}

uint32_t analyzer_model_rate_from_regs(uint8_t e, uint8_t m) {
    const uint64_t num = (uint64_t)(256u + m) * ((uint64_t)1u << (e & 0x0F)) * ANALYZER_FXOSC_HZ;
    return (uint32_t)(num >> 28);
}

uint32_t analyzer_model_bandwidth_from_reg(uint8_t chanbw_nibble) {
    const uint32_t e = (chanbw_nibble >> 2) & 0x03u;
    const uint32_t m = chanbw_nibble & 0x03u;
    return (uint32_t)(ANALYZER_FXOSC_HZ / (8ULL * (4ULL + m) * (1ULL << e)));
}

uint32_t analyzer_model_deviation_from_reg(uint8_t deviatn) {
    const uint32_t e = (deviatn >> 4) & 0x07u;
    const uint32_t m = deviatn & 0x07u;
    return (uint32_t)((ANALYZER_FXOSC_HZ * (8ULL + m) * (1ULL << e)) >> 17);
}

uint32_t analyzer_model_rate_baud(const AnalyzerModel* model) {
    if(model->rate_exact) return analyzer_model_rate_from_regs(model->rate_e, model->rate_m);
    return analyzer_rate[model->rate_idx % ANALYZER_RATE_COUNT].baud;
}

void analyzer_model_set_rate_baud(AnalyzerModel* model, uint32_t baud) {
    model->rate_exact = false;
    if(baud == 0) return;

    for(uint8_t e = 0; e < 16u; e++) {
        const uint64_t scaled = ((uint64_t)baud << 28) / ANALYZER_FXOSC_HZ;
        const uint64_t mant = scaled >> e;
        if(mant < 256u) continue;
        if(mant > 511u) continue;

        model->rate_e = e;
        model->rate_m = (uint8_t)(mant - 256u);
        model->rate_exact = true;
        model->rate_idx = analyzer_model_rate_index_for_baud(baud);
        return;
    }

    model->rate_idx = analyzer_model_rate_index_for_baud(baud);
}

uint8_t analyzer_model_bw_index_for_hz(uint32_t hz) {
    return analyzer_nearest(analyzer_bw, ANALYZER_BW_COUNT, hz);
}

uint8_t analyzer_model_bw_index_atleast(uint32_t hz) {
    for(uint8_t i = 0; i < ANALYZER_BW_COUNT; i++) {
        if(analyzer_bw[i].value >= hz) return i;
    }
    return ANALYZER_BW_COUNT - 1u;
}

uint8_t analyzer_model_dev_index_for_hz(uint32_t hz) {
    return analyzer_nearest(analyzer_dev, ANALYZER_DEV_COUNT, hz);
}

uint8_t analyzer_model_rate_index_for_baud(uint32_t baud) {
    size_t best = 0;
    uint32_t best_err = UINT32_MAX;
    for(size_t i = 0; i < ANALYZER_RATE_COUNT; i++) {
        const uint32_t r = analyzer_rate[i].baud;
        const uint32_t err = (r > baud) ? (r - baud) : (baud - r);
        if(err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return (uint8_t)best;
}

static uint8_t analyzer_sync_mode(uint8_t mod, bool cs_gate) {
    if(mod == AnalyzerModOok) return 0x00;
    return cs_gate ? 0x04 : 0x00;
}

static uint8_t analyzer_mod_format(uint8_t mod) {
    switch(mod) {
    case AnalyzerModGfsk:
        return 1;
    case AnalyzerModOok:
        return 3;
    default:
        return 0;
    }
}

size_t analyzer_model_build(const AnalyzerModel* model, uint8_t* out, size_t cap) {
    if(cap < ANALYZER_MODEL_BLOB_MAX) return 0;

    const bool ook = (model->mod == AnalyzerModOok);
    const uint32_t bw = analyzer_model_bandwidth_hz(model);
    const AnalyzerRate* grid = &analyzer_rate[model->rate_idx % ANALYZER_RATE_COUNT];
    const uint8_t rate_e = model->rate_exact ? model->rate_e : grid->e;
    const uint8_t rate_m = model->rate_exact ? model->rate_m : grid->m;

    uint8_t fifothr = 0x07;
    if(bw < ANALYZER_ADC_RETENTION_BW_HZ) fifothr |= 0x40;
    fifothr |= (uint8_t)((model->close_in_rx & 0x03) << 4);

    const uint8_t mdmcfg2 =
        (uint8_t)((model->dcfilt_off ? 0x80 : 0x00) | (analyzer_mod_format(model->mod) << 4) |
                  analyzer_sync_mode(model->mod, model->cs_gate));

    const uint8_t agcctrl2 =
        (uint8_t)(((model->max_lna_gain & 0x07) << 3) | analyzer_agc_target(model));

    size_t i = 0;
#define PUT(reg, val)              \
    do {                           \
        out[i++] = (reg);          \
        out[i++] = (uint8_t)(val); \
    } while(0)

    PUT(CC1101_IOCFG0, CC1101IocfgSerialDataOutput);
    PUT(CC1101_PKTCTRL0, 0x32);
    if(!ook) PUT(CC1101_PKTCTRL1, 0x04);

    PUT(CC1101_FIFOTHR, fifothr);

    PUT(CC1101_FSCTRL1, model->if_idx & 0x1F);
    PUT(CC1101_FSCTRL0, (uint8_t)model->freq_offset);

    PUT(CC1101_MDMCFG4,
        (uint8_t)((analyzer_bw[model->bw_idx % ANALYZER_BW_COUNT].reg << 4) | (rate_e & 0x0F)));
    PUT(CC1101_MDMCFG3, rate_m);
    PUT(CC1101_MDMCFG2, mdmcfg2);
    PUT(CC1101_MDMCFG1, ook ? 0x00 : 0x02);
    PUT(CC1101_MDMCFG0, 0x00);

    PUT(CC1101_DEVIATN, analyzer_dev[model->dev_idx % ANALYZER_DEV_COUNT].reg);

    PUT(CC1101_MCSM0, 0x18);
    PUT(CC1101_FOCCFG, ook ? 0x18 : 0x17);

    PUT(CC1101_AGCCTRL2, agcctrl2);
    PUT(CC1101_AGCCTRL1, 0x00);
    PUT(CC1101_AGCCTRL0, 0x91);

    PUT(CC1101_WORCTRL, 0xFB);
    PUT(CC1101_FREND0, ook ? 0x11 : 0x10);
    PUT(CC1101_FREND1, ook ? 0xB6 : 0x56);
#undef PUT

    out[i++] = 0;
    out[i++] = 0;

    memset(&out[i], 0, 8);
    if(ook) {
        out[i + 1] = 0xC0;
    } else {
        out[i + 0] = 0xC0;
    }
    i += 8;

    return i;
}

static const char* const analyzer_mod_name[AnalyzerModCount] = {"2-FSK", "GFSK", "ASK/OOK"};

void analyzer_model_format(const AnalyzerModel* model, AnalyzerParam param, char* out, size_t cap) {
    switch(param) {
    case AnalyzerParamFrequency:
        snprintf(
            out,
            cap,
            "%lu.%02lu MHz",
            (unsigned long)(model->frequency / 1000000u),
            (unsigned long)((model->frequency % 1000000u) / 10000u));
        break;
    case AnalyzerParamModulation:
        snprintf(out, cap, "%s", analyzer_mod_name[model->mod % AnalyzerModCount]);
        break;
    case AnalyzerParamBandwidth: {
        const uint32_t v = analyzer_model_bandwidth_hz(model);
        snprintf(
            out,
            cap,
            "%lu.%02lu kHz",
            (unsigned long)(v / 1000u),
            (unsigned long)((v % 1000u) / 10u));
        break;
    }
    case AnalyzerParamDeviation: {
        if(model->mod == AnalyzerModOok) {
            snprintf(out, cap, "n/a (OOK)");
            break;
        }
        const uint32_t v = analyzer_model_deviation_hz(model);
        snprintf(
            out,
            cap,
            "%lu.%02lu kHz",
            (unsigned long)(v / 1000u),
            (unsigned long)((v % 1000u) / 10u));
        break;
    }
    case AnalyzerParamRate: {
        const uint32_t v = analyzer_model_rate_baud(model);
        snprintf(
            out,
            cap,
            "%lu.%02lu kBd",
            (unsigned long)(v / 1000u),
            (unsigned long)((v % 1000u) / 10u));
        break;
    }
    case AnalyzerParamRxAtten:
        snprintf(out, cap, "%u dB", (unsigned)(model->close_in_rx * 6u));
        break;
    }
}
