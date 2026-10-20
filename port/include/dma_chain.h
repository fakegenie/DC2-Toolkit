#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int port_dma_map(uint32_t address, const void *data, size_t size);
void port_dma_unmap(void);
int port_dma_chain(unsigned channel, const void *tag, int transfer_tag);
int port_dma_send(unsigned channel, const void *data, size_t qwords);
const char *port_dma_error(void);

#ifdef __cplusplus
}
#endif
