#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void port_memory_set(const void *pool, const char *file, int line, int size);
void port_memory_use(const void *pool, int used, int size);

#ifdef __cplusplus
}
#endif
