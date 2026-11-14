// evdedupe.h - "has this one been reported already?" for events the game repeats (pure logic).
// OrderAttack::OnStart runs ~30x per real attack (network prediction re-simulates the order on copies that keep m_iOriginalLaunchTime),
// so on_attack_start keys an attack by (character, attack FName, original launch time) and reports each key once.
// A 64-entry ring with a linear scan: enough for a handful of fighters; grow it if many characters attack within one launch time.
#ifndef EVDEDUPE_H
#define EVDEDUPE_H
#include <stdint.h>
#define EVD_N 64
typedef struct { uint64_t k[EVD_N]; unsigned next; } EvDedupe;
static uint64_t evd_key(uint64_t a, uint64_t b, uint64_t c) {           // FNV-1a over the three words; never 0 (0 = empty slot)
    uint64_t v[3] = { a, b, c }, h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 8; j++) { h ^= (v[i] >> (8 * j)) & 0xFF; h *= 0x100000001b3ull; }
    return h ? h : 1;
}
static int evd_seen(EvDedupe *d, uint64_t key) {                          // 1 = already reported; 0 = new (and remembered now)
    for (unsigned i = 0; i < EVD_N; i++) if (d->k[i] == key) return 1;
    d->k[d->next++ % EVD_N] = key; return 0;
}
// on_hit ABS_HIT_PARRIED roles: UDefenseComponent::NotifyParrySuccessful runs on the ATTACKER's defense component (`this`, called by
// HandleParried) and its actor argument is the PARRIER. on_hit's victim is the character the attack was aimed at = the parrier.
static void ev_parry_roles(void *this_owner, void *actor_arg, void **victim, void **attacker) { *victim = actor_arg; *attacker = this_owner; }
#endif
