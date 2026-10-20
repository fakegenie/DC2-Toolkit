#include <cassert>

void mgSetRotateThread(int priority);
void WaitVSync(int start, int frames);

static int count;
static int rotations;
static int syncs;

int mgGetVSyncCount() { return count; }

extern "C" int RotateThreadReadyQueue(int priority) {
    assert(priority == 8);
    rotations++;
    count++;
    return 0;
}

extern "C" int sceGsSyncV(int mode) {
    assert(mode == 0);
    syncs++;
    count++;
    return 0;
}

int main() {
    WaitVSync(0, 3);
    assert(count == 3 && syncs == 3 && rotations == 0);
    mgSetRotateThread(8);
    WaitVSync(3, 2);
    assert(count == 5 && syncs == 3 && rotations == 2);
    mgSetRotateThread(-1);
    WaitVSync(5, 0);
    assert(syncs == 3 && rotations == 2);
    WaitVSync(5, 1);
    assert(count == 6 && syncs == 4);
}
