#include "analyzer_preset.h"

#include <cc1101_regs.h>

#include <stdio.h>
#include <string.h>

#define ANALYZER_PRESET_W_RATE 4u
#define ANALYZER_PRESET_W_BW   3u
#define ANALYZER_PRESET_W_DEV  1u

#define ANALYZER_PRESET_CLIP_PENALTY 12u

#define ANALYZER_PRESET_SHAPE_PENALTY 2u

#define ANALYZER_PRESET_CLOSE_BW_STEPS   1u
#define ANALYZER_PRESET_CLOSE_RATE_STEPS 2u
#define ANALYZER_PRESET_CLOSE_DEV_STEPS  4u

const char* analyzer_preset_tier_name(AnalyzerMatchTier tier) {
    switch(tier) {
    case AnalyzerMatchEquivalent:
        return "equivalent";
    case AnalyzerMatchClose:
        return "close";
    case AnalyzerMatchWrongFamily:
        return "other mod";
    case AnalyzerMatchDifferent:
    default:
        return "different";
    }
}

static bool analyzer_preset_family_fsk(uint8_t mod) {
    return mod != AnalyzerModOok;
}

bool analyzer_preset_decode(const uint8_t* blob, size_t len, AnalyzerPresetParams* out) {
    memset(out, 0, sizeof(*out));
    if(!blob || len < 2u) return false;

    bool have_mdmcfg4 = false, have_mdmcfg3 = false, have_mdmcfg2 = false, have_deviatn = false;
    uint8_t mdmcfg4 = 0, mdmcfg3 = 0, mdmcfg2 = 0, deviatn = 0;

    for(size_t i = 0; i + 1u < len; i += 2u) {
        const uint8_t reg = blob[i];
        const uint8_t val = blob[i + 1u];
        if(reg == 0u) break;

        switch(reg) {
        case CC1101_MDMCFG4:
            mdmcfg4 = val;
            have_mdmcfg4 = true;
            break;
        case CC1101_MDMCFG3:
            mdmcfg3 = val;
            have_mdmcfg3 = true;
            break;
        case CC1101_MDMCFG2:
            mdmcfg2 = val;
            have_mdmcfg2 = true;
            break;
        case CC1101_DEVIATN:
            deviatn = val;
            have_deviatn = true;
            break;
        default:
            break;
        }
    }

    if(!have_mdmcfg4 || !have_mdmcfg3 || !have_mdmcfg2) return false;

    const uint8_t mod_format = (uint8_t)((mdmcfg2 >> 4) & 0x07u);
    switch(mod_format) {
    case 0:
        out->mod = AnalyzerMod2Fsk;
        break;
    case 1:
        out->mod = AnalyzerModGfsk;
        break;
    case 3:
        out->mod = AnalyzerModOok;
        break;
    default:
        return false;
    }

    out->bandwidth_hz = analyzer_model_bandwidth_from_reg((uint8_t)((mdmcfg4 >> 4) & 0x0Fu));
    out->rate_baud = analyzer_model_rate_from_regs((uint8_t)(mdmcfg4 & 0x0Fu), mdmcfg3);

    out->has_deviation = have_deviatn && analyzer_preset_family_fsk(out->mod);
    out->deviation_hz = out->has_deviation ? analyzer_model_deviation_from_reg(deviatn) : 0u;

    out->dcfilt_off = (mdmcfg2 & 0x80u) != 0u;
    out->cs_gate = ((mdmcfg2 & 0x07u) == 0x04u);
    out->valid = true;
    return true;
}

void analyzer_preset_params_from_model(const AnalyzerModel* model, AnalyzerPresetParams* out) {
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->mod = model->mod;
    out->bandwidth_hz = analyzer_model_bandwidth_hz(model);
    out->rate_baud = analyzer_model_rate_baud(model);
    out->has_deviation = analyzer_preset_family_fsk(model->mod);
    out->deviation_hz = out->has_deviation ? analyzer_model_deviation_hz(model) : 0u;
    out->dcfilt_off = model->dcfilt_off;
    out->cs_gate = model->cs_gate;
}

static uint16_t analyzer_preset_abs_diff(uint8_t a, uint8_t b) {
    return (uint16_t)((a > b) ? (a - b) : (b - a));
}

static void analyzer_preset_compare(
    const AnalyzerPresetParams* measured,
    const AnalyzerPresetParams* preset,
    uint32_t occupied_bw_hz,
    AnalyzerPresetMatch* out) {
    if(analyzer_preset_family_fsk(measured->mod) != analyzer_preset_family_fsk(preset->mod)) {
        out->tier = AnalyzerMatchWrongFamily;
        out->grid_delta = UINT16_MAX;
        return;
    }

    const uint8_t bw_m = analyzer_model_bw_index_for_hz(measured->bandwidth_hz);
    const uint8_t bw_p = analyzer_model_bw_index_for_hz(preset->bandwidth_hz);
    const uint8_t rate_m = analyzer_model_rate_index_for_baud(measured->rate_baud);
    const uint8_t rate_p = analyzer_model_rate_index_for_baud(preset->rate_baud);

    const uint16_t d_bw = analyzer_preset_abs_diff(bw_m, bw_p);
    const uint16_t d_rate = analyzer_preset_abs_diff(rate_m, rate_p);

    uint16_t d_dev = 0;
    const bool score_dev = measured->has_deviation && preset->has_deviation;
    if(score_dev) {
        d_dev = analyzer_preset_abs_diff(
            analyzer_model_dev_index_for_hz(measured->deviation_hz),
            analyzer_model_dev_index_for_hz(preset->deviation_hz));
    }

    uint16_t delta = (uint16_t)(d_bw * ANALYZER_PRESET_W_BW + d_rate * ANALYZER_PRESET_W_RATE +
                                d_dev * ANALYZER_PRESET_W_DEV);

    if(measured->mod != preset->mod) delta = (uint16_t)(delta + ANALYZER_PRESET_SHAPE_PENALTY);

    out->narrower_than_signal = (occupied_bw_hz != 0u) && (preset->bandwidth_hz < occupied_bw_hz);
    if(out->narrower_than_signal) {
        delta = (uint16_t)(delta + ANALYZER_PRESET_CLIP_PENALTY);
    }

    out->grid_delta = delta;

    uint16_t worst = d_rate;
    out->worst_axis = AnalyzerParamRate;
    out->worst_measured = measured->rate_baud;
    out->worst_preset = preset->rate_baud;
    if(d_bw > worst) {
        worst = d_bw;
        out->worst_axis = AnalyzerParamBandwidth;
        out->worst_measured = measured->bandwidth_hz;
        out->worst_preset = preset->bandwidth_hz;
    }
    if(score_dev && d_dev > worst) {
        worst = d_dev;
        out->worst_axis = AnalyzerParamDeviation;
        out->worst_measured = measured->deviation_hz;
        out->worst_preset = preset->deviation_hz;
    }

    const bool same_grid = (d_bw == 0u) && (d_rate == 0u) && (d_dev == 0u);
    if(same_grid && measured->mod == preset->mod) {
        out->tier = AnalyzerMatchEquivalent;
    } else if(
        !out->narrower_than_signal && d_bw <= ANALYZER_PRESET_CLOSE_BW_STEPS &&
        d_rate <= ANALYZER_PRESET_CLOSE_RATE_STEPS && d_dev <= ANALYZER_PRESET_CLOSE_DEV_STEPS) {
        out->tier = AnalyzerMatchClose;
    } else {
        out->tier = AnalyzerMatchDifferent;
    }
}

uint8_t analyzer_preset_rank(
    SubGhzSetting* setting,
    const AnalyzerModel* measured,
    uint32_t occupied_bw_hz,
    AnalyzerPresetMatch* out,
    uint8_t cap) {
    if(!setting || !measured || !out || cap == 0u) return 0;

    AnalyzerPresetParams want;
    analyzer_preset_params_from_model(measured, &want);

    uint8_t blob[ANALYZER_MODEL_BLOB_MAX];
    const size_t blob_len = analyzer_model_build(measured, blob, sizeof(blob));

    uint8_t count = 0;
    const size_t preset_count = subghz_setting_get_preset_count(setting);

    for(size_t idx = 0; idx < preset_count; idx++) {
        const size_t data_len = subghz_setting_get_preset_data_size(setting, idx);
        uint8_t* data = subghz_setting_get_preset_data(setting, idx);
        if(!data || data_len == 0u) continue;

        AnalyzerPresetParams have;
        if(!analyzer_preset_decode(data, data_len, &have)) continue;

        AnalyzerPresetMatch match;
        memset(&match, 0, sizeof(match));
        match.setting_index = (int)idx;
        const char* name = subghz_setting_get_preset_name(setting, idx);
        snprintf(match.name, sizeof(match.name), "%s", name ? name : "?");

        match.exact_blob = (blob_len != 0u) && (data_len == blob_len) &&
                           (memcmp(data, blob, blob_len) == 0);

        analyzer_preset_compare(&want, &have, occupied_bw_hz, &match);
        if(match.tier == AnalyzerMatchWrongFamily) continue;

        if(match.exact_blob) {
            match.tier = AnalyzerMatchEquivalent;
            match.grid_delta = 0;
        }

        uint8_t pos = count;
        while(pos > 0u && out[pos - 1u].grid_delta > match.grid_delta) {
            if(pos < cap) out[pos] = out[pos - 1u];
            pos--;
        }
        if(pos < cap) {
            out[pos] = match;
            if(count < cap) count++;
        }
    }

    return count;
}

void analyzer_preset_derive_name(const AnalyzerModel* model, char* out, size_t cap) {
    if(model->mod == AnalyzerModOok) {
        const uint32_t bw_khz = (analyzer_model_bandwidth_hz(model) + 500u) / 1000u;
        snprintf(out, cap, "AM%lu", (unsigned long)bw_khz);
        return;
    }

    const uint32_t dev_hz = analyzer_model_deviation_hz(model);
    if(dev_hz < 15000u) {
        snprintf(out, cap, "FM%luK", (unsigned long)((dev_hz + 500u) / 1000u));
    } else {
        snprintf(out, cap, "FM%lu", (unsigned long)((dev_hz + 50u) / 100u));
    }
}
