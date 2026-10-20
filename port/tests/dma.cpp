#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "port_window.h"

int buff_id;
int start_dma;
static std::uint32_t channel[36];

extern "C" void *sceDmaGetChan(int id) {
    assert(id == 8);
    return channel;
}

void SendDMA(void *packet, int size);

alignas(16) static unsigned char ram[16384];

int main() {
    auto *scratch = reinterpret_cast<unsigned char *>(PORT_WINDOW + 0x70000000);
    unsigned char *destination = ram;
    std::uint32_t address = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(destination) & 0x0fffffff);
    for (std::size_t i = 0; i < 8192; i++) {
        scratch[i] = static_cast<unsigned char>(i);
        scratch[8192 + i] = static_cast<unsigned char>(255 - i);
    }
    channel[0] = 0x100;
    SendDMA(reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(destination) | 0x20000000), 512);
    assert(std::memcmp(destination, scratch, 8192) == 0);
    assert(buff_id == 1 && start_dma == 1);
    assert(channel[32] == 0 && channel[4] == address);
    assert(channel[8] == 0 && channel[0] == 0);
    SendDMA(destination + 8192, 512);
    assert(std::memcmp(destination + 8192, scratch + 8192, 8192) == 0);
    assert(buff_id == 0 && start_dma == 1);
    assert(channel[32] == 8192 && channel[4] == address + 8192);
    assert(channel[8] == 0 && channel[0] == 0);
    return 0;
}
