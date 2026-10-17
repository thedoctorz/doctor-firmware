#include "cc1101_image.h"

#include <string.h>

bool cc1101_image_reg_is_owned(uint8_t reg) {
    switch(reg) {
    case CC1101_IOCFG2: /* antenna path switch */
    case CC1101_FREQ2:
    case CC1101_FREQ1:
    case CC1101_FREQ0:
    case CC1101_FSCAL3:
    case CC1101_FSCAL2:
    case CC1101_FSCAL1:
        return true;
    default:
        return false;
    }
}

void cc1101_image_read(const FuriHalSpiBusHandle* handle, CC1101Image* out) {
    for(uint8_t reg = 0; reg < CC1101_IMAGE_REG_COUNT; reg++) {
        cc1101_read_reg(handle, reg, &out->reg[reg]);
    }
}

void cc1101_image_apply_preset(
    const CC1101Image* base,
    const uint8_t* preset_data,
    CC1101Image* out) {
    if(out != base) {
        memcpy(out, base, sizeof(CC1101Image));
    }

    size_t i = 0;
    while(preset_data[i]) {
        if(preset_data[i] < CC1101_IMAGE_REG_COUNT) {
            out->reg[preset_data[i]] = preset_data[i + 1];
        }
        i += 2;
    }
}

size_t cc1101_image_diff(
    const CC1101Image* from,
    const CC1101Image* to,
    uint8_t* prog,
    size_t prog_cap) {
    size_t len = 0;

    for(uint8_t reg = 0; reg < CC1101_IMAGE_REG_COUNT; reg++) {
        if(from->reg[reg] == to->reg[reg]) continue;
        if(cc1101_image_reg_is_owned(reg)) continue;
        if(len + 2 > prog_cap) break;

        prog[len++] = reg;
        prog[len++] = to->reg[reg];
    }

    return len;
}

void cc1101_image_write_prog(const FuriHalSpiBusHandle* handle, const uint8_t* prog, size_t len) {
    for(size_t i = 0; i + 1 < len; i += 2) {
        cc1101_write_reg(handle, prog[i], prog[i + 1]);
    }
}
