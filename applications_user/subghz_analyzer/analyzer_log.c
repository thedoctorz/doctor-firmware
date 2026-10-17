#include "analyzer_log.h"

#include <furi_hal.h>
#include <storage/storage.h>
#include <lib/subghz/types.h>

#include <stdarg.h>

#define TAG "SubGhzAnalyzerLog"

#define ANALYZER_LOG_FOLDER      EXT_PATH("subghz/analyzer")
#define ANALYZER_LOG_PATH_LEN    96u
#define ANALYZER_LOG_FLUSH_BYTES 512u

struct AnalyzerLog {
    Storage* storage;
    File* file;
    FuriString* buffer;
    char path[ANALYZER_LOG_PATH_LEN];
    bool open;
};

AnalyzerLog* analyzer_log_alloc(void) {
    AnalyzerLog* instance = malloc(sizeof(AnalyzerLog));
    instance->storage = NULL;
    instance->file = NULL;
    instance->buffer = furi_string_alloc();
    instance->path[0] = '\0';
    instance->open = false;
    return instance;
}

void analyzer_log_free(AnalyzerLog* instance) {
    furi_check(instance);
    analyzer_log_close(instance);
    furi_string_free(instance->buffer);
    free(instance);
}

static void analyzer_log_build_path(AnalyzerLog* instance, uint32_t frequency) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);

    snprintf(
        instance->path,
        sizeof(instance->path),
        "%s/%04u%02u%02u_%02u%02u%02u_%lu-%03lu.txt",
        ANALYZER_LOG_FOLDER,
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second,
        (unsigned long)(frequency / 1000000UL),
        (unsigned long)((frequency % 1000000UL) / 1000UL));
}

bool analyzer_log_open(AnalyzerLog* instance, uint32_t frequency) {
    furi_check(instance);
    if(instance->open) return true;

    instance->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(instance->storage, SUBGHZ_APP_FOLDER);
    storage_simply_mkdir(instance->storage, ANALYZER_LOG_FOLDER);

    analyzer_log_build_path(instance, frequency);

    instance->file = storage_file_alloc(instance->storage);
    if(!storage_file_open(instance->file, instance->path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "Cannot create %s", instance->path);
        storage_file_free(instance->file);
        instance->file = NULL;
        furi_record_close(RECORD_STORAGE);
        instance->storage = NULL;
        instance->path[0] = '\0';
        return false;
    }

    instance->open = true;
    furi_string_reset(instance->buffer);
    return true;
}

void analyzer_log_flush(AnalyzerLog* instance) {
    furi_check(instance);
    if(!instance->open) return;

    const size_t len = furi_string_size(instance->buffer);
    if(len == 0) return;

    storage_file_write(instance->file, furi_string_get_cstr(instance->buffer), len);
    furi_string_reset(instance->buffer);
}

static void analyzer_log_maybe_flush(AnalyzerLog* instance) {
    if(furi_string_size(instance->buffer) >= ANALYZER_LOG_FLUSH_BYTES) {
        analyzer_log_flush(instance);
    }
}

void analyzer_log_printf(AnalyzerLog* instance, const char* format, ...) {
    furi_check(instance);
    if(!instance->open) return;

    va_list args;
    va_start(args, format);
    furi_string_cat_vprintf(instance->buffer, format, args);
    va_end(args);
    furi_string_push_back(instance->buffer, '\n');

    analyzer_log_maybe_flush(instance);
}

void analyzer_log_newline(AnalyzerLog* instance) {
    furi_check(instance);
    if(!instance->open) return;

    furi_string_push_back(instance->buffer, '\n');
    analyzer_log_maybe_flush(instance);
}

void analyzer_log_hex(AnalyzerLog* instance, const char* label, const uint8_t* data, size_t len) {
    furi_check(instance);
    if(!instance->open || data == NULL) return;

    furi_string_cat_printf(instance->buffer, "%s", label);
    for(size_t i = 0; i < len; i++) {
        furi_string_cat_printf(instance->buffer, " %02X", data[i]);
    }
    furi_string_push_back(instance->buffer, '\n');

    analyzer_log_maybe_flush(instance);
}

void analyzer_log_close(AnalyzerLog* instance) {
    furi_check(instance);
    if(!instance->open) return;

    analyzer_log_flush(instance);
    instance->open = false;

    storage_file_close(instance->file);
    storage_file_free(instance->file);
    instance->file = NULL;

    furi_record_close(RECORD_STORAGE);
    instance->storage = NULL;
}

bool analyzer_log_is_open(const AnalyzerLog* instance) {
    furi_check(instance);
    return instance->open;
}

const char* analyzer_log_path(const AnalyzerLog* instance) {
    furi_check(instance);
    return instance->path;
}
