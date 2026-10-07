// LanNative M3.5 LAN-PvP guard: pure decision logic (the hook itself lives in lannative.c).
//
// Why it exists: the PvP player-controller Blueprint (AdversarialFightingPlayerControllerBP, ReceiveBeginPlay) calls
// UThePlainesGameInstance::BPF_ReturnToPreviousMap when `!AllowOfflinePlay && !joiningByInvite && OnlineStatus != 3`. A -NoEAC (offline)
// game always satisfies that, so every 1v1/3v3 world sends its players straight back to the main map (seen in the M3.3 logs).
// The guard skips that one call, but only (a) while the loaded world is a duel map and (b) shortly after a client travel was requested,
// so the quit menu, reward screen and any later return still work.
#ifndef LANNATIVE_PVPGATE_H
#define LANNATIVE_PVPGATE_H

#include <stdint.h>
#include <string.h>

#define PVP_WINDOW_MS 45000ull   // a duel map takes a few seconds to load; a joiner may take longer than the host

static int pvp_ci_contains(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == n) return 1;
    }
    return 0;
}

// World path as logged by LanNative, e.g. /Game/Maps/MainMap_GameModes/1v1/OPTI_GM_1v1_City_Coliseum.OPTI_GM_1v1_City_Coliseum
static int pvp_path_is_duel(const char *world_path) {
    return pvp_ci_contains(world_path, "/MainMap_GameModes/1v1/") || pvp_ci_contains(world_path, "/MainMap_GameModes/3v3/");
}

// The newest of two event timestamps (0 = that event has not happened). The guard's window starts at the latest travel request OR the latest
// change in the number of connected peers: the M3.5 test showed the HOST's own return call fires again 78 s later, when a joiner's player
// controller begins play on the host (the Blueprint runs on the server for every controller).
static uint64_t pvp_latest(uint64_t a, uint64_t b) { return a > b ? a : b; }

// 1 = skip the call. last_travel_ms == 0 means no triggering event has been seen yet.
static int pvp_should_suppress(int enabled, int duel_map, uint64_t now_ms, uint64_t last_travel_ms, uint64_t window_ms) {
    if (!enabled || !duel_map || !last_travel_ms) return 0;
    if (now_ms < last_travel_ms) return 0;                 // clock went backwards: do not guess
    return (now_ms - last_travel_ms) <= window_ms;
}

static int pvp_path_is_3v3(const char *world_path) { return pvp_ci_contains(world_path, "/MainMap_GameModes/3v3/"); }

// M3.9 auto-leave. In a duel map the offline game never notices that the other player left: the body stays until F6. When a peer WAS connected
// in this duel world and the connection count has been 0 for AUTOLEAVE_GRACE_MS, this asks for the same travel F6 does. It never fires when a
// travel was requested after the peer was last seen (the player is already leaving), and starts over in every new world.
#define AUTOLEAVE_GRACE_MS 3000ull
#define AUTOLEAVE_QUIET_MS 10000ull   // M4.1: a connection that has received nothing for this long counts as gone (the game itself waits ~30 s)
enum { AL_NONE = 0, AL_ANNOUNCE = 1, AL_LEAVE = 2 };
typedef struct { int had_peer; uint64_t seen_ms, gone_ms; } AutoLeave;
static void autoleave_reset(AutoLeave *s) { memset(s, 0, sizeof *s); }
static int autoleave_step(AutoLeave *s, uint64_t now, int peers, int duel_map, uint64_t last_travel_ms) {
    if (!duel_map) { autoleave_reset(s); return AL_NONE; }
    if (peers > 0) { s->had_peer = 1; s->seen_ms = now; s->gone_ms = 0; return AL_NONE; }
    if (!s->had_peer) return AL_NONE;
    if (last_travel_ms > s->seen_ms) { autoleave_reset(s); return AL_NONE; }   // already travelling
    if (!s->gone_ms) { s->gone_ms = now ? now : 1; return AL_ANNOUNCE; }
    if (now >= s->gone_ms && now - s->gone_ms >= AUTOLEAVE_GRACE_MS) { autoleave_reset(s); return AL_LEAVE; }
    return AL_NONE;
}

#endif
