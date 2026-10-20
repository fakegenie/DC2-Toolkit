#include "dataread.hpp"
#include "inventmn.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>

static_assert(sizeof(DATA_HEADER) == 12);
static_assert(sizeof(PIC_NAME_INFO) == 8);
static_assert(offsetof(DATA_HEADER, size) == 4);
static_assert(offsetof(DATA_HEADER, sector) == 8);

static unsigned char index_bytes[64];

int main() {
    assert((uintptr_t)index_bytes < UINT32_MAX);
    uint32_t record[] = {24, 17, 9, 32, 33, 10};
    memcpy(index_bytes, record, sizeof record);
    memcpy(index_bytes + 24, "a.img", 6);
    memcpy(index_bytes + 32, "b.img", 6);
    DATA_HEADER *headers = (DATA_HEADER *)index_bytes;
    for (int i = 0; i < 2; i++) headers[i].name += (uintptr_t)index_bytes;
    assert(!strcmp(headers[0].name, "a.img"));
    assert(!strcmp(headers[1].name, "b.img"));
    assert(headers[0].size == 17 && headers[1].size == 33);
    assert(headers[0].sector == 9 && headers[1].sector == 10);
}
