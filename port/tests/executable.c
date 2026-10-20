#include <assert.h>
#include <string.h>
#include "cdvd.h"
#include "executable.h"

static int missing;
static int failed_read;
static size_t read_bytes;
static int read_count;

int port_cdvd_find(const char *path, struct sceCdlFILE *file) {
    assert(strcmp(path, "SCES_511.90;1") == 0);
    if (missing) return 0;
    *file = (struct sceCdlFILE){0};
    file->size = 32769;
    return 1;
}

int port_cdvd_read_file(const struct sceCdlFILE *file, uint64_t offset, void *buffer, size_t size) {
    assert(offset == read_bytes);
    assert(size <= 16384 && offset + size <= file->size);
    if (failed_read) return 0;
    memset(buffer, 'a', size);
    read_bytes += size;
    ++read_count;
    return 1;
}

static void check_vector(const char *data, const char *expected) {
    size_t size = strlen(data);
    for (size_t chunk = 1; chunk <= 80; ++chunk) {
        struct port_sha256 hash;
        char digest[65];
        port_sha256_init(&hash);
        port_sha256_update(&hash, NULL, 0);
        for (size_t offset = 0; offset < size;) {
            size_t count = size - offset;
            if (count > chunk) count = chunk;
            port_sha256_update(&hash, data + offset, count);
            offset += count;
        }
        port_sha256_final(&hash, digest);
        assert(strcmp(digest, expected) == 0);
    }
}

int main(void) {
    check_vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_vector("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                 "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
                 "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    struct port_sha256 hash;
    char digest[65];
    unsigned char block[1000];
    memset(block, 'a', sizeof(block));
    port_sha256_init(&hash);
    for (int i = 0; i < 1000; ++i) port_sha256_update(&hash, block, sizeof(block));
    port_sha256_final(&hash, digest);
    assert(strcmp(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    assert(port_executable_check() == 1);
    assert(read_count == 3 && read_bytes == 32769);
    missing = 1;
    assert(port_executable_check() == 0);
    missing = 0;
    failed_read = 1;
    read_bytes = 0;
    assert(port_executable_check() == 0);
    return 0;
}
