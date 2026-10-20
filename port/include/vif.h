#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t row[4];
    uint32_t column[4];
    uint32_t mask;
    uint16_t base;
    uint16_t offset;
    uint16_t tops;
    uint16_t top;
    uint16_t itops;
    uint16_t itop;
    uint16_t cl;
    uint16_t wl;
    uint8_t mode;
    uint8_t dbf;
} PortVif;

extern PortVif port_vif;
void port_vif_reset(void);
int port_vif_feed(const void *data, size_t bytes);
int port_vif_finish(void);
const char *port_vif_error(void);

#ifdef __cplusplus
}
#endif
