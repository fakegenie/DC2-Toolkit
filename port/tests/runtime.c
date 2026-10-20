#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/runtime.c"
static int called;
static void *construct(void *pointer, int flag) {
    assert(flag == 1);
    *(int *)pointer = ++called;
    return pointer;
}
int main(void) {
    assert(fptosi(NAN) == 0 && fptoui(NAN) == 0);
    assert(fptosi(INFINITY) == INT_MAX && fptosi(-INFINITY) == INT_MIN);
    assert(fptoui(INFINITY) == UINT_MAX && fptoui(-INFINITY) == 0);
    assert(fptosi(-1.9f) == -1 && fptoui(1.9f) == 1);
    assert(fptosi(2147483520.0f) == 2147483520);
    assert(fptoui(4294967040.0f) == 4294967040u);
    uint32_t buffer[16] = {0};
    int *array = __construct_new_array(buffer, construct, NULL, sizeof(int), 8);
    assert(array == (int *)buffer + 4 && buffer[0] == 4 && buffer[1] == 8);
    for (int i = 0; i < 8; ++i) assert(array[i] == i + 1);
    assert(__construct_new_array(NULL, construct, NULL, 4, 8) == NULL);
    puts("runtime checks passed");
}
