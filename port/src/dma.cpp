#include "port_window.h"
#include <cstdint>
#include <cstring>

extern int buff_id;
extern int start_dma;
extern "C" void *sceDmaGetChan(int channel);

void SendDMA(void *packet, int size) {
    std::uintptr_t address = port_address(packet) & 0x0fffffff;
    std::uintptr_t scratch = buff_id ? 0x70002000 : 0x70000000;
    std::memcpy(reinterpret_cast<void *>(PORT_WINDOW + address), reinterpret_cast<void *>(PORT_WINDOW + scratch),
                static_cast<std::size_t>(size) * 16);
    auto *channel = static_cast<std::uint32_t *>(sceDmaGetChan(8));
    channel[32] = static_cast<std::uint32_t>(scratch & 0x0fffffff);
    channel[4] = static_cast<std::uint32_t>(address);
    channel[8] = 0;
    channel[0] &= ~std::uint32_t(0x100);
    start_dma = 1;
    buff_id = !buff_id;
}
