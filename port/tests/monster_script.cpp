#include "runscript_opcodes.hpp"
#include <cassert>
#include <cstring>

int main() {
    u_long128 storage[RS_MONSTER_MEMORY_QUADWORDS];
    std::memset(storage, 0xa5, sizeof(storage));
    mgCMemory memory;
    memory.stSetBuffer(storage, RS_MONSTER_MEMORY_QUADWORDS);
    assert(memory.Alloc(3184));
    RS_PROG_HEADER program = {};
    std::memcpy(program.magic, "SB2", 3);
    CRunScript script;
    assert(SetMonsterScript(&script, reinterpret_cast<char *>(&program), &memory));
    assert(script.stack && script.call);
    assert(script.stack_end - script.stack == 128);
    assert(script.call_end - script.call == 512);
    assert(reinterpret_cast<unsigned char *>(script.call_end) <= reinterpret_cast<unsigned char *>(storage + memory.stack_used));
    assert(memory.stack_used == 3184 + (128 * sizeof(RS_STACKDATA) + 512 * sizeof(RS_CALLDATA) + 15) / 16);
    script.sp = script.stack;
    script.frame = script.stack;
    script.call_sp = script.call;
    funcdata callee = {};
    vmcode_t instruction = {};
    for (int i = 0; i < 512; i++) {
        script.call_func(&callee, &instruction);
        assert(script.call_sp == script.call + i + 1);
    }
    for (int i = 512; i > 0; i--) {
        assert(script.ret_func() == &instruction);
        assert(script.call_sp == script.call + i - 1);
    }
    for (size_t i = memory.stack_used * 16; i < sizeof(storage); i++) assert(reinterpret_cast<unsigned char *>(storage)[i] == 0xa5);
}
