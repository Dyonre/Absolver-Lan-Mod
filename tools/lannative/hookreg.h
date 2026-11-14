// hookreg.h - the hook registry: which game functions are patched, by whom, and in what order the detours stack.
// Pure data logic (no OS calls) so it is testable offline (test\hookreg_test.c). The patching itself lives in plugins.inc / lannative.c.
// A patched function looks like:   [jmp [rip+0] -> top detour] ...   top detour -> (its "original") -> ... -> bottom detour -> trampoline -> game code.
// Registering a new detour on an already hooked function does not patch the game code again: the jmp's target address is rewritten to the new
// detour and the previous top becomes the new detour's "original" (call-next pointer).
#ifndef HOOKREG_H
#define HOOKREG_H
#include <stdint.h>
#include <string.h>

#define HR_MAX     96
#define HR_STOLEN  24
#define HR_OWNER   48

typedef struct {
    uint32_t rva;                  // target = game base + rva
    uint32_t stolen;               // bytes moved into the trampoline
    uint8_t  orig[HR_STOLEN];      // the game's original bytes at the target (for the byte check of later hooks)
    void    *tramp;                // runs the original instruction bytes, then continues in the game code
    void    *top;                  // the detour the patched jmp currently points to (newest)
    int      depth;                // number of detours stacked (1 = loader or first plugin only)
    char     owners[4][HR_OWNER];  // first four owners, oldest first (diagnostics)
} HookEntry;

typedef struct { HookEntry e[HR_MAX]; int n; } HookReg;

static HookEntry *hr_find(HookReg *r, uint32_t rva) {
    for (int i = 0; i < r->n; i++) if (r->e[i].rva == rva) return &r->e[i];
    return NULL;
}
static void hr_owner(HookEntry *h, const char *owner) {
    if (h->depth >= 1 && h->depth <= 4) { strncpy(h->owners[h->depth - 1], owner ? owner : "?", HR_OWNER - 1); h->owners[h->depth - 1][HR_OWNER - 1] = 0; }
}
// first detour on a freshly patched function; returns NULL when the table is full
static HookEntry *hr_add(HookReg *r, uint32_t rva, uint32_t stolen, const uint8_t *orig, void *tramp, void *detour, const char *owner) {
    if (r->n >= HR_MAX || stolen > HR_STOLEN) return NULL;
    HookEntry *h = &r->e[r->n++]; memset(h, 0, sizeof *h);
    h->rva = rva; h->stolen = stolen; memcpy(h->orig, orig, stolen); h->tramp = tramp; h->top = detour; h->depth = 1; hr_owner(h, owner);
    return h;
}
// does the byte check a plugin passes match the game's original bytes? (compares the shorter of the two lengths, at least 14)
static int hr_bytes_match(const HookEntry *h, const uint8_t *expected, uint32_t stolen) {
    uint32_t n = stolen < h->stolen ? stolen : h->stolen;
    return n >= 14 && memcmp(h->orig, expected, n) == 0;
}
// stack another detour on top; returns what the new detour must call as its "original" (the previous top)
static void *hr_stack(HookEntry *h, void *detour, const char *owner) {
    void *prev = h->top; h->top = detour; h->depth++; hr_owner(h, owner); return prev;
}
#endif
