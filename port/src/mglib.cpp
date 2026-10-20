#include "mglib.hpp"
#include "sce/eekernel.h"
#include "sce/libgraph.h"

static int rotate_priority = -1;

PORT_OVERRIDE void mgSetRotateThread(int priority) {
    rotate_priority = priority;
}

PORT_OVERRIDE void WaitVSync(int start, int frames) {
    while (mgGetVSyncCount() - start < frames) {
        if (rotate_priority > 0) RotateThreadReadyQueue(rotate_priority);
        else sceGsSyncV(0);
    }
}
