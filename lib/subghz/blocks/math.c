#include "math.h"

uint64_t subghz_protocol_blocks_reverse_key(uint64_t key, uint8_t bit_count) {
    uint64_t reverse_key = 0;
    for(uint8_t i = 0; i < bit_count; i++) {
        reverse_key = reverse_key << 1 | bit_read(key, i);
    }
    return reverse_key;
}

uint8_t subghz_protocol_blocks_get_parity(uint64_t key, uint8_t bit_count) {
    uint8_t parity = 0;
    for(uint8_t i = 0; i < bit_count; i++) {
        parity += bit_read(key, i);
    }
    return parity & 0x01;
}

uint8_t subghz_protocol_blocks_crc4(
    uint8_t const message[],
    size_t size,
    uint8_t polynomial,
    uint8_t init) {
    uint8_t remainder = init << 4; // LSBs are unused
    uint8_t poly = polynomial << 4;
    uint8_t bit;

    while(size--) {
        remainder ^= *message++;
        for(bit = 0; bit < 8; bit++) {
            if(remainder & 0x80) {
                remainder = (remainder << 1) ^ poly;
            } else {
                remainder = (remainder << 1);
            }
        }
    }
    return remainder >> 4 & 0x0f; // discard the LSBs
}

uint8_t subghz_protocol_blocks_crc7(
    uint8_t const message[],
    size_t size,
    uint8_t polynomial,
    uint8_t init) {
    uint8_t remainder = init << 1; // LSB is unused
    uint8_t poly = polynomial << 1;

    for(size_t byte = 0; byte < size; ++byte) {
        remainder ^= message[byte];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(remainder & 0x80) {
                remainder = (remainder << 1) ^ poly;
            } else {
                remainder = (remainder << 1);
            }
        }
    }
    return remainder >> 1 & 0x7f; // discard the LSB
}

uint8_t subghz_protocol_blocks_crc8(
    uint8_t const message[],
    size_t size,
    uint8_t polynomial,
    uint8_t init) {
    uint8_t remainder = init;

    for(size_t byte = 0; byte < size; ++byte) {
        remainder ^= message[byte];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(remainder & 0x80) {
                remainder = (remainder << 1) ^ polynomial;
            } else {
                remainder = (remainder << 1);
            }
        }
    }
    return remainder;
}

uint8_t subghz_protocol_blocks_crc8le(
    uint8_t const message[],
    size_t size,
    uint8_t polynomial,
    uint8_t init) {
    uint8_t remainder = subghz_protocol_blocks_reverse_key(init, 8);
    polynomial = subghz_protocol_blocks_reverse_key(polynomial, 8);

    for(size_t byte = 0; byte < size; ++byte) {
        remainder ^= message[byte];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(remainder & 1) {
                remainder = (remainder >> 1) ^ polynomial;
            } else {
                remainder = (remainder >> 1);
            }
        }
    }
    return remainder;
}

uint16_t subghz_protocol_blocks_crc16lsb(
    uint8_t const message[],
    size_t size,
    uint16_t polynomial,
    uint16_t init) {
    uint16_t remainder = init;

    for(size_t byte = 0; byte < size; ++byte) {
        remainder ^= message[byte];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(remainder & 1) {
                remainder = (remainder >> 1) ^ polynomial;
            } else {
                remainder = (remainder >> 1);
            }
        }
    }
    return remainder;
}

uint16_t subghz_protocol_blocks_crc16(
    uint8_t const message[],
    size_t size,
    uint16_t polynomial,
    uint16_t init) {
    uint16_t remainder = init;

    for(size_t byte = 0; byte < size; ++byte) {
        remainder ^= message[byte] << 8;
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(remainder & 0x8000) {
                remainder = (remainder << 1) ^ polynomial;
            } else {
                remainder = (remainder << 1);
            }
        }
    }
    return remainder;
}

uint8_t subghz_protocol_blocks_lfsr_digest8(
    uint8_t const message[],
    size_t size,
    uint8_t gen,
    uint8_t key) {
    uint8_t sum = 0;
    for(size_t byte = 0; byte < size; ++byte) {
        uint8_t data = message[byte];
        for(int i = 7; i >= 0; --i) {
            // XOR key into sum if data bit is set
            if((data >> i) & 1) sum ^= key;

            // roll the key right (actually the LSB is dropped here)
            // and apply the gen (needs to include the dropped LSB as MSB)
            if(key & 1)
                key = (key >> 1) ^ gen;
            else
                key = (key >> 1);
        }
    }
    return sum;
}

uint8_t subghz_protocol_blocks_lfsr_digest8_reflect(
    uint8_t const message[],
    size_t size,
    uint8_t gen,
    uint8_t key) {
    uint8_t sum = 0;
    // Process message from last byte to first byte (reflected)
    for(int byte = size - 1; byte >= 0; --byte) {
        uint8_t data = message[byte];
        // Process individual bits of each byte (reflected)
        for(uint8_t i = 0; i < 8; ++i) {
            // XOR key into sum if data bit is set
            if((data >> i) & 1) {
                sum ^= key;
            }

            // roll the key left (actually the LSB is dropped here)
            // and apply the gen (needs to include the dropped lsb as MSB)
            if(key & 0x80)
                key = (key << 1) ^ gen;
            else
                key = (key << 1);
        }
    }
    return sum;
}

uint16_t subghz_protocol_blocks_lfsr_digest16(
    uint8_t const message[],
    size_t size,
    uint16_t gen,
    uint16_t key) {
    uint16_t sum = 0;
    for(size_t byte = 0; byte < size; ++byte) {
        uint8_t data = message[byte];
        for(int8_t i = 7; i >= 0; --i) {
            // if data bit is set then xor with key
            if((data >> i) & 1) sum ^= key;

            // roll the key right (actually the LSB is dropped here)
            // and apply the gen (needs to include the dropped LSB as MSB)
            if(key & 1)
                key = (key >> 1) ^ gen;
            else
                key = (key >> 1);
        }
    }
    return sum;
}

uint8_t subghz_protocol_blocks_add_bytes(uint8_t const message[], size_t size) {
    uint32_t result = 0;
    for(size_t i = 0; i < size; ++i) {
        result += message[i];
    }
    return (uint8_t)result;
}

uint8_t subghz_protocol_blocks_parity8(uint8_t byte) {
    byte ^= byte >> 4;
    byte &= 0xf;
    return (0x6996 >> byte) & 1;
}

uint8_t subghz_protocol_blocks_parity_bytes(uint8_t const message[], size_t size) {
    uint8_t result = 0;
    for(size_t i = 0; i < size; ++i) {
        result ^= subghz_protocol_blocks_parity8(message[i]);
    }
    return result;
}

uint8_t subghz_protocol_blocks_xor_bytes(uint8_t const message[], size_t size) {
    uint8_t result = 0;
    for(size_t i = 0; i < size; ++i) {
        result ^= message[i];
    }
    return result;
}

bool subghz_protocol_blocks_key_is_degenerate(uint64_t data, uint8_t bit_count) {
    if(bit_count == 0) {
        return true;
    }
    if(bit_count > 64) {
        bit_count = 64;
    }

    const uint64_t mask = (bit_count == 64) ? UINT64_MAX : ((1ULL << bit_count) - 1ULL);
    data &= mask;

    if((data == 0) || (data == mask)) {
        return true;
    }

    if((data == (0xAAAAAAAAAAAAAAAAULL & mask)) || (data == (0x5555555555555555ULL & mask))) {
        return true;
    }

    const uint8_t nibble_count = (uint8_t)((bit_count + 3U) / 4U);
    uint8_t first = (uint8_t)(data & 0x0FU);
    bool all_same = true;
    uint8_t run_nibble = first;
    uint8_t run_len = 1;
    for(uint8_t i = 1; i < nibble_count; i++) {
        uint8_t nibble = (uint8_t)((data >> (4U * i)) & 0x0FU);
        if(nibble != first) {
            all_same = false;
        }
        if(nibble == run_nibble) {
            run_len++;
            if(run_len >= 4) {
                return true;
            }
        } else {
            run_nibble = nibble;
            run_len = 1;
        }
    }

    return all_same;
}
