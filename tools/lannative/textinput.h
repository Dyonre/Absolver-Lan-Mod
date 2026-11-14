// Pure helpers for the in-game "type the join address" box: which characters a key press makes, and the edit buffer.
// lannative.c polls the keys (GetAsyncKeyState edges) and feeds them here; only characters that can be part of a host address are accepted.
#ifndef LANNATIVE_TEXTINPUT_H
#define LANNATIVE_TEXTINPUT_H

#include <string.h>

#define TI_MAX 60

typedef struct { char buf[TI_MAX + 1]; int len; } TextInput;

// The character a virtual key types (0 = none). Letters come out lower case unless shift is held; shift + ';' is ':'.
static char ti_vk_char(int vk, int shift) {
    if (vk >= 0x30 && vk <= 0x39) return shift ? 0 : (char)vk;                    // 0-9 (shift = symbols: not wanted)
    if (vk >= 0x60 && vk <= 0x69) return (char)('0' + (vk - 0x60));               // numpad 0-9
    if (vk >= 0x41 && vk <= 0x5A) return (char)(shift ? vk : vk + 32);            // A-Z
    if (vk == 0xBE || vk == 0x6E) return '.';                                     // '.' / numpad '.'
    if (vk == 0xBD || vk == 0x6D) return shift && vk == 0xBD ? 0 : '-';           // '-' (shift + '-' is '_': not wanted)
    if (vk == 0xBA) return shift ? ':' : 0;                                       // shift + ';' = ':'
    return 0;
}
// The virtual keys the box listens to (for edge detection).
static const int ti_keys[] = {
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39, 0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,
    0xBE,0x6E,0xBD,0x6D,0xBA };
#define TI_KEY_COUNT ((int)(sizeof ti_keys / sizeof ti_keys[0]))

static void ti_clear(TextInput *t) { t->buf[0] = 0; t->len = 0; }
static void ti_set(TextInput *t, const char *s) { ti_clear(t); for (; *s && t->len < TI_MAX; s++) t->buf[t->len++] = *s; t->buf[t->len] = 0; }
static int ti_add(TextInput *t, char c) {                                         // 1 = added
    int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':';
    if (!ok || t->len >= TI_MAX) return 0;
    t->buf[t->len++] = c; t->buf[t->len] = 0; return 1;
}
static void ti_backspace(TextInput *t) { if (t->len > 0) t->buf[--t->len] = 0; }

#endif
