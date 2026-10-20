#ifndef PORT_VU1_H
#define PORT_VU1_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char port_vu1_code[16384];
extern unsigned char port_vu1_data[16384];
extern uint16_t port_vu1_top;
extern uint16_t port_vu1_itop;
struct port_vu1_registers {
    uint32_t vf[32][4];
    uint16_t vi[16];
    uint32_t acc[4];
    uint32_t i;
    uint32_t q;
    uint32_t p;
    uint32_t clip;
    uint16_t mac;
    uint16_t status;
    unsigned pc;
    unsigned steps;
    int halted;
    uint32_t fault_upper;
    uint32_t fault_lower;
};
extern struct port_vu1_registers port_vu1;
void port_vu1_reset(void);
int port_vu1_start(unsigned entry);
int port_vu1_continue(void);
#ifdef __cplusplus
}
#endif
#endif
