#pragma once

#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <toolbox/stream/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUBGHZ_RAW_FILE_READER_BUFFER_SIZE 2048U

typedef struct SubGhzRawFileReader SubGhzRawFileReader;

SubGhzRawFileReader* subghz_raw_file_reader_alloc(void);
void subghz_raw_file_reader_free(SubGhzRawFileReader* reader);
bool subghz_raw_file_reader_open(SubGhzRawFileReader* reader, const char* file_path);
void subghz_raw_file_reader_close(SubGhzRawFileReader* reader);
bool subghz_raw_file_reader_get_next(SubGhzRawFileReader* reader, bool* level, uint32_t* duration);
bool subghz_raw_file_reader_is_finished(SubGhzRawFileReader* reader);
uint8_t subghz_raw_file_reader_get_progress(const SubGhzRawFileReader* reader);

struct SubGhzRawFileReader {
    Storage* storage;
    FlipperFormat* ff;
    Stream* stream;
    uint8_t buffer[SUBGHZ_RAW_FILE_READER_BUFFER_SIZE];
    uint16_t buffer_count;
    uint16_t buffer_index;
    bool in_line;
    bool file_finished;
    bool storage_opened;
    uint64_t file_size;
    uint64_t data_start_offset;
    uint64_t stream_pos;
};

#ifdef __cplusplus
}
#endif
