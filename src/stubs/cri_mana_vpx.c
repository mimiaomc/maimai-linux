#include <stdio.h>
#include <stdint.h>

int64_t criVvp9_GetAlphaInterface(void) {
    static int count = 0;
    if (count < 5) { printf("[LinuxStub:cri_mana_vpx] criVvp9_GetAlphaInterface()\n"); fflush(stdout); count++; }
    return 0;
}

int64_t criVvp9_GetInterface(void) {
    static int count = 0;
    if (count < 5) { printf("[LinuxStub:cri_mana_vpx] criVvp9_GetInterface()\n"); fflush(stdout); count++; }
    return 0;
}

int64_t criVvp9_SetUserAllocator(void) {
    static int count = 0;
    if (count < 5) { printf("[LinuxStub:cri_mana_vpx] criVvp9_SetUserAllocator()\n"); fflush(stdout); count++; }
    return 0;
}
