#include "mercedes.h"

#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"

#include "../blocks/custom_btn_i.h"

#include <stdio.h>
#include <string.h>

#define TAG "SubGhzProtocolMERCEDES"

#define MERCEDES_DEBUG       1
#define MERCEDES_DEBUG_BURST 0

#define MERCEDES_TE_US            50
#define MERCEDES_TE1_MIN_US       25
#define MERCEDES_TE1_MAX_US       79
#define MERCEDES_TE2_MIN_US       80
#define MERCEDES_TE2_MAX_US       140
#define MERCEDES_GAP_US           280
#define MERCEDES_PREAMBLE_MIN     32
#define MERCEDES_PAYLOAD_BITS     122
#define MERCEDES_CNT_BITS         10
#define MERCEDES_CNT_MASK         0x3FFU
#define MERCEDES_HOP_MASK         0x3FFFFFFFFFFFFFULL
#define MERCEDES_FIXED_4_11       0x54U
#define MERCEDES_PREAMBLE_TX_BITS 561
#define MERCEDES_REPEAT_DEFAULT   4
#define MERCEDES_REPEAT_GAP_US    50000
#define MERCEDES_UPLOAD_CAPACITY  3072
#define MERCEDES_LO_INVERT_MASK   0xFFFFFFFFFFFFFFC0ULL

typedef enum {
    MERCEDESDecoderStepReset = 0,
    MERCEDESDecoderStepPreambleLow,
    MERCEDESDecoderStepPreambleHigh,
    MERCEDESDecoderStepData,
} MERCEDESDecoderStep;

#if MERCEDES_DEBUG
#define MERCEDES_DBG_MIN_EDGES   64
#define MERCEDES_DBG_QUIET_MS    200
#define MERCEDES_DBG_FRAME_EDGES 200
#define MERCEDES_DBG_SNAP        12

typedef struct {
    uint32_t edges;
    uint32_t us_total;
    uint32_t gap_us;
    uint16_t run_cur;
    uint16_t run_best;
    uint16_t n1;
    uint16_t n2;
    uint16_t bad_short;
    uint16_t bad_dead;
    uint16_t bad_long;
    uint16_t d_min;
    uint16_t d_max;
    uint32_t hi_sum;
    uint32_t lo_sum;
    uint16_t hi_n;
    uint16_t lo_n;
    uint16_t pre_best;
    uint16_t data_best;
    uint16_t resets;
    uint16_t rx_resets;
    uint16_t break_dur;
    uint16_t break_bits;
    uint8_t step_best;
    uint8_t snap_n;
    uint16_t snap[MERCEDES_DBG_SNAP];
    uint16_t last_edge;
    uint32_t best_edges;
    uint16_t best_run;
    uint32_t ok_total;
    uint32_t rej_total;
    uint32_t last_log_tick;
} MercedesDecoderDebug;
#endif

struct SubGhzProtocolDecoderMERCEDES {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint64_t hi;
    uint64_t lo;
    uint16_t preamble_count;
    int8_t pair_chip;
    uint8_t nbits;
#if MERCEDES_DEBUG
    MercedesDecoderDebug dbg;
#endif
};

struct SubGhzProtocolEncoderMERCEDES {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

static void mercedes_reset_soft(SubGhzProtocolDecoderMERCEDES* instance);
static void mercedes_parse_fields(SubGhzBlockGeneric* instance);
static void mercedes_rebuild_payload(SubGhzBlockGeneric* instance);
static bool mercedes_get_bit(uint64_t hi, uint64_t lo, uint8_t i);
static uint16_t mercedes_bits(uint64_t hi, uint64_t lo, uint8_t start, uint8_t len);
static void mercedes_invert(uint64_t* hi, uint64_t* lo);
static bool mercedes_fields_valid(uint64_t hi, uint64_t lo);
static bool mercedes_to_storage(uint64_t* hi, uint64_t* lo);
static uint32_t mercedes_get_counter(uint64_t hi, uint64_t lo);
static void mercedes_set_counter(uint64_t* hi, uint64_t* lo, uint32_t cnt);

const SubGhzProtocolDecoder subghz_protocol_mercedes_decoder = {
    .alloc = subghz_protocol_decoder_mercedes_alloc,
    .free = subghz_protocol_decoder_mercedes_free,
    .feed = subghz_protocol_decoder_mercedes_feed,
    .reset = subghz_protocol_decoder_mercedes_reset,
    .get_hash_data = subghz_protocol_decoder_mercedes_get_hash_data,
    .serialize = subghz_protocol_decoder_mercedes_serialize,
    .deserialize = subghz_protocol_decoder_mercedes_deserialize,
    .get_string = subghz_protocol_decoder_mercedes_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_mercedes_encoder = {
    .alloc = subghz_protocol_encoder_mercedes_alloc,
    .free = subghz_protocol_encoder_mercedes_free,
    .deserialize = subghz_protocol_encoder_mercedes_deserialize,
    .stop = subghz_protocol_encoder_mercedes_stop,
    .yield = subghz_protocol_encoder_mercedes_yield,
};

const SubGhzProtocol subghz_protocol_mercedes = {
    .name = SUBGHZ_PROTOCOL_MERCEDES_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_mercedes_decoder,
    .encoder = &subghz_protocol_mercedes_encoder,
};

static bool mercedes_is_1t(uint32_t duration) {
    return (duration >= MERCEDES_TE1_MIN_US) && (duration <= MERCEDES_TE1_MAX_US);
}

static bool mercedes_is_2t(uint32_t duration) {
    return (duration >= MERCEDES_TE2_MIN_US) && (duration <= MERCEDES_TE2_MAX_US);
}

static uint8_t mercedes_chips(uint32_t duration) {
    if(mercedes_is_1t(duration)) return 1;
    if(mercedes_is_2t(duration)) return 2;
    return 0;
}

#if MERCEDES_DEBUG
static void mercedes_dbg_burst_clear(MercedesDecoderDebug* d) {
    d->edges = 0;
    d->us_total = 0;
    d->gap_us = 0;
    d->run_cur = 0;
    d->run_best = 0;
    d->n1 = 0;
    d->n2 = 0;
    d->bad_short = 0;
    d->bad_dead = 0;
    d->bad_long = 0;
    d->d_min = 0xFFFFU;
    d->d_max = 0;
    d->hi_sum = 0;
    d->lo_sum = 0;
    d->hi_n = 0;
    d->lo_n = 0;
    d->pre_best = 0;
    d->data_best = 0;
    d->resets = 0;
    d->rx_resets = 0;
    d->break_dur = 0;
    d->break_bits = 0;
    d->step_best = 0;
    d->snap_n = 0;
    d->last_edge = 0;
}

#if MERCEDES_DEBUG_BURST
static const char* mercedes_dbg_step_name(uint8_t step) {
    switch(step) {
    case MERCEDESDecoderStepReset:
        return "nothing";
    case MERCEDESDecoderStepPreambleLow:
    case MERCEDESDecoderStepPreambleHigh:
        return "preamble";
    case MERCEDESDecoderStepData:
        return "payload";
    default:
        return "?";
    }
}

static void mercedes_dbg_edge(
    SubGhzProtocolDecoderMERCEDES* instance,
    bool level,
    uint32_t duration,
    uint8_t n) {
    MercedesDecoderDebug* d = &instance->dbg;
    const uint32_t step = instance->decoder.parser_step;

    d->edges++;
    d->us_total += duration;
    if(n) {
        if(d->run_cur < 0xFFFFU) d->run_cur++;
        if(d->run_cur > d->run_best) d->run_best = d->run_cur;
    } else {
        d->run_cur = 0;
    }
    if(duration < d->d_min) d->d_min = (uint16_t)duration;
    if(duration > d->d_max) d->d_max = (uint16_t)duration;

    if(n == 1) {
        d->n1++;
        if(level) {
            d->hi_sum += duration;
            d->hi_n++;
        } else {
            d->lo_sum += duration;
            d->lo_n++;
        }
    } else if(n == 2) {
        d->n2++;
    } else {
        if(duration < MERCEDES_TE1_MIN_US) {
            d->bad_short++;
        } else if(duration < MERCEDES_TE2_MIN_US) {
            d->bad_dead++;
        } else {
            d->bad_long++;
        }
        if(d->break_dur == 0 && step == MERCEDESDecoderStepData) {
            d->break_dur = (uint16_t)duration;
            d->break_bits = instance->nbits;
        }
    }

    if(step > d->step_best) d->step_best = (uint8_t)step;
    if(instance->preamble_count > d->pre_best) d->pre_best = instance->preamble_count;
    if(instance->nbits > d->data_best) d->data_best = instance->nbits;

    d->last_edge = (uint16_t)(duration & 0x7FFFU) | (level ? 0x8000U : 0U);
    if(d->snap_n < MERCEDES_DBG_SNAP && step == MERCEDESDecoderStepData) {
        d->snap[d->snap_n++] = d->last_edge;
    }
}

static void mercedes_dbg_summary(SubGhzProtocolDecoderMERCEDES* instance) {
    MercedesDecoderDebug* d = &instance->dbg;

    if(d->edges < MERCEDES_DBG_MIN_EDGES) {
        mercedes_dbg_burst_clear(d);
        return;
    }

    if(d->edges > d->best_edges) d->best_edges = d->edges;
    if(d->run_best > d->best_run) d->best_run = d->run_best;

    const bool reached_data = d->step_best >= MERCEDESDecoderStepData;
    const bool frame_sized = d->edges >= MERCEDES_DBG_FRAME_EDGES;
    const uint32_t now = furi_get_tick();
    if(!reached_data && !frame_sized &&
       (now - d->last_log_tick) < furi_ms_to_ticks(MERCEDES_DBG_QUIET_MS)) {
        mercedes_dbg_burst_clear(d);
        return;
    }
    d->last_log_tick = now;

    const char* verdict = (d->run_best >= 100) ? "FRAME" :
                          (d->run_best >= 20)  ? "PARTIAL" :
                                                 "noise";

    FURI_LOG_I(
        TAG,
        "%s: %lu edges / %lu us (%lu us/edge) | clean run %u | span %u-%u us",
        verdict,
        d->edges,
        d->us_total,
        d->us_total / d->edges,
        d->run_best,
        d->d_min,
        d->d_max);
    FURI_LOG_I(
        TAG,
        "  1T %u (hi %lu lo %lu) 2T %u | unusable %u = %u short %u dead %u long",
        d->n1,
        d->hi_n ? d->hi_sum / d->hi_n : 0UL,
        d->lo_n ? d->lo_sum / d->lo_n : 0UL,
        d->n2,
        (uint16_t)(d->bad_short + d->bad_dead + d->bad_long),
        d->bad_short,
        d->bad_dead,
        d->bad_long);
    FURI_LOG_I(
        TAG,
        "  reached %s | preamble %u/%u | payload %u/%u",
        mercedes_dbg_step_name(d->step_best),
        d->pre_best,
        MERCEDES_PREAMBLE_MIN,
        d->data_best,
        MERCEDES_PAYLOAD_BITS);
    FURI_LOG_I(
        TAG,
        "  resets %u soft %u rx | gap %lu us | peak %lu edges %u run | ok %lu rej %lu",
        d->resets,
        d->rx_resets,
        d->gap_us,
        d->best_edges,
        d->best_run,
        d->ok_total,
        d->rej_total);

    if(reached_data) {
        char snap[96];
        size_t off = 0;
        for(uint8_t i = 0; i < d->snap_n && off + 8 < sizeof(snap); i++) {
            off += snprintf(
                snap + off,
                sizeof(snap) - off,
                " %c%u",
                (d->snap[i] & 0x8000U) ? '+' : '-',
                (unsigned)(d->snap[i] & 0x7FFFU));
        }
        snap[off] = '\0';
        if(d->break_dur) {
            FURI_LOG_I(
                TAG,
                "  broke on a %u us edge at payload %u | chips:%s",
                d->break_dur,
                d->break_bits,
                snap);
        } else {
            FURI_LOG_I(TAG, "  no unusable edge inside payload | chips:%s", snap);
        }
    }

    mercedes_dbg_burst_clear(d);
}
#endif
#endif

static bool mercedes_get_bit(uint64_t hi, uint64_t lo, uint8_t i) {
    if(i < 64) {
        return (hi >> (63U - i)) & 1U;
    }
    return (lo >> (63U - (i - 64U))) & 1U;
}

static void mercedes_set_bit(uint64_t* hi, uint64_t* lo, uint8_t i, bool value) {
    if(i < 64) {
        uint64_t mask = 1ULL << (63U - i);
        if(value) {
            *hi |= mask;
        } else {
            *hi &= ~mask;
        }
    } else {
        uint64_t mask = 1ULL << (63U - (i - 64U));
        if(value) {
            *lo |= mask;
        } else {
            *lo &= ~mask;
        }
    }
}

static uint16_t mercedes_bits(uint64_t hi, uint64_t lo, uint8_t start, uint8_t len) {
    uint16_t value = 0;
    for(uint8_t i = 0; i < len; i++) {
        value =
            (uint16_t)((value << 1) | (mercedes_get_bit(hi, lo, (uint8_t)(start + i)) ? 1U : 0U));
    }
    return value;
}

static void mercedes_invert(uint64_t* hi, uint64_t* lo) {
    *hi = ~(*hi);
    *lo ^= MERCEDES_LO_INVERT_MASK;
}

static uint32_t mercedes_get_counter(uint64_t hi, uint64_t lo) {
    uint32_t cnt = mercedes_get_bit(hi, lo, 48) ? 0x200UL : 0UL;
    if(mercedes_get_bit(hi, lo, 49)) cnt |= 0x100UL;
    for(uint8_t k = 0; k < 8; k++) {
        if(mercedes_get_bit(hi, lo, (uint8_t)(57 + k))) {
            cnt |= (1UL << k);
        }
    }
    return cnt & MERCEDES_CNT_MASK;
}

static void mercedes_set_counter(uint64_t* hi, uint64_t* lo, uint32_t cnt) {
    cnt &= MERCEDES_CNT_MASK;
    mercedes_set_bit(hi, lo, 48, cnt & 0x200UL);
    mercedes_set_bit(hi, lo, 49, cnt & 0x100UL);
    for(uint8_t k = 0; k < 8; k++) {
        mercedes_set_bit(hi, lo, (uint8_t)(57 + k), (cnt >> k) & 1U);
    }
}

static bool mercedes_fields_valid(uint64_t hi, uint64_t lo) {
    const bool btn = mercedes_get_bit(hi, lo, 1);
    const bool counter_state = mercedes_get_bit(hi, lo, 2);

    if(!mercedes_get_bit(hi, lo, 0)) return false;
    if(mercedes_get_bit(hi, lo, 3) != !(btn ^ counter_state)) return false;
    if(mercedes_bits(hi, lo, 4, 8) != MERCEDES_FIXED_4_11) return false;
    if(!mercedes_get_bit(hi, lo, 121)) return false;
    return true;
}

#if MERCEDES_DEBUG
static const char* mercedes_fields_fail(uint64_t hi, uint64_t lo) {
    const bool btn = mercedes_get_bit(hi, lo, 1);
    const bool counter_state = mercedes_get_bit(hi, lo, 2);

    if(!mercedes_get_bit(hi, lo, 0)) return "bit0";
    if(mercedes_get_bit(hi, lo, 3) != !(btn ^ counter_state)) return "btn";
    if(mercedes_bits(hi, lo, 4, 8) != MERCEDES_FIXED_4_11) return "fix4";
    if(!mercedes_get_bit(hi, lo, 121)) return "bit121";
    return "ok";
}
#endif

static bool mercedes_to_storage(uint64_t* hi, uint64_t* lo) {
    if(!mercedes_get_bit(*hi, *lo, 0)) {
        mercedes_invert(hi, lo);
    }
    return mercedes_fields_valid(*hi, *lo);
}

static void mercedes_parse_fields(SubGhzBlockGeneric* instance) {
    const uint64_t hi = instance->data;
    const uint64_t lo = instance->data_2;

    instance->serial = ((uint32_t)mercedes_bits(hi, lo, 16, 16) << 16) |
                       mercedes_bits(hi, lo, 32, 16);
    instance->btn = mercedes_get_bit(hi, lo, 1) ? 1 : 0;
    instance->extra = mercedes_get_bit(hi, lo, 13) ? 0 : 1;
    instance->cnt = mercedes_get_counter(hi, lo);

    uint64_t hop = 0;
    for(uint8_t i = 67; i <= 120; i++) {
        hop = (hop << 1) | (mercedes_get_bit(hi, lo, i) ? 1ULL : 0ULL);
    }
    instance->data_3 = hop & MERCEDES_HOP_MASK;
    instance->te = MERCEDES_TE_US;
    instance->data_count_bit = MERCEDES_PAYLOAD_BITS;
}

static void mercedes_rebuild_payload(SubGhzBlockGeneric* instance) {
    uint64_t hi = instance->data;
    uint64_t lo = instance->data_2;
    const bool type_bit = instance->btn & 1U;
    const bool state_bit = mercedes_get_bit(hi, lo, 2);

    mercedes_set_bit(&hi, &lo, 0, true);
    mercedes_set_bit(&hi, &lo, 1, type_bit);
    mercedes_set_bit(&hi, &lo, 3, !(type_bit ^ state_bit));
    mercedes_set_bit(&hi, &lo, 13, !(instance->extra & 1U));
    mercedes_set_counter(&hi, &lo, instance->cnt);
    mercedes_set_bit(&hi, &lo, 121, true);

    instance->data = hi;
    instance->data_2 = lo;
    instance->data_count_bit = MERCEDES_PAYLOAD_BITS;
    instance->te = MERCEDES_TE_US;
}

static const char* mercedes_btn_name(uint8_t btn) {
    return btn ? "Unlock" : "Lock";
}

static uint8_t mercedes_get_btn_code(void) {
    SubGhzCustomButtonMapItem item;
    if(subghz_custom_btn_map_get(subghz_custom_btn_get(), &item)) {
        return item.code & 1U;
    }
    return subghz_custom_btn_get_original() & 1U;
}

static size_t
    mercedes_emit_merge(LevelDuration* up, size_t i, size_t cap, bool level, uint32_t us) {
    if(i > 0 && level_duration_get_level(up[i - 1]) == level) {
        uint32_t prev = level_duration_get_duration(up[i - 1]);
        up[i - 1] = level_duration_make(level, prev + us);
        return i;
    }
    if(i < cap) up[i++] = level_duration_make(level, us);
    return i;
}

static size_t mercedes_emit_bit(LevelDuration* up, size_t i, size_t cap, bool bit_value) {
    if(bit_value) {
        i = mercedes_emit_merge(up, i, cap, true, MERCEDES_TE_US);
        i = mercedes_emit_merge(up, i, cap, false, MERCEDES_TE_US);
    } else {
        i = mercedes_emit_merge(up, i, cap, false, MERCEDES_TE_US);
        i = mercedes_emit_merge(up, i, cap, true, MERCEDES_TE_US);
    }
    return i;
}

static bool mercedes_air_bit(uint64_t hi, uint64_t lo, uint8_t i) {
    return !mercedes_get_bit(hi, lo, i);
}

static size_t mercedes_emit_burst(
    LevelDuration* up,
    size_t i,
    size_t cap,
    const SubGhzBlockGeneric* generic) {
    for(uint16_t n = 0; n < MERCEDES_PREAMBLE_TX_BITS; n++) {
        i = mercedes_emit_bit(up, i, cap, true);
    }
    for(uint8_t bit = 0; bit < MERCEDES_PAYLOAD_BITS; bit++) {
        i = mercedes_emit_bit(up, i, cap, mercedes_air_bit(generic->data, generic->data_2, bit));
    }
    return i;
}

static bool mercedes_get_upload(SubGhzProtocolEncoderMERCEDES* instance) {
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = MERCEDES_UPLOAD_CAPACITY;
    size_t index = 0;

    uint8_t btn = instance->generic.btn;
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(btn);
        subghz_custom_btn_set_max(1);
    }
    btn = mercedes_get_btn_code();
    subghz_block_generic_global_button_override_get(&btn);
    if(btn != instance->generic.btn) {
        instance->generic.btn = btn;
        mercedes_rebuild_payload(&instance->generic);
    }
    if(subghz_block_generic_global_counter_override_get(&instance->generic.cnt)) {
        mercedes_rebuild_payload(&instance->generic);
    }

    index = mercedes_emit_burst(up, index, cap, &instance->generic);
    index = mercedes_emit_merge(up, index, cap, false, MERCEDES_REPEAT_GAP_US);

    instance->encoder.size_upload = index;
    return index > 0 && index < cap;
}

void* subghz_protocol_encoder_mercedes_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMERCEDES* instance = malloc(sizeof(SubGhzProtocolEncoderMERCEDES));
    memset(instance, 0, sizeof(SubGhzProtocolEncoderMERCEDES));
    instance->base.protocol = &subghz_protocol_mercedes;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = MERCEDES_REPEAT_DEFAULT;
    instance->encoder.size_upload = MERCEDES_UPLOAD_CAPACITY;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_mercedes_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderMERCEDES* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_mercedes_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderMERCEDES* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;
    if(instance->generic.data_count_bit != MERCEDES_PAYLOAD_BITS) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }
    mercedes_parse_fields(&instance->generic);
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(instance->generic.btn);
        subghz_custom_btn_set_max(1);
    }
    flipper_format_read_uint32(flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);
    instance->encoder.front = 0;
    if(!mercedes_get_upload(instance)) return SubGhzProtocolStatusErrorEncoderGetUpload;
    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_mercedes_stop(void* context) {
    SubGhzProtocolEncoderMERCEDES* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_mercedes_yield(void* context) {
    SubGhzProtocolEncoderMERCEDES* instance = context;
    if(instance->encoder.repeat == 0 || !instance->encoder.is_running ||
       instance->encoder.size_upload == 0) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

void* subghz_protocol_decoder_mercedes_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMERCEDES* instance = malloc(sizeof(SubGhzProtocolDecoderMERCEDES));
    memset(instance, 0, sizeof(SubGhzProtocolDecoderMERCEDES));
    instance->base.protocol = &subghz_protocol_mercedes;
    instance->generic.protocol_name = instance->base.protocol->name;
    mercedes_reset_soft(instance);
#if MERCEDES_DEBUG
    mercedes_dbg_burst_clear(&instance->dbg);
    FURI_LOG_I(
        TAG,
        "live 1T %u-%u us  2T %u-%u us  gap>=%u  TE=%u  FM20K",
        MERCEDES_TE1_MIN_US,
        MERCEDES_TE1_MAX_US,
        MERCEDES_TE2_MIN_US,
        MERCEDES_TE2_MAX_US,
        MERCEDES_GAP_US,
        MERCEDES_TE_US);
#endif
    return instance;
}

void subghz_protocol_decoder_mercedes_free(void* context) {
    furi_check(context);
    free(context);
}

static void mercedes_reset_soft(SubGhzProtocolDecoderMERCEDES* instance) {
#if MERCEDES_DEBUG
    if(instance->decoder.parser_step != MERCEDESDecoderStepReset && instance->dbg.resets < 0xFFFFU)
        instance->dbg.resets++;
#endif
    instance->decoder.parser_step = MERCEDESDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->hi = 0;
    instance->lo = 0;
    instance->preamble_count = 0;
    instance->pair_chip = -1;
    instance->nbits = 0;
}

void subghz_protocol_decoder_mercedes_reset(void* context) {
    furi_check(context);
#if MERCEDES_DEBUG
    SubGhzProtocolDecoderMERCEDES* instance = context;
    if(instance->dbg.rx_resets < 0xFFFFU) instance->dbg.rx_resets++;
#endif
    mercedes_reset_soft(context);
}

static void mercedes_try_callback(SubGhzProtocolDecoderMERCEDES* instance) {
    if(instance->nbits != MERCEDES_PAYLOAD_BITS) {
        mercedes_reset_soft(instance);
        return;
    }

    uint64_t hi = instance->hi;
    uint64_t lo = instance->lo;
#if MERCEDES_DEBUG
    const char* how = "sliced";
#endif
    if(!mercedes_to_storage(&hi, &lo)) {
        hi = instance->hi;
        lo = instance->lo;
        mercedes_invert(&hi, &lo);
#if MERCEDES_DEBUG
        how = "chipinv";
#endif
        if(!mercedes_to_storage(&hi, &lo)) {
#if MERCEDES_DEBUG
            uint64_t s_hi = instance->hi;
            uint64_t s_lo = instance->lo;
            uint64_t i_hi = instance->hi;
            uint64_t i_lo = instance->lo;
            mercedes_invert(&i_hi, &i_lo);
            if(!mercedes_get_bit(s_hi, s_lo, 0)) mercedes_invert(&s_hi, &s_lo);
            if(!mercedes_get_bit(i_hi, i_lo, 0)) mercedes_invert(&i_hi, &i_lo);
            instance->dbg.rej_total++;
            FURI_LOG_I(
                TAG,
                "rejected %016llX %016llX | sliced %s chipinv %s",
                (unsigned long long)instance->hi,
                (unsigned long long)instance->lo,
                mercedes_fields_fail(s_hi, s_lo),
                mercedes_fields_fail(i_hi, i_lo));
#endif
            mercedes_reset_soft(instance);
            return;
        }
    }

    instance->generic.data = hi;
    instance->generic.data_2 = lo;
    instance->generic.data_count_bit = MERCEDES_PAYLOAD_BITS;
    mercedes_parse_fields(&instance->generic);
#if MERCEDES_DEBUG
    instance->dbg.ok_total++;
    instance->dbg.data_best = MERCEDES_PAYLOAD_BITS;
    FURI_LOG_I(
        TAG,
        "ok %s Sn:%08lX Btn:%s Rpt:%lu Cnt:%lu Hop:%014llX",
        how,
        (unsigned long)instance->generic.serial,
        mercedes_btn_name(instance->generic.btn),
        (unsigned long)instance->generic.extra,
        (unsigned long)instance->generic.cnt,
        (unsigned long long)instance->generic.data_3);
#endif
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(instance->generic.btn);
        subghz_custom_btn_set_max(1);
    }
    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    mercedes_reset_soft(instance);
}

static void mercedes_add_logical_bit(SubGhzProtocolDecoderMERCEDES* instance, bool bit) {
    furi_assert(instance->nbits < MERCEDES_PAYLOAD_BITS);
    mercedes_set_bit(&instance->hi, &instance->lo, instance->nbits, bit);
    instance->nbits++;
    instance->decoder.decode_count_bit = instance->nbits;
    if(instance->nbits == MERCEDES_PAYLOAD_BITS) {
        mercedes_try_callback(instance);
    }
}

static void mercedes_add_chip(SubGhzProtocolDecoderMERCEDES* instance, bool chip) {
    if(instance->pair_chip < 0) {
        instance->pair_chip = chip ? 1 : 0;
        return;
    }
    const uint8_t first = (uint8_t)instance->pair_chip;
    const uint8_t second = chip ? 1 : 0;
    instance->pair_chip = -1;
    if(first == 1 && second == 0) {
        mercedes_add_logical_bit(instance, true);
    } else if(first == 0 && second == 1) {
        mercedes_add_logical_bit(instance, false);
    } else {
#if MERCEDES_DEBUG
        if(instance->nbits >= 16) {
            FURI_LOG_I(TAG, "manchester %u%u at bit %u", first, second, instance->nbits);
        }
#endif
        mercedes_reset_soft(instance);
    }
}

static void mercedes_enter_data(SubGhzProtocolDecoderMERCEDES* instance, bool level) {
#if MERCEDES_DEBUG && MERCEDES_DEBUG_BURST
    instance->dbg.snap[0] = instance->dbg.last_edge;
    instance->dbg.snap_n = 1;
#endif
    instance->decoder.parser_step = MERCEDESDecoderStepData;
    instance->hi = 0;
    instance->lo = 0;
    instance->nbits = 0;
    instance->decoder.decode_count_bit = 0;
    instance->pair_chip = level ? 1 : 0;
}

void subghz_protocol_decoder_mercedes_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderMERCEDES* instance = context;

    if(duration >= MERCEDES_GAP_US) {
#if MERCEDES_DEBUG
        instance->dbg.gap_us = duration;
        if(instance->decoder.parser_step == MERCEDESDecoderStepData && instance->nbits &&
           instance->nbits != MERCEDES_PAYLOAD_BITS) {
            FURI_LOG_I(
                TAG,
                "short %u/%u pre %u",
                instance->nbits,
                MERCEDES_PAYLOAD_BITS,
                instance->preamble_count);
        }
#endif
#if MERCEDES_DEBUG && MERCEDES_DEBUG_BURST
        mercedes_dbg_summary(instance);
#endif
        mercedes_reset_soft(instance);
        return;
    }

    const uint8_t n = mercedes_chips(duration);
#if MERCEDES_DEBUG && MERCEDES_DEBUG_BURST
    mercedes_dbg_edge(instance, level, duration, n);
#endif
    if(n == 0) {
        if(instance->decoder.parser_step != MERCEDESDecoderStepReset) {
            mercedes_reset_soft(instance);
        }
        return;
    }

    switch(instance->decoder.parser_step) {
    case MERCEDESDecoderStepReset:
        if(level && n == 1) {
            instance->preamble_count = 0;
            instance->decoder.parser_step = MERCEDESDecoderStepPreambleLow;
        } else if(!level && n == 1) {
            instance->preamble_count = 0;
            instance->decoder.parser_step = MERCEDESDecoderStepPreambleHigh;
        }
        break;

    case MERCEDESDecoderStepPreambleLow:
        if(!level && n == 1) {
            instance->preamble_count++;
            instance->decoder.parser_step = MERCEDESDecoderStepPreambleHigh;
        } else if(!level && n == 2 && instance->preamble_count >= MERCEDES_PREAMBLE_MIN) {
            mercedes_enter_data(instance, false);
        } else {
            mercedes_reset_soft(instance);
        }
        break;

    case MERCEDESDecoderStepPreambleHigh:
        if(level && n == 1) {
            instance->decoder.parser_step = MERCEDESDecoderStepPreambleLow;
        } else if(level && n == 2 && instance->preamble_count >= MERCEDES_PREAMBLE_MIN) {
            mercedes_enter_data(instance, true);
        } else {
            mercedes_reset_soft(instance);
        }
        break;

    case MERCEDESDecoderStepData:
        for(uint8_t i = 0; i < n; i++) {
            if(instance->decoder.parser_step != MERCEDESDecoderStepData) {
                break;
            }
            mercedes_add_chip(instance, level);
        }
        break;

    default:
        mercedes_reset_soft(instance);
        break;
    }

    instance->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_mercedes_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMERCEDES* instance = context;
    uint64_t mix = instance->generic.data ^ instance->generic.data_2 ^ instance->generic.data_3;
    return (uint8_t)(mix ^ (mix >> 8) ^ (mix >> 16) ^ (mix >> 24) ^ (mix >> 32) ^ (mix >> 40));
}

SubGhzProtocolStatus subghz_protocol_decoder_mercedes_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderMERCEDES* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mercedes_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMERCEDES* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret == SubGhzProtocolStatusOk) {
        if(instance->generic.data_count_bit == MERCEDES_PAYLOAD_BITS) {
            mercedes_parse_fields(&instance->generic);
        } else {
            ret = SubGhzProtocolStatusErrorValueBitCount;
        }
    }
    return ret;
}

void subghz_protocol_decoder_mercedes_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMERCEDES* instance = context;
    mercedes_parse_fields(&instance->generic);

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_length_bit = 1;
    subghz_block_generic_global.cnt_length_bit = MERCEDES_CNT_BITS;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    if(!subghz_custom_btn_has_original()) {
        subghz_custom_btn_set_original(instance->generic.btn);
    }
    subghz_custom_btn_set_max(1);

    furi_string_cat_printf(
        output,
        "%s 122bit\r\n"
        "Sn:%08lX\r\n"
        "Btn:%s Rpt:%lu Cnt:%lu\r\n"
        "Hop:%014llX",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.serial,
        mercedes_btn_name(instance->generic.btn),
        (unsigned long)instance->generic.extra,
        (unsigned long)instance->generic.cnt,
        (unsigned long long)instance->generic.data_3);
}
