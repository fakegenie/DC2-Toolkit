#include <stdio.h>
#include <string.h>
#include "cdvd.h"
#include "executable.h"

#if __has_include("port_build.h")
#include "port_build.h"
#endif

#ifndef PORT_EXECUTABLE_SHA256
#define PORT_EXECUTABLE_SHA256 "41dec16868ec5ccc74f8927dad347a810828ae87ecd021ef7cc8a9568abb856c"
#endif

static uint32_t rotate(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32 - count));
}

static void sha256_block(struct port_sha256 *hash) {
    static const uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t schedule[64];
    for (int i = 0; i < 16; ++i) {
        const unsigned char *bytes = hash->block + i * 4;
        schedule[i] = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
                      (uint32_t)bytes[2] << 8 | bytes[3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t a = schedule[i - 15];
        uint32_t b = schedule[i - 2];
        schedule[i] = schedule[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) +
                      schedule[i - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    uint32_t a = hash->words[0], b = hash->words[1], c = hash->words[2], d = hash->words[3];
    uint32_t e = hash->words[4], f = hash->words[5], g = hash->words[6], h = hash->words[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t first = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                         ((e & f) ^ (~e & g)) + constants[i] + schedule[i];
        uint32_t second = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                          ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    hash->words[0] += a;
    hash->words[1] += b;
    hash->words[2] += c;
    hash->words[3] += d;
    hash->words[4] += e;
    hash->words[5] += f;
    hash->words[6] += g;
    hash->words[7] += h;
}

void port_sha256_init(struct port_sha256 *hash) {
    *hash = (struct port_sha256){
        {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0
    };
}

void port_sha256_update(struct port_sha256 *hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    hash->size += size;
    while (size) {
        size_t count = 64 - hash->used;
        if (count > size) count = size;
        memcpy(hash->block + hash->used, bytes, count);
        hash->used += count;
        bytes += count;
        size -= count;
        if (hash->used == 64) {
            sha256_block(hash);
            hash->used = 0;
        }
    }
}

void port_sha256_final(struct port_sha256 *hash, char digest[65]) {
    static const char hex[] = "0123456789abcdef";
    uint64_t bits = hash->size * 8;
    unsigned char tail[128] = {0x80};
    size_t count = hash->used < 56 ? 56 - hash->used : 120 - hash->used;
    for (int i = 0; i < 8; ++i) tail[count + i] = (unsigned char)(bits >> (56 - i * 8));
    port_sha256_update(hash, tail, count + 8);
    for (int i = 0; i < 32; ++i) {
        unsigned char value = (unsigned char)(hash->words[i / 4] >> (24 - (i % 4) * 8));
        digest[i * 2] = hex[value >> 4];
        digest[i * 2 + 1] = hex[value & 15];
    }
    digest[64] = 0;
}

int port_executable_check(void) {
    struct sceCdlFILE file;
    if (!port_cdvd_find("SCES_511.90;1", &file)) {
        fprintf(stderr, "executable: cannot find SCES_511.90 on disc\n");
        return 0;
    }
    struct port_sha256 hash;
    unsigned char data[16384];
    char digest[65];
    port_sha256_init(&hash);
    for (uint64_t offset = 0; offset < file.size;) {
        size_t size = file.size - offset;
        if (size > sizeof(data)) size = sizeof(data);
        if (!port_cdvd_read_file(&file, offset, data, size)) {
            fprintf(stderr, "executable: cannot read SCES_511.90 on disc\n");
            return 0;
        }
        port_sha256_update(&hash, data, size);
        offset += size;
    }
    port_sha256_final(&hash, digest);
    if (strcmp(digest, PORT_EXECUTABLE_SHA256)) {
        fprintf(stderr, "warning: disc executable SHA-256 differs from this native build\n"
                        "  expected: %s\n  actual:   %s\n", PORT_EXECUTABLE_SHA256, digest);
    }
    return 1;
}
