// grantpoll.h - pure state for the content grants (items and powers; content.inc / powers.inc). Offline test: test\grantpoll_test.c.
// A grant stays pending until the local player's inventory has been reachable with the SAME non-zero item count on two polls in a row (the save's
// items are in, nothing is still being added) and then either already holds the item or inventory_add returns an index >= 0. The loader polls every
// GP_POLL_S seconds from every new non-title world while anything is pending; the pending flags are re-armed when a character save loads.
#pragma once
#define GP_POLL_S 1.5f
enum { GP_OK, GP_NO_PC, GP_NO_PAWN, GP_NO_INV, GP_INV_EMPTY, GP_SETTLING, GP_ADD_FAILED, GP_NO_OBJECT };
static const char *gp_why(int w) {
    switch (w) {
        case GP_OK: return "ready"; case GP_NO_PC: return "no player controller"; case GP_NO_PAWN: return "no pawn"; case GP_NO_INV: return "no inventory component";
        case GP_INV_EMPTY: return "inventory empty (save not applied yet)"; case GP_SETTLING: return "inventory settling"; case GP_ADD_FAILED: return "inventory_add failed";
        case GP_NO_OBJECT: return "item object not created"; default: return "?";
    }
}
typedef struct { int last_n, stable; } GpReady;
// probe = GP_OK when pawn + inventory component exist (else the reason), n = inventory item count. Returns GP_OK when it is safe to grant.
static int gp_ready(GpReady *r, int probe, int n) {
    if (probe != GP_OK) { r->last_n = -1; r->stable = 0; return probe; }
    if (n <= 0) { r->last_n = -1; r->stable = 0; return GP_INV_EMPTY; }
    if (n == r->last_n) r->stable++; else { r->stable = 0; r->last_n = n; }
    return r->stable >= 1 ? GP_OK : GP_SETTLING;
}
typedef enum { GPV_WAIT, GPV_OWNED, GPV_ADD } GpVerdict;
static GpVerdict gp_decide(int ready, int owned_index) { return ready != GP_OK ? GPV_WAIT : owned_index >= 0 ? GPV_OWNED : GPV_ADD; }
// throttled "waiting" log: attempt 1, then every 20th (30 s at 1.5 s), and whenever the reason changes
static int gp_log_now(int attempts, int why, int last_why) { return attempts == 1 || attempts % 20 == 0 || why != last_why; }
