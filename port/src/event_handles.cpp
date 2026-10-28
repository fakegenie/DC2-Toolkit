#include "event_func.hpp"
#include "mg_memory.hpp"

static void *handle_target(CEoh *handle) {
    if (handle->type == EOH_TYPE_OBJECT) return handle->object;
    if (handle->type == EOH_TYPE_FRAME) return handle->frame;
    if (handle->type == EOH_TYPE_FUNC_POINT) return handle->func_point;
    return NULL;
}

void port_forget_event_handles(mgCMemory *memory) {
    char *begin = (char *)memory->stack;
    char *end = begin + memory->stack_size * sizeof(u_long128);
    for (int slot = 0; slot < EOH_NUM; ++slot) {
        CEoh *handle = &EventObjHandleMother.eoh[slot];
        char *target = (char *)handle_target(handle);
        if (target >= begin && target < end) *handle = CEoh();
    }
}
