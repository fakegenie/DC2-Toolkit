#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

int buff_id;
int start_dma;
static std::uint32_t channel[36];

extern "C" void *sceDmaGetChan(int id) {
    assert(id == 8);
    return channel;
}

void SendDMA(void *packet, int size);

int main() {
    auto *scratch = static_cast<unsigned char *>(mmap(reinterpret_cast<void *>(0x70000000), 16384,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
    assert(scratch != MAP_FAILED);
    auto *destination = static_cast<unsigned char *>(mmap(reinterpret_cast<void *>(0x08000000), 16384,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
    assert(destination != MAP_FAILED);
    for (std::size_t i = 0; i < 8192; i++) {
        scratch[i] = static_cast<unsigned char>(i);
        scratch[8192 + i] = static_cast<unsigned char>(255 - i);
    }
    channel[0] = 0x100;
    SendDMA(reinterpret_cast<void *>(0x28000000), 512);
    assert(std::memcmp(destination, scratch, 8192) == 0);
    assert(buff_id == 1 && start_dma == 1);
    assert(channel[32] == 0 && channel[4] == 0x08000000);
    assert(channel[8] == 0 && channel[0] == 0);
    SendDMA(destination + 8192, 512);
    assert(std::memcmp(destination + 8192, scratch + 8192, 8192) == 0);
    assert(buff_id == 0 && start_dma == 1);
    assert(channel[32] == 8192 && channel[4] == 0x08002000);
    assert(channel[8] == 0 && channel[0] == 0);
    assert(munmap(scratch, 16384) == 0);
    assert(munmap(destination, 16384) == 0);
    return 0;
}
