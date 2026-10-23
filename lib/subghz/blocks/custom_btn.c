#include "custom_btn_i.h"
#include "generic.h"
#include <furi.h>
#include <string.h>

static uint8_t custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
static uint8_t custom_btn_original = 0;
static bool custom_btn_original_set = false;
static uint8_t custom_btn_max_btns = 0;
static uint8_t controller_programming_mode = PROG_MODE_OFF;
static SubGhzCustomButtonMapItem custom_btn_map[SUBGHZ_CUSTOM_BTN_COUNT] = {0};

static bool subghz_custom_btn_protocol_is_kia_v3_v4(const char* protocol_name) {
    return !strcmp(protocol_name, "Kia V3/V4") || !strcmp(protocol_name, "Kia V3") ||
           !strcmp(protocol_name, "Kia V4");
}

static void subghz_custom_btn_map_set(uint8_t btn_id, uint8_t code, const char* label) {
    if(btn_id >= SUBGHZ_CUSTOM_BTN_COUNT || btn_id > custom_btn_max_btns || !label) return;

    custom_btn_map[btn_id].enabled = true;
    custom_btn_map[btn_id].code = code;
    strlcpy(custom_btn_map[btn_id].label, label, sizeof(custom_btn_map[btn_id].label));
}

static void subghz_custom_btn_map_set_default_kia(void) {
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x03, "Boot");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x04, "Panic");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x08, "Aux");
}

static void subghz_custom_btn_map_set_ford_other(void) {
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x01, "Panic");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x02, "Lock");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x04, "Unlock");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x08, "Boot");
    subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x10, "Aux");
}

bool subghz_custom_btn_set(uint8_t btn_id) {
    if(btn_id > custom_btn_max_btns) {
        custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
        return false;
    } else {
        custom_btn_id = btn_id;
        return true;
    }
}

uint8_t subghz_custom_btn_get(void) {
    return custom_btn_id;
}

void subghz_custom_btn_set_original(uint8_t btn_code) {
    custom_btn_original = btn_code;
    custom_btn_original_set = true;
}

uint8_t subghz_custom_btn_get_original(void) {
    return custom_btn_original;
}

bool subghz_custom_btn_has_original(void) {
    return custom_btn_original_set;
}

void subghz_custom_btn_set_max(uint8_t b) {
    custom_btn_max_btns = b;
}

void subghz_custom_btns_reset(void) {
    custom_btn_original = 0;
    custom_btn_original_set = false;
    custom_btn_max_btns = 0;
    controller_programming_mode = PROG_MODE_OFF;
    custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
    subghz_custom_btn_map_reset();
}

bool subghz_custom_btn_is_allowed(void) {
    return custom_btn_max_btns != 0;
}

void subghz_custom_btn_set_prog_mode(ProgMode prog_mode) {
    controller_programming_mode = prog_mode;
}

ProgMode subghz_custom_btn_get_prog_mode(void) {
    return controller_programming_mode;
}

void subghz_custom_btn_map_reset(void) {
    memset(custom_btn_map, 0, sizeof(custom_btn_map));
}

bool subghz_custom_btn_map_is_available(void) {
    for(uint8_t i = 0; i < SUBGHZ_CUSTOM_BTN_COUNT; i++) {
        if(custom_btn_map[i].enabled) return true;
    }
    return false;
}

bool subghz_custom_btn_map_get(uint8_t btn_id, SubGhzCustomButtonMapItem* item) {
    if(btn_id >= SUBGHZ_CUSTOM_BTN_COUNT || !custom_btn_map[btn_id].enabled) return false;
    if(item) *item = custom_btn_map[btn_id];
    return true;
}

void subghz_custom_btn_configure_map(const char* protocol_name, uint32_t type, uint32_t flags) {
    UNUSED(flags);
    subghz_custom_btn_map_reset();
    if(!protocol_name || !subghz_custom_btn_is_allowed()) return;

    if(subghz_block_generic_global.hitag2_replay_only || !strcmp(protocol_name, "Fiat V2")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x00, "Replay");
        return;
    }

    if(!strcmp(protocol_name, "Datsun Lada")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, custom_btn_original, "OG");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x40, "BTN0");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x20, "BTN3");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x60, "BTN5");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x80, "BTN1");
    } else if(!strcmp(protocol_name, "Kia V7")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x03, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x08, "Panic");
    } else if(subghz_custom_btn_protocol_is_kia_v3_v4(protocol_name)) {
        subghz_custom_btn_map_set_default_kia();
    } else if(strstr(protocol_name, "Kia")) {
        if(!strcmp(protocol_name, "Kia V0") && type == 2U) {
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x03, "Lock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x04, "Unlock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x02, "Boot");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x01, "Panic");
        } else if(!strcmp(protocol_name, "Kia V0") && type == 3U) {
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x03, "Boot");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x04, "Panic");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x05, "Aux");
        } else {
            subghz_custom_btn_map_set_default_kia();
        }
    } else if(strstr(protocol_name, "VAG")) {
        if(custom_btn_original == 0x10U || custom_btn_original == 0x20U ||
           custom_btn_original == 0x40U) {
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x20, "Lock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x10, "Unlock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x40, "Boot");
        } else {
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x02, "Lock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x01, "Unlock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x04, "Boot");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x08, "Panic");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x03, "L+U");
        }
    } else if(!strcmp(protocol_name, "Renault V0")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x06, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x0A, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x05, "Trunk");
    } else if(!strcmp(protocol_name, "Honda V1")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x08, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x00, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x09, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x0A, "Panic");
    } else if(!strcmp(protocol_name, "Honda Static")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x04, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x08, "Panic");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x05, "Start");
    } else if(!strcmp(protocol_name, "Honda V2")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x02, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x04, "Unlock");
    } else if(!strcmp(protocol_name, "Mazda V2")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x04, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x08, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x10, "Trunk");
    } else if(strstr(protocol_name, "Mazda")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x04, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x08, "Remote");
    } else if(!strcmp(protocol_name, "PSA")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x04, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x01, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x02, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x08, "Panic");
    } else if(strstr(protocol_name, "Ford")) {
        if(!strcmp(protocol_name, "Ford V1")) {
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x04, "Trunk");
            subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x08, "Panic");
        } else {
            subghz_custom_btn_map_set_ford_other();
        }
    } else if(!strcmp(protocol_name, "BMW")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x02, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x01, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x03, "Trunk");
    } else if(!strcmp(protocol_name, "Mercedes V0")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x0C, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x08, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x0A, "??");
    } else if(!strcmp(protocol_name, "Mercedes V1")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x00, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x01, "Unlock");
    } else if(strstr(protocol_name, "Subaru")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x00, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x01, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x02, "Boot");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x03, "Panic");
    } else if(strstr(protocol_name, "Chrysler")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x02, "Unlock");
    } else if(!strcmp(protocol_name, "Fiat V1")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x04, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x08, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x02, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x01, "Close");
    } else if(!strcmp(protocol_name, "Renault V1")) {
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, 0x00, "Sync");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "Lock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x02, "Unlock");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x04, "Trunk");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x08, "Panic");
    } else if(!strcmp(protocol_name, "Toyota Lexus")) {
        const uint8_t command =
            (custom_btn_original >= 1U && custom_btn_original <= 3U) ? custom_btn_original : 1U;
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_OK, command, "OG");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_UP, 0x01, "BTN1");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_DOWN, 0x02, "BT2");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_LEFT, 0x03, "BT3");
        subghz_custom_btn_map_set(SUBGHZ_CUSTOM_BTN_RIGHT, 0x03, "BT3");
    }
}
