#include "mg_texture.hpp"
#include "mg_tanime.hpp"
#include "mg_memory.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>

int main() {
    u_long128 bytes[64];
    memset(bytes, 0xa5, sizeof(bytes));
    mgCMemory memory;
    memory.stSetBuffer(bytes, 64);
    mgCTextureAnime animation;
    CList<mgCTexAnimeData> *first = animation.NewTexAnimeGroupData(23, &memory);
    assert(first && animation.list[23] == first && animation.now[23] == first);
    assert(animation.list[22] == nullptr && animation.now[22] == nullptr);
    assert((uintptr_t)(first + 1) <= (uintptr_t)(bytes + memory.stack_used));
    for (size_t i = memory.stack_used * 16; i < sizeof(bytes); i++) assert(((unsigned char *)bytes)[i] == 0xa5);
    CList<mgCTexAnimeData> *second = animation.NewTexAnimeGroupData(23, &memory);
    assert(second && first->next == second && second->prev == first);
    assert((uintptr_t)(first + 1) <= (uintptr_t)second);
    assert((uintptr_t)(second + 1) <= (uintptr_t)(bytes + memory.stack_used));
    for (size_t i = memory.stack_used * 16; i < sizeof(bytes); i++) assert(((unsigned char *)bytes)[i] == 0xa5);
    assert(animation.NewTexAnimeGroupData(24, &memory) == nullptr);
}
