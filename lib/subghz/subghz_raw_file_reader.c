#include "subghz_raw_file_reader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <flipper_format/flipper_format_i.h>
#include <lib/subghz/types.h>

#define TAG "SubGhzRawFileReader"

#define RAW_READER_MAX_TOKEN_LEN 64U
#define RAW_READER_KEY           "RAW_Data:"

static const char local_flipper_format_delimiter = ':';
static const char local_flipper_format_comment = '#';
static const char local_flipper_format_eoln = '\n';
static const char local_flipper_format_eolr = '\r';

SubGhzRawFileReader* subghz_raw_file_reader_alloc(void) {
    SubGhzRawFileReader* reader = malloc(sizeof(SubGhzRawFileReader));
    if(!reader) return NULL;
    memset(reader, 0, sizeof(SubGhzRawFileReader));
    return reader;
}

void subghz_raw_file_reader_free(SubGhzRawFileReader* reader) {
    if(!reader) return;
    subghz_raw_file_reader_close(reader);
    free(reader);
}

static bool local_flipper_format_stream_read_valid_key(Stream* stream, FuriString* key) {
    furi_string_reset(key);
    const size_t buffer_size = 32;
    uint8_t buffer[buffer_size];

    bool found = false;
    bool error = false;
    bool accumulate = true;
    bool new_line = true;

    while(true) {
        size_t was_read = stream_read(stream, buffer, buffer_size);
        if(was_read == 0) break;

        for(size_t i = 0; i < was_read; i++) {
            uint8_t data = buffer[i];
            if(data == local_flipper_format_eoln) {
                furi_string_reset(key);
                accumulate = true;
                new_line = true;
            } else if(data == local_flipper_format_eolr) {
                // ignore
            } else if(data == local_flipper_format_comment && new_line) {
                accumulate = false;
                new_line = false;
            } else if(data == local_flipper_format_delimiter) {
                if(new_line) {
                    furi_string_reset(key);
                    accumulate = false;
                    new_line = false;
                } else {
                    if(accumulate) {
                        if(!stream_seek(
                               stream, (int32_t)i - (int32_t)was_read, StreamOffsetFromCurrent)) {
                            error = true;
                            break;
                        }

                        found = true;
                        break;
                    }
                }
            } else {
                new_line = false;
                if(accumulate) {
                    if(furi_string_size(key) >= RAW_READER_MAX_TOKEN_LEN) {
                        accumulate = false;
                    } else {
                        furi_string_push_back(key, data);
                    }
                }
            }
        }

        if(found || error) break;
    }

    return found;
}

static bool
    local_flipper_format_stream_seek_to_key(Stream* stream, const char* key, bool strict_mode) {
    bool found = false;
    FuriString* read_key = furi_string_alloc();

    while(!stream_eof(stream)) {
        if(local_flipper_format_stream_read_valid_key(stream, read_key)) {
            if(furi_string_cmp_str(read_key, key) == 0) {
                if(!stream_seek(stream, 2, StreamOffsetFromCurrent)) break;

                found = true;
                break;
            } else if(strict_mode) {
                found = false;
                break;
            }
        }
    }
    furi_string_free(read_key);

    return found;
}

static bool subghz_raw_file_reader_stream_read_char(SubGhzRawFileReader* reader, char* out) {
    if(!reader || !reader->stream || !out) {
        return false;
    }

    if(reader->buffer_index >= reader->buffer_count) {
        const size_t was_read =
            stream_read(reader->stream, reader->buffer, SUBGHZ_RAW_FILE_READER_BUFFER_SIZE);
        if(was_read == 0U) {
            return false;
        }

        reader->buffer_count = (uint16_t)was_read;
        reader->buffer_index = 0U;
    }

    const uint8_t value = reader->buffer[reader->buffer_index++];
    if(reader->stream_pos < UINT64_MAX) {
        reader->stream_pos++;
    }
    *out = (char)value;
    return true;
}

static bool subghz_raw_file_reader_at_logical_eof(const SubGhzRawFileReader* reader) {
    return reader && reader->stream && reader->buffer_index >= reader->buffer_count &&
           stream_eof(reader->stream);
}

static void subghz_raw_file_reader_mark_finished(SubGhzRawFileReader* reader) {
    reader->file_finished = true;
    reader->stream_pos = reader->file_size > 0U ? reader->file_size : reader->stream_pos;
}

static bool subghz_raw_file_reader_seek_next_values(SubGhzRawFileReader* reader) {
    const char* key = RAW_READER_KEY;
    size_t match = 0U;
    char c = '\0';

    while(subghz_raw_file_reader_stream_read_char(reader, &c)) {
        if(c == key[match]) {
            match++;
            if(key[match] == '\0') {
                reader->in_line = true;
                return true;
            }
        } else {
            match = (c == key[0]) ? 1U : 0U;
        }
    }

    subghz_raw_file_reader_mark_finished(reader);
    return false;
}

static bool subghz_raw_file_reader_stream_next_int(SubGhzRawFileReader* reader, int32_t* out) {
    if(!reader || !out || !reader->stream) return false;

    while(true) {
        if(!reader->in_line && !subghz_raw_file_reader_seek_next_values(reader)) {
            return false;
        }

        bool negative = false;
        bool sign_seen = false;
        bool have_digits = false;
        int32_t value = 0;
        char c = '\0';

        while(subghz_raw_file_reader_stream_read_char(reader, &c)) {
            if(c == '\r') {
                continue;
            }

            if(c == '\n') {
                reader->in_line = false;
                if(have_digits) {
                    *out = negative ? -value : value;
                    return true;
                }
                break;
            }

            if(!sign_seen && !have_digits && (c == ' ' || c == '\t' || c == ',')) {
                continue;
            }

            if(!sign_seen && !have_digits && (c == '-' || c == '+')) {
                negative = (c == '-');
                sign_seen = true;
                continue;
            }

            if(c >= '0' && c <= '9') {
                have_digits = true;
                value = (value * 10) + (c - '0');
                continue;
            }

            if(have_digits) {
                *out = negative ? -value : value;
                return true;
            }

            negative = false;
            sign_seen = false;
        }

        if(have_digits) {
            subghz_raw_file_reader_mark_finished(reader);
            *out = negative ? -value : value;
            return true;
        }

        if(subghz_raw_file_reader_at_logical_eof(reader)) {
            subghz_raw_file_reader_mark_finished(reader);
            return false;
        }
    }
}

bool subghz_raw_file_reader_open(SubGhzRawFileReader* reader, const char* file_path) {
    if(!reader || !file_path) return false;

    subghz_raw_file_reader_close(reader);

    reader->storage = furi_record_open(RECORD_STORAGE);
    reader->storage_opened = true;
    reader->ff = flipper_format_file_alloc(reader->storage);

    if(!flipper_format_file_open_existing(reader->ff, file_path)) {
        FURI_LOG_E(TAG, "Failed to open file: %s", file_path);
        subghz_raw_file_reader_close(reader);
        return false;
    }

    FuriString* temp_str = furi_string_alloc();
    uint32_t version = 0;

    bool valid = false;
    do {
        if(!flipper_format_read_header(reader->ff, temp_str, &version)) {
            FURI_LOG_E(TAG, "Failed to read header");
            break;
        }

        if(furi_string_cmp_str(temp_str, SUBGHZ_RAW_FILE_TYPE) != 0 ||
           version != SUBGHZ_RAW_FILE_VERSION) {
            FURI_LOG_E(TAG, "Not a RAW file");
            break;
        }

        if(!flipper_format_read_string(reader->ff, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol field");
            break;
        }

        if(furi_string_cmp_str(temp_str, "RAW") != 0) {
            FURI_LOG_E(TAG, "Protocol is not RAW");
            break;
        }

        valid = true;
    } while(false);

    furi_string_free(temp_str);

    if(!valid) {
        subghz_raw_file_reader_close(reader);
        return false;
    }

    reader->buffer_count = 0;
    reader->buffer_index = 0;
    reader->file_finished = false;
    reader->in_line = false;

    reader->stream = flipper_format_get_raw_stream(reader->ff);
    if(!reader->stream) {
        FURI_LOG_E(TAG, "Missing raw stream");
        subghz_raw_file_reader_close(reader);
        return false;
    }

    if(!local_flipper_format_stream_seek_to_key(reader->stream, "RAW_Data", false)) {
        FURI_LOG_E(TAG, "RAW file has no samples");
        subghz_raw_file_reader_close(reader);
        return false;
    }
    reader->in_line = true;
    reader->data_start_offset = stream_tell(reader->stream);
    reader->stream_pos = reader->data_start_offset;
    reader->file_size = stream_size(reader->stream);
    if(reader->file_size == 0) {
        FileInfo file_info = {0};
        if(storage_common_stat(reader->storage, file_path, &file_info) == FSE_OK) {
            reader->file_size = file_info.size;
        }
    }

    FURI_LOG_I(TAG, "Opened RAW file for streaming decode: %s", file_path);
    return true;
}

void subghz_raw_file_reader_close(SubGhzRawFileReader* reader) {
    if(!reader) return;

    reader->stream = NULL;

    if(reader->ff) {
        flipper_format_free(reader->ff);
        reader->ff = NULL;
    }

    if(reader->storage_opened) {
        furi_record_close(RECORD_STORAGE);
        reader->storage_opened = false;
    }

    reader->storage = NULL;
    reader->buffer_count = 0;
    reader->buffer_index = 0;
    reader->in_line = false;
    reader->file_finished = false;
    reader->file_size = 0;
    reader->data_start_offset = 0;
    reader->stream_pos = 0;
}

bool subghz_raw_file_reader_get_next(SubGhzRawFileReader* reader, bool* level, uint32_t* duration) {
    if(!reader || !level || !duration) return false;

    if(reader->buffer_index >= reader->buffer_count && memmgr_get_free_heap() < 1024) {
        FURI_LOG_E(TAG, "Not enough memory to continue reading");
        return false;
    }

    int32_t value = 0;
    if(!subghz_raw_file_reader_stream_next_int(reader, &value)) {
        return false;
    }

    if(value >= 0) {
        *level = true;
        *duration = (uint32_t)value;
    } else {
        *level = false;
        *duration = (uint32_t)(-value);
    }

    return true;
}

bool subghz_raw_file_reader_is_finished(SubGhzRawFileReader* reader) {
    if(!reader) return true;
    return reader->file_finished && reader->buffer_index >= reader->buffer_count;
}

uint8_t subghz_raw_file_reader_get_progress(const SubGhzRawFileReader* reader) {
    if(!reader) return 0;
    if(reader->file_finished && reader->buffer_index >= reader->buffer_count) return 100;
    if(reader->file_size <= reader->data_start_offset) return 0;

    const uint64_t total = reader->file_size - reader->data_start_offset;
    const uint64_t current_pos = reader->stream_pos;
    uint64_t done = 0;
    if(current_pos > reader->data_start_offset) {
        done = current_pos - reader->data_start_offset;
    }
    if(done >= total) return 100;

    return (uint8_t)((done * 100ULL) / total);
}
