#pragma once

#include <furi.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Gunzip Gunzip;

typedef void (*GunzipProgressCb)(uint8_t percent, void* context);

Gunzip* gunzip_alloc(Storage* storage);

void gunzip_free(Gunzip* gunzip);

bool gunzip_file(
    Gunzip* gunzip,
    const char* source_path,
    const char* dest_path,
    GunzipProgressCb progress_cb,
    void* context);

const char* gunzip_last_error(Gunzip* gunzip);

#ifdef __cplusplus
}
#endif
