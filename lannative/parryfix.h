// Pure byte-level part of the Forsaken parry patch. Kept free of Win32 so the
// offline fake-memory test can prove that a different game build is never changed.
#ifndef LANNATIVE_PARRYFIX_H
#define LANNATIVE_PARRYFIX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PARRYFIX_SIZE 7u

static int parryfix_apply_bytes(uint8_t *code, size_t size) {
    static const uint8_t original[PARRYFIX_SIZE] = { 0x48, 0x81, 0xC1, 0xFC, 0x01, 0x00, 0x00 };
    if (!code || size < PARRYFIX_SIZE || memcmp(code, original, PARRYFIX_SIZE) != 0) return 0;
    code[3] = 0xF4; // add rcx, 0x1FC -> add rcx, 0x1F4
    return 1;
}

#endif
