#include "dataread.hpp"
#include "userdata.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>

static_assert(sizeof(DATA_HEADER) == 12);
static_assert(offsetof(DATA_HEADER, size) == 4);
static_assert(offsetof(DATA_HEADER, sector) == 8);
static_assert(sizeof(USER_PICTURE_INFO) == 0x18);

static unsigned char index_bytes[64];
static char pixels[2][16];

int main() {
    uint32_t record[] = {24, 17, 9, 32, 33, 10};
    memcpy(index_bytes, record, sizeof record);
    memcpy(index_bytes + 24, "a.img", 6);
    memcpy(index_bytes + 32, "b.img", 6);
    DATA_HEADER *headers = (DATA_HEADER *)index_bytes;
    assert(!strcmp((char *)index_bytes + headers[0].name_offset, "a.img"));
    assert(!strcmp((char *)index_bytes + headers[1].name_offset, "b.img"));
    assert(headers[0].size == 17 && headers[1].size == 33);
    assert(headers[0].sector == 9 && headers[1].sector == 10);
    USER_PICTURE_INFO photos[2];
    photos[0].image = pixels[0];
    photos[1].image = pixels[1];
    assert((char *)photos[0].image == pixels[0] && (char *)photos[1].image == pixels[1]);
    photos[0].image = photos[1].image;
    assert((char *)photos[0].image == pixels[1]);
    photos[1].image = NULL;
    assert((char *)photos[1].image == NULL);
}
