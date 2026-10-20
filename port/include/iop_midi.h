#pragma once
#include <stdint.h>
int iop_midi_rpc(unsigned command, const void *send, int send_size, void *receive, int receive_size);
void iop_midi_tick(void);
void iop_midi_reset(void);
int iop_midi_message(unsigned port, uint32_t message);
int iop_midi_hs_message(unsigned port, const unsigned char *message);
