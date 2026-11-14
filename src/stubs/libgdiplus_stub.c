#include <stdint.h>
#include <stdio.h>

int GdiplusStartup(uint64_t *token, void *input, void *output) {
    if (token) *token = 1;
    printf("[libgdiplus_stub] GdiplusStartup() called -> OK\n");
    return 0;
}

int GdiplusShutdown(uint64_t token) {
    printf("[libgdiplus_stub] GdiplusShutdown() called -> OK\n");
    return 0;
}
