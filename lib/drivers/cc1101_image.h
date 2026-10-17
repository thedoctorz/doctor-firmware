#pragma once

#include "cc1101.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CC1101_IMAGE_REG_COUNT 0x2F

#define CC1101_IMAGE_PROG_MAX (CC1101_IMAGE_REG_COUNT * 2)

typedef struct {
    uint8_t reg[CC1101_IMAGE_REG_COUNT];
} CC1101Image;

bool cc1101_image_reg_is_owned(uint8_t reg);

void cc1101_image_read(const FuriHalSpiBusHandle* handle, CC1101Image* out);

void cc1101_image_apply_preset(
    const CC1101Image* base,
    const uint8_t* preset_data,
    CC1101Image* out);

size_t cc1101_image_diff(
    const CC1101Image* from,
    const CC1101Image* to,
    uint8_t* prog,
    size_t prog_cap);

void cc1101_image_write_prog(const FuriHalSpiBusHandle* handle, const uint8_t* prog, size_t len);

#ifdef __cplusplus
}
#endif
