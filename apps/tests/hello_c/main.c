/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/tests/hello_c/main.c
 * Purpose: First user-space standard C test application for irisOS.
 *
 * Licensing: MIT License. Permissive Zone 1 test app.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const char* message = "[hello_c] Hello from standard C on irisOS! malloc/free/printf working.\n";
    size_t len = strlen(message) + 1;

    char* buffer = (char*)malloc(len);
    if (!buffer) {
        printf("[hello_c] Error: malloc(%zu) failed!\n", len);
        return 1;
    }

    strcpy(buffer, message);
    printf("%s", buffer);

    free(buffer);
    return 0;
}
