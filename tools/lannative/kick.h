#ifndef LANNATIVE_KICK_H
#define LANNATIVE_KICK_H
// Host "Kick player" page: pure logic (rows, labels, the press-twice confirmation). kick.inc does the engine side.
// A row is one connected joiner: its player name and its address with the last octet hidden (the same mask as the join check log).
// Confirmation: Enter on a row arms it; Enter on the SAME player (name + address) within KICK_CONFIRM_MS kicks. Any other row, or a late press,
// arms again instead. Keyed by player, not by line, so a list that changes between the two presses can never kick someone else.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KICK_MAX        16
#define KICK_CONFIRM_MS 5000ull

typedef struct { char name[48], addr[40]; } KickRow;
typedef struct { char key[96]; uint64_t at; int armed; } KickConfirm;

static void kick_key(const KickRow *r, char *out, size_t cap) { snprintf(out, cap, "%s|%s", r->name, r->addr); }
// 1 = kick now (second press on the same player in time), 0 = armed (first press)
static int kick_confirm(KickConfirm *c, const KickRow *r, uint64_t now) {
    char k[96]; kick_key(r, k, sizeof k);
    if (c->armed && !strcmp(c->key, k) && now >= c->at && now - c->at <= KICK_CONFIRM_MS) { c->key[0] = 0; c->armed = 0; return 1; }
    snprintf(c->key, sizeof c->key, "%s", k); c->at = now; c->armed = 1; return 0;
}
static int kick_is_armed(const KickConfirm *c, const KickRow *r, uint64_t now) {
    char k[96]; kick_key(r, k, sizeof k); return c->armed && !strcmp(c->key, k) && now >= c->at && now - c->at <= KICK_CONFIRM_MS;
}
static void kick_label(const KickRow *r, int armed, char *out, size_t cap) {
    snprintf(out, cap, "%s - %s   %s", r->name[0] ? r->name : "(no name)", r->addr[0] ? r->addr : "?", armed ? "PRESS ENTER AGAIN TO KICK" : "(Enter = kick)");
}
// line shown when there is nobody to kick
static const char *kick_empty_line(int is_host) { return is_host ? "No joiners connected" : "Kick player: host only (you are not hosting)"; }
#endif
