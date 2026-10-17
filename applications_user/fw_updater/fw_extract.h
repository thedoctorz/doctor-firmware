#pragma once

#include <furi.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FwExtractStageDecompress,
    FwExtractStageUnpack,
} FwExtractStage;

typedef void (*FwExtractProgressCb)(FwExtractStage stage, uint8_t percent, void* context);

bool fw_extract_archive(
    Storage* storage,
    const char* archive_path,
    FwExtractProgressCb progress_cb,
    void* context,
    FuriString* error_out);

#ifdef __cplusplus
}
#endif
