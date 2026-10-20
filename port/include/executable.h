#pragma once

#include <stddef.h>
#include <stdint.h>

struct port_sha256 {
    uint32_t words[8];
    uint64_t size;
    unsigned char block[64];
    size_t used;
};

void port_sha256_init(struct port_sha256 *hash);
void port_sha256_update(struct port_sha256 *hash, const void *data, size_t size);
void port_sha256_final(struct port_sha256 *hash, char digest[65]);
int port_executable_check(void);
