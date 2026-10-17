#include "gunzip.h"

#include <toolbox/crc32_calc.h>

#define TAG "Gunzip"

#define GZ_IN_BUFFER_SIZE 1024
#define GZ_WINDOW_SIZE    32768
#define GZ_WINDOW_MASK    (GZ_WINDOW_SIZE - 1)
#define GZ_WRITE_CHUNK    4096
#define GZ_MAX_CODE_LEN   15

#define GZ_FLAG_FHCRC    0x02
#define GZ_FLAG_FEXTRA   0x04
#define GZ_FLAG_FNAME    0x08
#define GZ_FLAG_FCOMMENT 0x10

#define GZ_SYMBOL_COUNT 288
#define GZ_DIST_COUNT   30

typedef struct {
    uint16_t counts[GZ_MAX_CODE_LEN + 1];
    uint16_t symbols[GZ_SYMBOL_COUNT];
} GzHuffman;

struct Gunzip {
    Storage* storage;
    File* input;
    File* output;

    uint8_t* in_buffer;
    size_t in_length;
    size_t in_position;
    uint64_t in_consumed;
    uint64_t in_size;

    uint32_t bit_buffer;
    uint8_t bit_count;

    uint8_t* window;
    size_t window_position;
    uint32_t crc;
    uint64_t out_total;

    GzHuffman* literal;
    GzHuffman* distance;

    const char* error;

    GunzipProgressCb progress_cb;
    void* progress_context;
    uint8_t last_percent;
};

static const uint16_t gz_length_base[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                            15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                            67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t gz_length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t gz_dist_base[GZ_DIST_COUNT] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t gz_dist_extra[GZ_DIST_COUNT] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                     4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                     9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/* Order the code length alphabet is transmitted in */
static const uint8_t gz_code_length_order[19] =
    {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

Gunzip* gunzip_alloc(Storage* storage) {
    Gunzip* gunzip = malloc(sizeof(Gunzip));
    gunzip->storage = storage;
    gunzip->input = storage_file_alloc(storage);
    gunzip->output = storage_file_alloc(storage);
    gunzip->in_buffer = malloc(GZ_IN_BUFFER_SIZE);
    gunzip->window = malloc(GZ_WINDOW_SIZE);
    gunzip->literal = malloc(sizeof(GzHuffman));
    gunzip->distance = malloc(sizeof(GzHuffman));
    gunzip->error = NULL;
    return gunzip;
}

void gunzip_free(Gunzip* gunzip) {
    furi_assert(gunzip);
    free(gunzip->distance);
    free(gunzip->literal);
    free(gunzip->window);
    free(gunzip->in_buffer);
    storage_file_free(gunzip->output);
    storage_file_free(gunzip->input);
    free(gunzip);
}

const char* gunzip_last_error(Gunzip* gunzip) {
    return gunzip->error ? gunzip->error : "OK";
}

static void gz_fail(Gunzip* gunzip, const char* reason) {
    if(!gunzip->error) {
        gunzip->error = reason;
    }
}

static int gz_next_byte(Gunzip* gunzip) {
    if(gunzip->in_position >= gunzip->in_length) {
        gunzip->in_length = storage_file_read(gunzip->input, gunzip->in_buffer, GZ_IN_BUFFER_SIZE);
        gunzip->in_position = 0;
        if(gunzip->in_length == 0) {
            gz_fail(gunzip, "Archive ends early");
            return -1;
        }
    }

    gunzip->in_consumed++;

    if(gunzip->progress_cb && gunzip->in_size) {
        const uint8_t percent = (uint8_t)((gunzip->in_consumed * 100) / gunzip->in_size);
        if(percent != gunzip->last_percent) {
            gunzip->last_percent = percent;
            gunzip->progress_cb(percent, gunzip->progress_context);
        }
    }

    return gunzip->in_buffer[gunzip->in_position++];
}

static uint32_t gz_read_bits(Gunzip* gunzip, uint8_t count) {
    while(gunzip->bit_count < count) {
        const int byte = gz_next_byte(gunzip);
        if(byte < 0) {
            return 0;
        }
        gunzip->bit_buffer |= (uint32_t)byte << gunzip->bit_count;
        gunzip->bit_count += 8;
    }

    const uint32_t value = gunzip->bit_buffer & ((1UL << count) - 1);
    gunzip->bit_buffer >>= count;
    gunzip->bit_count -= count;
    return value;
}

static int gz_read_aligned_byte(Gunzip* gunzip) {
    const uint8_t partial = gunzip->bit_count & 7;
    gunzip->bit_buffer >>= partial;
    gunzip->bit_count -= partial;

    if(gunzip->bit_count >= 8) {
        const int byte = (int)(gunzip->bit_buffer & 0xFF);
        gunzip->bit_buffer >>= 8;
        gunzip->bit_count -= 8;
        return byte;
    }

    return gz_next_byte(gunzip);
}

static bool gz_write_out(Gunzip* gunzip, const uint8_t* data, size_t size) {
    gunzip->crc = crc32_calc_buffer(gunzip->crc, data, size);

    size_t written = 0;
    while(written < size) {
        const size_t chunk = MIN((size_t)GZ_WRITE_CHUNK, size - written);
        if(storage_file_write(gunzip->output, data + written, chunk) != chunk) {
            gz_fail(gunzip, "SD card write failed");
            return false;
        }
        written += chunk;
    }

    return true;
}

static bool gz_emit(Gunzip* gunzip, uint8_t byte) {
    gunzip->window[gunzip->window_position] = byte;
    gunzip->window_position = (gunzip->window_position + 1) & GZ_WINDOW_MASK;
    gunzip->out_total++;

    if(gunzip->window_position == 0) {
        return gz_write_out(gunzip, gunzip->window, GZ_WINDOW_SIZE);
    }

    return true;
}

static bool gz_flush(Gunzip* gunzip) {
    if(gunzip->window_position == 0) {
        return true;
    }
    return gz_write_out(gunzip, gunzip->window, gunzip->window_position);
}

static void gz_huffman_build(GzHuffman* table, const uint8_t* lengths, size_t count) {
    memset(table->counts, 0, sizeof(table->counts));
    for(size_t i = 0; i < count; i++) {
        table->counts[lengths[i]]++;
    }
    table->counts[0] = 0;

    uint16_t offsets[GZ_MAX_CODE_LEN + 1];
    offsets[0] = 0;
    offsets[1] = 0;
    for(uint8_t length = 1; length < GZ_MAX_CODE_LEN; length++) {
        offsets[length + 1] = offsets[length] + table->counts[length];
    }

    for(size_t i = 0; i < count; i++) {
        if(lengths[i]) {
            table->symbols[offsets[lengths[i]]++] = (uint16_t)i;
        }
    }
}

static int gz_huffman_decode(Gunzip* gunzip, const GzHuffman* table) {
    int code = 0;
    int first = 0;
    int index = 0;

    for(uint8_t length = 1; length <= GZ_MAX_CODE_LEN; length++) {
        code |= (int)gz_read_bits(gunzip, 1);
        if(gunzip->error) {
            return -1;
        }

        const int count = table->counts[length];
        if((code - first) < count) {
            return table->symbols[index + (code - first)];
        }

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    gz_fail(gunzip, "Bad compressed data");
    return -1;
}

static bool gz_inflate_stored(Gunzip* gunzip) {
    const int low = gz_read_aligned_byte(gunzip);
    const int high = gz_read_aligned_byte(gunzip);
    const int inv_low = gz_read_aligned_byte(gunzip);
    const int inv_high = gz_read_aligned_byte(gunzip);

    if(gunzip->error) {
        return false;
    }

    const uint16_t length = (uint16_t)(low | (high << 8));
    const uint16_t inverse = (uint16_t)(inv_low | (inv_high << 8));

    if((uint16_t)~length != inverse) {
        gz_fail(gunzip, "Bad stored block");
        return false;
    }

    for(uint16_t i = 0; i < length; i++) {
        const int byte = gz_read_aligned_byte(gunzip);
        if(byte < 0) {
            return false;
        }
        if(!gz_emit(gunzip, (uint8_t)byte)) {
            return false;
        }
    }

    return true;
}

static void gz_build_fixed_tables(Gunzip* gunzip) {
    uint8_t* lengths = malloc(GZ_SYMBOL_COUNT);

    for(size_t i = 0; i < 144; i++)
        lengths[i] = 8;
    for(size_t i = 144; i < 256; i++)
        lengths[i] = 9;
    for(size_t i = 256; i < 280; i++)
        lengths[i] = 7;
    for(size_t i = 280; i < GZ_SYMBOL_COUNT; i++)
        lengths[i] = 8;
    gz_huffman_build(gunzip->literal, lengths, GZ_SYMBOL_COUNT);

    for(size_t i = 0; i < GZ_DIST_COUNT; i++)
        lengths[i] = 5;
    gz_huffman_build(gunzip->distance, lengths, GZ_DIST_COUNT);

    free(lengths);
}

static bool gz_build_dynamic_tables(Gunzip* gunzip) {
    const uint16_t literal_count = (uint16_t)gz_read_bits(gunzip, 5) + 257;
    const uint16_t distance_count = (uint16_t)gz_read_bits(gunzip, 5) + 1;
    const uint16_t code_length_count = (uint16_t)gz_read_bits(gunzip, 4) + 4;

    if(gunzip->error) {
        return false;
    }

    if((literal_count > GZ_SYMBOL_COUNT) || (distance_count > GZ_DIST_COUNT)) {
        gz_fail(gunzip, "Bad compressed data");
        return false;
    }

    uint8_t* lengths = malloc(GZ_SYMBOL_COUNT + GZ_DIST_COUNT);
    memset(lengths, 0, GZ_SYMBOL_COUNT + GZ_DIST_COUNT);
    bool success = false;

    do {
        uint8_t code_lengths[COUNT_OF(gz_code_length_order)] = {0};
        for(uint16_t i = 0; i < code_length_count; i++) {
            code_lengths[gz_code_length_order[i]] = (uint8_t)gz_read_bits(gunzip, 3);
        }
        if(gunzip->error) {
            break;
        }

        gz_huffman_build(gunzip->literal, code_lengths, COUNT_OF(gz_code_length_order));

        const uint16_t total = literal_count + distance_count;
        uint16_t index = 0;
        while(index < total) {
            const int symbol = gz_huffman_decode(gunzip, gunzip->literal);
            if(symbol < 0) {
                break;
            }

            if(symbol < 16) {
                lengths[index++] = (uint8_t)symbol;
                continue;
            }

            uint8_t value = 0;
            uint16_t repeat = 0;

            if(symbol == 16) {
                if(index == 0) {
                    gz_fail(gunzip, "Bad compressed data");
                    break;
                }
                value = lengths[index - 1];
                repeat = 3 + (uint16_t)gz_read_bits(gunzip, 2);
            } else if(symbol == 17) {
                repeat = 3 + (uint16_t)gz_read_bits(gunzip, 3);
            } else {
                repeat = 11 + (uint16_t)gz_read_bits(gunzip, 7);
            }

            if(gunzip->error || ((index + repeat) > total)) {
                gz_fail(gunzip, "Bad compressed data");
                break;
            }

            while(repeat--) {
                lengths[index++] = value;
            }
        }

        if(gunzip->error) {
            break;
        }

        gz_huffman_build(gunzip->literal, lengths, literal_count);
        gz_huffman_build(gunzip->distance, lengths + literal_count, distance_count);
        success = true;
    } while(false);

    free(lengths);
    return success;
}

static bool gz_inflate_block(Gunzip* gunzip) {
    while(true) {
        const int symbol = gz_huffman_decode(gunzip, gunzip->literal);
        if(symbol < 0) {
            return false;
        }

        if(symbol < 256) {
            if(!gz_emit(gunzip, (uint8_t)symbol)) {
                return false;
            }
            continue;
        }

        if(symbol == 256) {
            return true;
        }

        const uint16_t length_index = (uint16_t)(symbol - 257);
        if(length_index >= COUNT_OF(gz_length_base)) {
            gz_fail(gunzip, "Bad compressed data");
            return false;
        }

        const uint16_t length = gz_length_base[length_index] +
                                (uint16_t)gz_read_bits(gunzip, gz_length_extra[length_index]);

        const int distance_symbol = gz_huffman_decode(gunzip, gunzip->distance);
        if(distance_symbol < 0) {
            return false;
        }
        if(distance_symbol >= (int)COUNT_OF(gz_dist_base)) {
            gz_fail(gunzip, "Bad compressed data");
            return false;
        }

        const uint32_t distance =
            gz_dist_base[distance_symbol] + gz_read_bits(gunzip, gz_dist_extra[distance_symbol]);

        if(gunzip->error) {
            return false;
        }

        if((distance > GZ_WINDOW_SIZE) || (distance > gunzip->out_total)) {
            gz_fail(gunzip, "Bad compressed data");
            return false;
        }

        for(uint16_t i = 0; i < length; i++) {
            const size_t source = (gunzip->window_position - distance) & GZ_WINDOW_MASK;
            if(!gz_emit(gunzip, gunzip->window[source])) {
                return false;
            }
        }
    }
}

static bool gz_read_header(Gunzip* gunzip) {
    const int id1 = gz_next_byte(gunzip);
    const int id2 = gz_next_byte(gunzip);
    const int method = gz_next_byte(gunzip);
    const int flags = gz_next_byte(gunzip);

    if(gunzip->error) {
        return false;
    }

    if((id1 != 0x1F) || (id2 != 0x8B)) {
        gz_fail(gunzip, "Not a gzip archive");
        return false;
    }

    if(method != 8) {
        gz_fail(gunzip, "Unsupported compression");
        return false;
    }

    for(size_t i = 0; i < 6; i++) {
        gz_next_byte(gunzip);
    }

    if(flags & GZ_FLAG_FEXTRA) {
        const int low = gz_next_byte(gunzip);
        const int high = gz_next_byte(gunzip);
        if(gunzip->error) {
            return false;
        }
        for(uint16_t i = 0; i < (uint16_t)(low | (high << 8)); i++) {
            gz_next_byte(gunzip);
        }
    }

    if(flags & GZ_FLAG_FNAME) {
        while(gz_next_byte(gunzip) > 0) {
        }
    }

    if(flags & GZ_FLAG_FCOMMENT) {
        while(gz_next_byte(gunzip) > 0) {
        }
    }

    if(flags & GZ_FLAG_FHCRC) {
        gz_next_byte(gunzip);
        gz_next_byte(gunzip);
    }

    return !gunzip->error;
}

static bool gz_verify_trailer(Gunzip* gunzip) {
    uint32_t stored_crc = 0;
    uint32_t stored_size = 0;

    for(uint8_t i = 0; i < 4; i++) {
        const int byte = gz_read_aligned_byte(gunzip);
        if(byte < 0) {
            return false;
        }
        stored_crc |= (uint32_t)byte << (i * 8);
    }

    for(uint8_t i = 0; i < 4; i++) {
        const int byte = gz_read_aligned_byte(gunzip);
        if(byte < 0) {
            return false;
        }
        stored_size |= (uint32_t)byte << (i * 8);
    }

    if(stored_size != (uint32_t)gunzip->out_total) {
        gz_fail(gunzip, "Archive is truncated");
        return false;
    }

    if(stored_crc != gunzip->crc) {
        gz_fail(gunzip, "Archive is corrupted");
        return false;
    }

    return true;
}

bool gunzip_file(
    Gunzip* gunzip,
    const char* source_path,
    const char* dest_path,
    GunzipProgressCb progress_cb,
    void* context) {
    furi_assert(gunzip);

    gunzip->in_length = 0;
    gunzip->in_position = 0;
    gunzip->in_consumed = 0;
    gunzip->in_size = 0;
    gunzip->bit_buffer = 0;
    gunzip->bit_count = 0;
    gunzip->window_position = 0;
    gunzip->crc = 0;
    gunzip->out_total = 0;
    gunzip->error = NULL;
    gunzip->progress_cb = progress_cb;
    gunzip->progress_context = context;
    gunzip->last_percent = UINT8_MAX;

    bool success = false;

    do {
        if(!storage_file_open(gunzip->input, source_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            gz_fail(gunzip, "Cannot open archive");
            break;
        }
        gunzip->in_size = storage_file_size(gunzip->input);

        if(!storage_file_open(gunzip->output, dest_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            gz_fail(gunzip, "Cannot create temp file");
            break;
        }

        if(!gz_read_header(gunzip)) {
            break;
        }

        bool final_block = false;
        while(!final_block) {
            final_block = gz_read_bits(gunzip, 1) != 0;
            const uint32_t block_type = gz_read_bits(gunzip, 2);
            if(gunzip->error) {
                break;
            }

            bool block_ok = false;
            if(block_type == 0) {
                block_ok = gz_inflate_stored(gunzip);
            } else if(block_type == 1) {
                gz_build_fixed_tables(gunzip);
                block_ok = gz_inflate_block(gunzip);
            } else if(block_type == 2) {
                block_ok = gz_build_dynamic_tables(gunzip) && gz_inflate_block(gunzip);
            } else {
                gz_fail(gunzip, "Bad compressed data");
            }

            if(!block_ok) {
                break;
            }
        }

        if(gunzip->error) {
            break;
        }

        if(!gz_flush(gunzip)) {
            break;
        }

        success = gz_verify_trailer(gunzip);
    } while(false);

    storage_file_close(gunzip->output);
    storage_file_close(gunzip->input);

    if(!success) {
        FURI_LOG_E(TAG, "%s: %s", source_path, gunzip_last_error(gunzip));
    }

    return success;
}
