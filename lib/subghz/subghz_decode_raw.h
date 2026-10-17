#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SubGhzDecodeRawStatusOk = 0,
    SubGhzDecodeRawStatusErrorNotExist,
    SubGhzDecodeRawStatusErrorInvalid,
    SubGhzDecodeRawStatusErrorInternal,
    SubGhzDecodeRawStatusStopped,
} SubGhzDecodeRawStatus;

typedef struct {
    bool (*should_stop)(void* context);
    void (*on_progress)(uint8_t percent, uint32_t hit_count, uint32_t activity, void* context);
    void (*on_hit)(
        const char* protocol,
        const char* flipper_format,
        uint8_t percent,
        uint32_t hit_count,
        void* context);
} SubGhzDecodeRawCallbacks;

SubGhzDecodeRawStatus subghz_decode_raw_file(
    const char* path,
    const SubGhzDecodeRawCallbacks* callbacks,
    void* context);

#ifdef __cplusplus
}
#endif
