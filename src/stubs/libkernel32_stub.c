#include <stdio.h>
#include <stdint.h>

int SetConsoleOutputCP(unsigned int cp) {
    return 1;
}

unsigned int GetConsoleOutputCP(void) {
    return 65001; // UTF-8
}

int SetConsoleCP(unsigned int cp) {
    return 1;
}

unsigned int GetConsoleCP(void) {
    return 65001;
}

void* GetStdHandle(int nStdHandle) {
    return (void*)(intptr_t)1;
}

int SetConsoleTextAttribute(void* hConsoleOutput, uint16_t wAttributes) {
    return 1;
}

uint32_t GetLastError(void) {
    return 0;
}

void SetLastError(uint32_t dwErrCode) {
}

int CloseHandle(void* hObject) {
    return 1;
}
