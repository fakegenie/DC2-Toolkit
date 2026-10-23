#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void smooth_presented(void);
int smooth_due(void);
void smooth_shown(void);
void smooth_until(uint64_t deadline);

#ifdef __cplusplus
}
#endif
