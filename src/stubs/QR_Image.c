#include <stdio.h>
#include <stdint.h>

int64_t QREncode(void) {
    static int count = 0;
    if (count < 5) { printf("[LinuxStub:QR_Image] QREncode()\n"); fflush(stdout); count++; }
    return 0;
}

int64_t QREncodeString(void) {
    static int count = 0;
    if (count < 5) { printf("[LinuxStub:QR_Image] QREncodeString()\n"); fflush(stdout); count++; }
    return 0;
}

int64_t QRGetSize(void) {
    static int count = 0;
    if (count < 5) { printf("[LinuxStub:QR_Image] QRGetSize()\n"); fflush(stdout); count++; }
    return 0;
}
