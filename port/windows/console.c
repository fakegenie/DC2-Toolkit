#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

// The port is a Windows program that opens no console of its own. Started from one, it writes to that console, unless
// its output already goes somewhere else.
__attribute__((constructor)) static void attach_console(void) {
    if (GetStdHandle(STD_ERROR_HANDLE) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    if (!freopen("CONOUT$", "w", stdout) || !freopen("CONOUT$", "w", stderr)) return;
    setvbuf(stderr, NULL, _IONBF, 0);
}
