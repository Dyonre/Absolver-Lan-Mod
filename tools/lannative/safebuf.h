#ifndef LANNATIVE_SAFEBUF_H
#define LANNATIVE_SAFEBUF_H
// Bounded string helpers for parsing mod content. Pure C, no engine calls; every write is checked against the capacity given.
#include <stddef.h>
#include <string.h>

// Copies in -> out, replacing the value after every occurrence of key (up to the next ',' or ')') by value. Returns the number of
// replacements, or -1 when the result would not fit in cap bytes (out is then unusable).
static int sb_replace_keyed(const char *in, const char *key, const char *value, char *out, size_t cap) {
    size_t kl = strlen(key), vl = strlen(value), w = 0; int reps = 0;
    if (!cap) return -1;
    for (const char *p = in; *p; ) {
        if (!strncmp(p, key, kl)) {
            if (w + kl + vl >= cap) return -1;
            memcpy(out + w, key, kl); w += kl; memcpy(out + w, value, vl); w += vl; p += kl;
            while (*p && *p != ',' && *p != ')') p++;
            reps++;
        } else {
            if (w + 1 >= cap) return -1;
            out[w++] = *p++;
        }
    }
    out[w] = 0; return reps;
}

// Appends s and a comma to the NUL-terminated list lst (capacity cap). What does not fit is cut; the list stays terminated.
// Returns 1 when s was appended whole, 0 when it was cut or dropped.
static int sb_list_add(char *lst, size_t cap, const char *s) {
    size_t n = strlen(lst), sl = strlen(s);
    if (cap < 3 || n > cap - 3) return 0;
    size_t room = cap - n - 2, take = sl < room ? sl : room;
    memcpy(lst + n, s, take); lst[n + take] = ','; lst[n + take + 1] = 0;
    return take == sl;
}
#endif
