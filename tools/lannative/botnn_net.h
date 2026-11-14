// botnn_net.h - load and run the neural-network bot's policy file ("policy.bin") in the game adapter. Pure C (stdio + math), no game access.
// File: "ABNN", int32 version 1, n_obs, n_hidden, n_act, then float32 W1[h][obs] b1[h] W2[h][h] b2[h] Wpi[act][h] bpi[act] (tanh hidden layers).
// Actions = NNA_* below, in the order the policy was trained with (the file's n_act must equal NNA_N).
#ifndef BOTNN_NET_H
#define BOTNN_NET_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bot_cmd.h"

enum { NNA_WAIT = 0, NNA_ATK1, NNA_ATK2, NNA_FEINT, NNA_GUARD_ON, NNA_GUARD_OFF, NNA_DODGE_BACK, NNA_DODGE_LEFT, NNA_DODGE_RIGHT, NNA_DODGE_FWD, NNA_STANCE_FL, NNA_STANCE_FR, NNA_STANCE_BR, NNA_STANCE_BL,
       NNA_ABILITY, NNA_MOVE_FWD, NNA_MOVE_BACK, NNA_MOVE_STOP, NNA_LOCK_TOGGLE, NNA_POWER0, NNA_POWER1, NNA_DRAW, NNA_ABILITY_V1, NNA_ABILITY_V2, NNA_ABILITY_V3, NNA_STRAFE,
       NNA_ATK1_FEINT, NNA_ATK2_FEINT, NNA_STRAFE_R, NNA_MOVE_FL, NNA_MOVE_FR, NNA_MOVE_BL, NNA_MOVE_BR, NNA_WALK_SLOW, NNA_RUSH,
       NNA_DODGE_FL, NNA_DODGE_FR, NNA_DODGE_BL, NNA_DODGE_BR, NNA_N };   // 39 actions
typedef struct { int ok, n_obs, n_hid, n_act; float *w1, *b1, *w2, *b2, *w3, *b3; char path[260]; } NnNet;

static void nn_free(NnNet *n) { free(n->w1); memset(n, 0, sizeof *n); }
// returns 1 = loaded; why = reason when not
static int nn_load(NnNet *n, const char *path, char *why, int cap) {
    nn_free(n); FILE *f = fopen(path, "rb"); if (!f) { snprintf(why, cap, "cannot open %s", path); return 0; }
    char mg[4]; int hd[4]; if (fread(mg, 1, 4, f) != 4 || memcmp(mg, "ABNN", 4) || fread(hd, 4, 4, f) != 4 || hd[0] != 1) { fclose(f); snprintf(why, cap, "%s: not an ABNN v1 file", path); return 0; }
    n->n_obs = hd[1]; n->n_hid = hd[2]; n->n_act = hd[3];
    if (n->n_obs <= 0 || n->n_obs > 4096 || n->n_hid <= 0 || n->n_hid > 4096 || n->n_act != NNA_N) { fclose(f); snprintf(why, cap, "%s: dims obs %d hidden %d act %d (want act %d)", path, n->n_obs, n->n_hid, n->n_act, NNA_N); return 0; }
    size_t sz = (size_t)n->n_hid * n->n_obs + n->n_hid + (size_t)n->n_hid * n->n_hid + n->n_hid + (size_t)n->n_act * n->n_hid + n->n_act;
    float *w = (float *)malloc(sz * sizeof(float)); if (!w) { fclose(f); snprintf(why, cap, "out of memory"); return 0; }
    size_t got = fread(w, sizeof(float), sz, f); fclose(f); if (got != sz) { free(w); snprintf(why, cap, "%s: %zu of %zu weights", path, got, sz); return 0; }
    n->w1 = w; n->b1 = n->w1 + (size_t)n->n_hid * n->n_obs; n->w2 = n->b1 + n->n_hid; n->b2 = n->w2 + (size_t)n->n_hid * n->n_hid; n->w3 = n->b2 + n->n_hid; n->b3 = n->w3 + (size_t)n->n_act * n->n_hid;
    snprintf(n->path, sizeof n->path, "%s", path); n->ok = 1; return 1;
}
static void nn_layer(const float *w, const float *b, const float *x, int nin, int nout, float *y, int act_tanh) {
    for (int o = 0; o < nout; o++) { const float *r = w + (size_t)o * nin; float s = b[o]; for (int i = 0; i < nin; i++) s += r[i] * x[i]; y[o] = act_tanh ? tanhf(s) : s; }
}
// logits[n_act] for obs[n_obs] (hidden <= 1024)
static void nn_forward(const NnNet *n, const float *obs, float *logits) {
    float h1[1024], h2[1024]; int h = n->n_hid > 1024 ? 1024 : n->n_hid;
    nn_layer(n->w1, n->b1, obs, n->n_obs, h, h1, 1); nn_layer(n->w2, n->b2, h1, h, h, h2, 1); nn_layer(n->w3, n->b3, h2, h, n->n_act, logits, 0);
}
// sample a legal action from the masked softmax (the policy is stochastic, like in training); -1 when nothing is legal
static int nn_sample(const float *logits, const unsigned char *mask, int n, BotRng *r) {
    float mx = -1e30f; for (int i = 0; i < n; i++) if (mask[i] && logits[i] > mx) mx = logits[i];
    if (mx <= -1e29f) return -1;
    double tot = 0, p[64]; for (int i = 0; i < n && i < 64; i++) { p[i] = mask[i] ? exp((double)(logits[i] - mx)) : 0.0; tot += p[i]; }
    double x = bc_rand(r) * tot, acc = 0; for (int i = 0; i < n && i < 64; i++) { acc += p[i]; if (mask[i] && x < acc) return i; }
    for (int i = n - 1; i >= 0; i--) if (mask[i]) return i; return -1;
}
#endif
