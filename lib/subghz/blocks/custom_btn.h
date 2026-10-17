#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default btn ID
#define SUBGHZ_CUSTOM_BTN_OK    (0U)
#define SUBGHZ_CUSTOM_BTN_UP    (1U)
#define SUBGHZ_CUSTOM_BTN_DOWN  (2U)
#define SUBGHZ_CUSTOM_BTN_LEFT  (3U)
#define SUBGHZ_CUSTOM_BTN_RIGHT (4U)

#define SUBGHZ_CUSTOM_BTN_COUNT         (5U)
#define SUBGHZ_CUSTOM_BTN_LABEL_SIZE    (10U)
#define SUBGHZ_CUSTOM_BTN_PROTOCOL_NONE (0U)
#define SUBGHZ_CUSTOM_BTN_PROTOCOL_ROLL (1U << 0)

typedef struct {
    bool enabled;
    uint8_t code;
    char label[SUBGHZ_CUSTOM_BTN_LABEL_SIZE];
} SubGhzCustomButtonMapItem;

bool subghz_custom_btn_set(uint8_t btn_id);

uint8_t subghz_custom_btn_get(void);

uint8_t subghz_custom_btn_get_original(void);

bool subghz_custom_btn_has_original(void);

void subghz_custom_btns_reset(void);

bool subghz_custom_btn_is_allowed(void);

void subghz_custom_btn_map_reset(void);

bool subghz_custom_btn_map_is_available(void);

bool subghz_custom_btn_map_get(uint8_t btn_id, SubGhzCustomButtonMapItem* item);

void subghz_custom_btn_configure_map(const char* protocol_name, uint32_t type, uint32_t flags);

#ifdef __cplusplus
}
#endif
