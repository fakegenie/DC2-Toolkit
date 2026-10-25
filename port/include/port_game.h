#pragma once

#ifdef __APPLE__
#define PORT_GAME_BEGIN _Pragma("clang section bss=\"__GAME,__bss,zerofill\" data=\"__GAME,__data\"")
#else
#define PORT_GAME_BEGIN _Pragma("clang section bss=\"game_bss\" data=\"game_data\"")
#endif
#define PORT_GAME_END _Pragma("clang section bss=\"\" data=\"\"")
