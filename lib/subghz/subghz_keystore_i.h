#pragma once

#include <m-array.h>

typedef struct SubGhzKeystoreNameChunk SubGhzKeystoreNameChunk;

struct SubGhzKeystore {
    SubGhzKeyArray_t data;
    SubGhzKeystoreNameChunk* names;
    const char* mfname;
    uint8_t kl_type;
};
