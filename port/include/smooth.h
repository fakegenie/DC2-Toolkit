#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void smooth_presented(void);
void smooth_until(uint64_t deadline);

#ifdef __cplusplus
}
#endif
