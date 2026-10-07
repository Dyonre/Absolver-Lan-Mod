// LanNative M3.3 telemetry: world-change events, a session snapshot (frame time, per-connection net stats, per-player position/ping) written to
// CSV files, and a log marker key. Read-only: it never writes into game memory. Everything here is pure helpers + the module's entry points;
// the engine reads live in lan_telemetry.c and go through the same Api table as lan_logic.c so the offline test can drive them.
#ifndef LANNATIVE_TELEMETRY_H
#define LANNATIVE_TELEMETRY_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lan_logic.h"

// engine layout (PDB, Absolver 1.31 b1.25_575; work/structs_net.txt)
#define OFF_W_NETDRIVER      0x38    // UWorld::NetDriver
#define OFF_ND_SERVERCONN    0x78    // UNetDriver::ServerConnection (non-null on a client)
#define OFF_ND_CLIENTCONNS   0x80    // UNetDriver::ClientConnections (TArray: Data, Num at +8)
#define OFF_ND_INBPS         0x154   // UNetDriver::InBytesPerSecond
#define OFF_ND_OUTBPS        0x158
#define OFF_ND_INPKTLOST     0x17C
#define OFF_ND_OUTPKTLOST    0x180
#define OFF_NC_OWNINGACTOR   0xE8    // UNetConnection::OwningActor
#define OFF_NC_MAXPACKET     0xF0
#define OFF_NC_STATE         0x17C   // EConnectionState (int32): 0 invalid, 1 closed, 2 pending, 3 open
#define OFF_NC_BESTLAG       0x3AC   // float seconds
#define OFF_NC_AVGLAG        0x3B0
#define OFF_NC_INBYTES       0x3E4   // int32 counters
#define OFF_NC_OUTBYTES      0x3E8
#define OFF_NC_INPKTS        0x3EC
#define OFF_NC_OUTPKTS       0x3F0
#define OFF_NC_INBPS         0x3F4
#define OFF_NC_OUTBPS        0x3F8
#define OFF_NC_INPPS         0x3FC
#define OFF_NC_OUTPPS        0x400
#define OFF_NC_INLOST        0x404
#define OFF_NC_OUTLOST       0x408
#define OFF_PS_PING          0x3A4   // APlayerState::Ping (uint8, ping in ms / 4)
#define OFF_W_PAWNLIST       0x198   // UWorld::PawnList (TArray<TWeakObjectPtr<APawn>>): every pawn this PC knows about, replicated ones included
#define OFF_PAWN_PLAYERSTATE 0x3C0   // APawn::PlayerState (null for NPCs)

// M3.7 match probe (duel worlds only; PDB: work/structs_adv.txt)
#define OFF_GM_MATCHSTATE      0x430   // AGameMode::MatchState (FName), server only
#define OFF_GM_NUMPLAYERS      0x440   // AGameMode::NumPlayers
#define OFF_ADVGM_TIMEOUT      0x578   // AAdversarialGameMode::m_fTimeoutWaitForClients (float)
#define OFF_ADVGM_READYWAIT    0x57C   // m_bReadyToWaitForClients
#define OFF_ADVGM_NBCLIENTS    0x57D   // m_uiNbConnectedClients
#define OFF_ADVGM_NBPLAYERS    0x57E   // m_uiNbConnectedPlayers
#define OFF_ADVGM_RETURNASKED  0x57F   // m_bReturnToPreviousMapAsked
#define OFF_GS_PLAYERARRAY     0x3B8   // AGameStateBase::PlayerArray (TArray: Data, Num at +8)
#define OFF_ADVGS_VALID        0x780   // AAdversarialGameState::m_bMatchValidity
#define OFF_ADVGS_READYEND     0x789   // m_bReadyToEndMatch
#define OFF_ADVGS_COUNTDOWN    0x7A0   // m_bCountDownOver
#define OFF_ADVGS_CACHESTATE   0x7B0   // m_CacheMatchState (FName, replicated)
#define OFF_ADVGS_EXPECTED     0x7B8   // m_uiNbExpectedPlayers
#define OFF_ADVGS_CONNECTED    0x7B9   // m_uiNbConnectedPlayerCount
#define OFF_ADVGS_ONCE         0x7C9   // m_bMatchInProgressOnce
#define OFF_ADVGS_FULL         0x7CB   // m_bMatchFull
#define OFF_FPC_PAWNINIT       0x1914  // AFightingPlayerController::m_bPawnInitialized
#define OFF_FPC_GAMEINIT       0x1915  // m_bGameInitialized
#define OFF_FPC_WAITFOR        0x1AF9  // m_eGameModeAnswerWaitingFor
#define OFF_FPC_LOADINGIDX     0x1B09  // m_iLoadingMenuIndex
#define OFF_ADVPS_ANSWER       0x470   // AAdversarialPlayerState::m_eLastAnswer

#define TEL_OFF     0
#define TEL_BASIC   1
#define TEL_VERBOSE 2

#define TEL_CSV_FRAMES  0
#define TEL_CSV_CONNS   1
#define TEL_CSV_PLAYERS 2
#define TEL_HDR_FRAMES  "wall,tick_ms,pid,role,map,frames,avg_ms,max_ms,drv_in_bps,drv_out_bps,drv_in_lost,drv_out_lost,conns,players,npcs"
#define TEL_HDR_CONNS   "wall,tick_ms,pid,role,map,idx,conn,state,avg_lag_ms,best_lag_ms,in_pps,out_pps,in_bps,out_bps,in_lost,out_lost,in_pkts,out_pkts,in_bytes,out_bytes,max_packet"
#define TEL_HDR_PLAYERS "wall,tick_ms,pid,role,map,pawn,local,has_ctrl,pawn_role,pawn_remote,x,y,z,ping_ms,respawn_zone"

typedef struct { int level; int interval_ms; } TelCfg;

// LanNative\telemetry.txt: "off", "basic" (default) or "verbose", optionally followed by the snapshot interval in seconds ("basic 10").
// Returns 1 if the text was understood; otherwise cfg holds the default (basic, 5 s).
static int tel_parse_config(const char *text, TelCfg *cfg) {
    cfg->level = TEL_BASIC; cfg->interval_ms = 5000;
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    char word[16]; int n = 0;
    while (text[n] && text[n] != ' ' && text[n] != '\t' && text[n] != '\r' && text[n] != '\n' && n < 15) { word[n] = (char)((text[n] >= 'A' && text[n] <= 'Z') ? text[n] + 32 : text[n]); n++; }
    word[n] = 0;
    int ok = 1;
    if (strcmp(word, "off") == 0) { cfg->level = TEL_OFF; cfg->interval_ms = 0; }
    else if (strcmp(word, "basic") == 0) { cfg->level = TEL_BASIC; cfg->interval_ms = 5000; }
    else if (strcmp(word, "verbose") == 0) { cfg->level = TEL_VERBOSE; cfg->interval_ms = 1000; }
    else ok = 0;
    const char *p = text + n;
    while (ok && (*p == ' ' || *p == '\t')) p++;
    if (ok && cfg->level != TEL_OFF && *p >= '0' && *p <= '9') {
        int secs = 0;
        while (*p >= '0' && *p <= '9' && secs < 1000) secs = secs * 10 + (*p++ - '0');
        if (secs < 1) secs = 1;
        if (secs > 60) secs = 60;
        cfg->interval_ms = secs * 1000;
    }
    return ok;
}

// Says where a call into UEngine::SetClientTravel came from: "LanNative", "exe+0xRVA" (RVA of the return address in the game image), or "other(0x...)".
static void tel_describe_caller(uint64_t ret, uint64_t exe_base, uint64_t exe_size, uint64_t self_base, uint64_t self_size, char *out, int cap) {
    if (self_size && ret >= self_base && ret < self_base + self_size) snprintf(out, (size_t)cap, "LanNative");
    else if (exe_size && ret >= exe_base && ret < exe_base + exe_size) snprintf(out, (size_t)cap, "exe+0x%llX", (unsigned long long)(ret - exe_base));
    else snprintf(out, (size_t)cap, "other(0x%llX)", (unsigned long long)ret);
}

void T_init(const Api *api, const TelCfg *cfg, uint32_t pid);
void T_frame(float dt_seconds);     // every game frame (cheap accumulation only)
void T_tick(uint64_t now_ms);       // every game frame; throttled inside (world check 2/s, snapshot every cfg interval)
uint64_t T_quiet_ms(uint64_t now_ms); // M4.1: how long no packet has arrived on any connection (0 = no network session). Read-only, per frame.
void T_mark(void);                  // F8: a numbered marker line with position, for lining up two PCs' logs
int  T_peers_now(void);             // remote peers right now (host: client connections; client: 1 with a server connection; else 0). Read-only.

#endif
