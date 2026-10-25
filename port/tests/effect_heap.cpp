#include "effscript.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>

int SetEffectScript(CRunScript *script, char *program, mgCMemory *memory);

static u_long128 storage[2048];
static RS_PROG_HEADER program;

_EFF_SCRIPT *allocate_effect(mgCMemory *memory) {
    u_long128 *bytes = memory->Alloc((sizeof(_EFF_SCRIPT) + 15) / 16 + 2);
    assert(bytes);
    _EFF_SCRIPT *effect = new (bytes) _EFF_SCRIPT;
    assert(memory->Alloc((sizeof(CCharacter2) + 15) / 16 + 2));
    assert(memory->Alloc(65));
    SetEffectScript(&effect->run, reinterpret_cast<char *>(&program), memory);
    memory->stAlign64();
    memory->EndStackMode();
    return effect;
}

int main() {
    static_assert(sizeof(mgMEMORY_BLOCK) == 16);
    std::memcpy(program.magic, "SB2", 3);
    mgCMemory memory;
    memory.SetHeapMem(storage, 2048);
    assert(memory.heap_top->next == reinterpret_cast<mgMEMORY_BLOCK *>(storage + 2047));
    u_long128 *hole = memory.StartStackMode(MG_STACK_MODE_FIT, 300);
    assert(hole);
    assert(memory.Alloc(300) == hole);
    memory.EndStackMode();
    u_long128 *barrier = memory.StartStackMode(MG_STACK_MODE_FIT, 32);
    assert(barrier);
    assert(memory.Alloc(32) == barrier);
    memory.EndStackMode();
    memory.Free(hole);
    u_long128 *old_reserve = memory.StartStackMode(MG_STACK_MODE_FIT, 125 + 36 + 65);
    assert(old_reserve == hole);
    _EFF_SCRIPT *failed = allocate_effect(&memory);
    assert(failed->run.stack == nullptr && failed->run.call != nullptr);
    memory.Free(old_reserve);
    u_long128 *work = memory.StartStackMode(MG_STACK_MODE_FIT, EFF_SPT_RUNTIME_QUADWORDS + 65);
    assert(work && work > barrier);
    _EFF_SCRIPT *effect = allocate_effect(&memory);
    assert(effect->run.stack && effect->run.call);
    assert(effect->run.stack_end - effect->run.stack == 64);
    assert(effect->run.call_end - effect->run.call == 2);
    mgMEMORY_BLOCK *block = reinterpret_cast<mgMEMORY_BLOCK *>(work) - 1;
    uintptr_t begin = reinterpret_cast<uintptr_t>(static_cast<u_long128 *>(block->data));
    uintptr_t end = reinterpret_cast<uintptr_t>(reinterpret_cast<u_long128 *>(block) + block->size);
    assert(end <= reinterpret_cast<uintptr_t>(storage + 2047));
    assert(reinterpret_cast<uintptr_t>(effect->run.stack) >= begin);
    assert(reinterpret_cast<uintptr_t>(effect->run.stack_end) <= end);
    assert(reinterpret_cast<uintptr_t>(effect->run.call) >= begin);
    assert(reinterpret_cast<uintptr_t>(effect->run.call_end) <= end);
    assert(reinterpret_cast<uintptr_t>(effect->run.stack_end) <= reinterpret_cast<uintptr_t>(effect->run.call));
    for (int i = 0; i < 64; ++i) effect->run.stack[i].val.i = 1000 + i;
    memory.Free(barrier);
    u_long128 *reused = memory.StartStackMode(MG_STACK_MODE_FIT, 300);
    assert(reused == hole);
    assert(memory.Alloc(300) == reused);
    std::memset(reused, 0xa5, 300 * 16);
    memory.EndStackMode();
    for (int i = 0; i < 64; ++i) assert(effect->run.stack[i].val.i == 1000 + i);
    assert(effect->run.prog == &program);
    memory.Free(reused);
    memory.Free(work);
    assert(memory.heap_top->next == reinterpret_cast<mgMEMORY_BLOCK *>(storage + 2047));
}
