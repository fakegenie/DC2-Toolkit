#pragma once

#include "sce/libmc.h"

#define PORT_MC_BUSY (-200)

int port_memory_card_init(const char *root);
const char *port_memory_card_root(void);
