// Pure encoder / decoder of the input + state RECORDER (loader feature `input_rec`, glue inputrec.inc).
// No game access, no Windows, no allocation.
//
// A recording is JSONL, one object per line, schema IR_SCHEMA. The FIRST line is the session header (type "header") with the personal-data marker.
// Every other line has "type", "t" (seconds since the session started) and "f" (30 fps combat frame = floor(t * 30)):
//   key    {"k": engine key name, "e": 0 pressed 1 released 2 repeat 3 double click, "gp": 1 gamepad}       one line per edge (APlayerController::InputKey)
//   axis   {"k": axis key name, "v": value}  mouse axes: summed delta of one sample period, written when non-zero; stick axes: value, written on a change >= IR_AXIS_EPS
//   obs    {"fight", "round", "p": [IR_SIDE_N], "o": [IR_SIDE_N], "d": distance uu (-1 none), "lock": 0|1, "dir": 0 none 1 N .. 8 NW (stick / WASD, camera relative),
//           "on": opponent name, "op": opponent is a player 0|1, "pp" / "po": [x, y, yaw deg] of player / opponent (absent = unknown),
//           "pl" / "ol": the availability layer on top for player / opponent (last pushed and not popped; absent = none)}   written when it changed (ir_obs_changed) or every IR_HEARTBEAT_S
//   attack {"who": "p"|"o"|"n" (player / opponent / anyone else), "row": attack row}
//   hit    {"who": attacker, "oth": victim, "row", "x": damage, "a": outcome (ABS_HIT_*), "b": perfect link}
//   def    {"who": defender, "oth": attacker, "row", "a": ABS_DEF_* type, "x": frame in attack (-1 unknown)}
//   layer  {"who", "s": availability layer data name (PerfectAttackOnly = gold-link window, GenericAll = interruptible ...), "a": 1 pushed / 0 popped}
//   world  {"s": map path}       note {"s": free text (rotation, cap, errors)}
// Side vector "p" / "o" (IR_SIDE_N numbers, -1 = unknown): health, max health, guard (stamina), max guard, stance (0 FL 1 FR 2 BR 3 BL), style, attacking, guarding, down,
//   shards, attack frame (frames since that side's last attack start while attacking, else -1). The row of the running attack is in the "attack" events.
#ifndef INPUTREC_H
#define INPUTREC_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>

#define IR_SCHEMA 1                                                     // optional obs "pp" / "po" [x, y, yaw] + "pl" / "ol" top layer; header "pose_fields" marks them
#define IR_SIDE_N 11
#define IR_AXIS_EPS 0.02f
#define IR_HEARTBEAT_S 1.0
#define IR_PERSONAL "LOCAL ONLY: a personal input recording. Never commit, never upload, never share."
static const char *const IR_SIDE_FIELDS = "health,max_health,guard,max_guard,stance,style,attacking,guarding,down,shards,attack_frame";

typedef struct { float v[IR_SIDE_N]; } IrSide;
typedef struct { float x, y, yaw; int ok; } IrPose;                     // world x / y (uu) + yaw (deg), ok 0 = unknown (not written)
typedef struct { int fight, round; IrSide p, o; float d; int lock, dir; char on[64]; int op; IrPose pp, po; char pl[48], ol[48]; } IrObs;   // pl / ol: availability layer on top ("" none)
#define IR_POSE_EPS 25.0f
#define IR_YAW_EPS 10.0f
static float ir_yaw_diff(float a, float b) { float d = fmodf(fabsf(a - b), 360.0f); return d > 180.0f ? 360.0f - d : d; }
static int ir_pose_changed(const IrPose *a, const IrPose *b) {
    if (a->ok != b->ok) return 1; if (!a->ok) return 0;
    return fabsf(a->x - b->x) >= IR_POSE_EPS || fabsf(a->y - b->y) >= IR_POSE_EPS || ir_yaw_diff(a->yaw, b->yaw) >= IR_YAW_EPS;
}

static long ir_frame(double t) { return t < 0 ? 0 : (long)floor(t * 30.0 + 1e-9); }
// 8-way direction of a stick / WASD vector (x right, y forward), dead zone 0.3: 0 none, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW
static int ir_dir8(float x, float y) {
    if (x * x + y * y < 0.09f) return 0;
    double a = atan2((double)x, (double)y) * 57.29577951308232;     // 0 = forward, +90 = right
    if (a < 0) a += 360.0;
    return 1 + ((int)floor((a + 22.5) / 45.0) % 8);
}
// write one sample? relative axes (mouse): any non-zero sum; absolute axes (sticks): a change of at least IR_AXIS_EPS (also back to 0)
static int ir_axis_emit(int relative, float last, float v) { return relative ? v != 0.0f : fabsf(v - last) >= IR_AXIS_EPS; }
static int ir_axis_relative(const char *k) { return k && (!strcmp(k, "MouseX") || !strcmp(k, "MouseY") || !strcmp(k, "MouseWheelAxis")); }
// obs changed enough to write: any discrete field, health / guard by >= 0.5, shards by >= 0.05, distance by >= 10 uu, or a new opponent. attack_frame alone does not count.
static int ir_obs_changed(const IrObs *a, const IrObs *b) {
    if (a->fight != b->fight || a->round != b->round || a->lock != b->lock || a->dir != b->dir || a->op != b->op || strcmp(a->on, b->on)) return 1;
    if (fabsf(a->d - b->d) >= 10.0f) return 1;
    if (ir_pose_changed(&a->pp, &b->pp) || ir_pose_changed(&a->po, &b->po) || strcmp(a->pl, b->pl) || strcmp(a->ol, b->ol)) return 1;
    const IrSide *s[2][2] = { { &a->p, &b->p }, { &a->o, &b->o } };
    for (int k = 0; k < 2; k++) for (int i = 0; i < IR_SIDE_N - 1; i++) {
        float x = s[k][0]->v[i], y = s[k][1]->v[i], tol = (i <= 3) ? 0.5f : i == 9 ? 0.05f : 0.0f;
        if (tol == 0.0f ? x != y : fabsf(x - y) >= tol) return 1;
    }
    return 0;
}
// file-name safe character name (letters, digits, - _ ; others '_'), never empty
static void ir_safe_name(const char *in, char *out, int cap) {
    int n = 0; for (const char *p = in ? in : ""; *p && n < cap - 1; p++) { unsigned char c = (unsigned char)*p; out[n++] = (char)((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ? c : '_'); }
    if (!n && cap > 7) { memcpy(out, "unknown", 7); n = 7; } out[n] = 0;
}
// config input_rec_dir: a subfolder of the loader folder (letters, digits, _ - and at most one '\'), never absolute, never "..", never empty
static int ir_subdir_ok(const char *s) {
    int n = 0, sep = 0; if (!s || !*s || *s == '\\') return 0;
    for (const char *p = s; *p; p++, n++) { char c = *p; if (c == '\\') { if (++sep > 1 || p[1] == 0) return 0; continue; } if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return 0; }
    return n < 60;
}
// "<stamp>-<char>.jsonl" or "<stamp>-<char>-<part>.jsonl" (part >= 2 after a rotation)
static int ir_file_name(const char *stamp, const char *safe_char, int part, char *out, int cap) {
    return part > 1 ? snprintf(out, cap, "%s-%s-%d.jsonl", stamp, safe_char, part) : snprintf(out, cap, "%s-%s.jsonl", stamp, safe_char);
}
// size policy: 1 = rotate to a new part before writing `add` more bytes; the directory budget is checked separately (ir_dir_full: stop, never delete user data)
static int ir_should_rotate(long long file_bytes, long long add, long long cap) { return cap > 0 && file_bytes > 0 && file_bytes + add > cap; }
static int ir_dir_full(long long dir_bytes, long long cap) { return cap > 0 && dir_bytes >= cap; }

// Encoders return the length written without the newline they append; -1 = did not fit.
static int ir_put_str(char *o, int n, int cap, const char *s) {           // JSON string with escapes
    if (n < 0 || n >= cap) return -1; o[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (n + 7 >= cap) return -1;
        if (*p == '"' || *p == '\\') { o[n++] = '\\'; o[n++] = (char)*p; }
        else if (*p < 0x20) n += snprintf(o + n, cap - n, "\\u%04x", *p);
        else o[n++] = (char)*p;
    }
    if (n + 1 >= cap) return -1; o[n++] = '"'; o[n] = 0; return n;
}
static int ir_cat(char *o, int n, int cap, const char *fmt, ...) {
    if (n < 0 || n >= cap) return -1; va_list ap; va_start(ap, fmt); int k = vsnprintf(o + n, cap - n, fmt, ap); va_end(ap);
    return k < 0 || n + k >= cap ? -1 : n + k;
}
static int ir_end(char *o, int n, int cap) { n = ir_cat(o, n, cap, "}\n"); return n < 0 ? -1 : n - 1; }
static int ir_begin(char *o, int cap, const char *type, double t) { int n = ir_cat(o, 0, cap, "{\"type\":"); n = ir_put_str(o, n, cap, type); return ir_cat(o, n, cap, ",\"t\":%.3f,\"f\":%ld", t, ir_frame(t)); }
static int ir_enc_header(char *o, int cap, const char *loader, const char *package, const char *started, const char *character, int hz, int bot_reads_inputs) {
    int n = ir_cat(o, 0, cap, "{\"type\":\"header\",\"schema\":%d,\"personal_data\":", IR_SCHEMA); n = ir_put_str(o, n, cap, IR_PERSONAL);
    n = ir_cat(o, n, cap, ",\"loader\":"); n = ir_put_str(o, n, cap, loader); n = ir_cat(o, n, cap, ",\"package\":"); n = ir_put_str(o, n, cap, package);
    n = ir_cat(o, n, cap, ",\"started\":"); n = ir_put_str(o, n, cap, started); n = ir_cat(o, n, cap, ",\"char\":"); n = ir_put_str(o, n, cap, character);
    n = ir_cat(o, n, cap, ",\"hz\":%d,\"bot_reads_inputs\":%d,\"side_fields\":", hz, bot_reads_inputs ? 1 : 0); n = ir_put_str(o, n, cap, IR_SIDE_FIELDS);
    n = ir_cat(o, n, cap, ",\"pose_fields\":\"x,y,yaw\"");
    return ir_end(o, n, cap);
}
static int ir_enc_key(char *o, int cap, double t, const char *key, int ev, int gamepad) {
    int n = ir_begin(o, cap, "key", t); n = ir_cat(o, n, cap, ",\"k\":"); n = ir_put_str(o, n, cap, key); n = ir_cat(o, n, cap, ",\"e\":%d,\"gp\":%d", ev, gamepad ? 1 : 0); return ir_end(o, n, cap);
}
static int ir_enc_axis(char *o, int cap, double t, const char *key, float v) {
    int n = ir_begin(o, cap, "axis", t); n = ir_cat(o, n, cap, ",\"k\":"); n = ir_put_str(o, n, cap, key); n = ir_cat(o, n, cap, ",\"v\":%.4g", v); return ir_end(o, n, cap);
}
static int ir_put_side(char *o, int n, int cap, const char *name, const IrSide *s) {
    n = ir_cat(o, n, cap, ",\"%s\":[", name);
    for (int i = 0; i < IR_SIDE_N; i++) n = ir_cat(o, n, cap, i ? ",%.6g" : "%.6g", s->v[i]);
    return ir_cat(o, n, cap, "]");
}
static int ir_enc_obs(char *o, int cap, double t, const IrObs *b) {
    int n = ir_begin(o, cap, "obs", t); n = ir_cat(o, n, cap, ",\"fight\":%d,\"round\":%d", b->fight, b->round);
    n = ir_put_side(o, n, cap, "p", &b->p); n = ir_put_side(o, n, cap, "o", &b->o);
    n = ir_cat(o, n, cap, ",\"d\":%.1f,\"lock\":%d,\"dir\":%d,\"on\":", b->d, b->lock, b->dir); n = ir_put_str(o, n, cap, b->on); n = ir_cat(o, n, cap, ",\"op\":%d", b->op);
    if (b->pp.ok) n = ir_cat(o, n, cap, ",\"pp\":[%.1f,%.1f,%.1f]", b->pp.x, b->pp.y, b->pp.yaw);
    if (b->po.ok) n = ir_cat(o, n, cap, ",\"po\":[%.1f,%.1f,%.1f]", b->po.x, b->po.y, b->po.yaw);
    if (b->pl[0]) { n = ir_cat(o, n, cap, ",\"pl\":"); n = ir_put_str(o, n, cap, b->pl); }
    if (b->ol[0]) { n = ir_cat(o, n, cap, ",\"ol\":"); n = ir_put_str(o, n, cap, b->ol); }
    return ir_end(o, n, cap);
}
// generic event: who / oth ("p" "o" "n" or NULL), row (or NULL), ints a b, float x, text s (or NULL)
static int ir_enc_event(char *o, int cap, const char *type, double t, const char *who, const char *oth, const char *row, int a, int b, float x, const char *s) {
    int n = ir_begin(o, cap, type, t);
    if (who) { n = ir_cat(o, n, cap, ",\"who\":"); n = ir_put_str(o, n, cap, who); }
    if (oth) { n = ir_cat(o, n, cap, ",\"oth\":"); n = ir_put_str(o, n, cap, oth); }
    if (row) { n = ir_cat(o, n, cap, ",\"row\":"); n = ir_put_str(o, n, cap, row); }
    n = ir_cat(o, n, cap, ",\"a\":%d,\"b\":%d,\"x\":%.4g", a, b, x);
    if (s) { n = ir_cat(o, n, cap, ",\"s\":"); n = ir_put_str(o, n, cap, s); }
    return ir_end(o, n, cap);
}

// Decoder: only what the encoder writes (flat keys, numbers, strings, number arrays).
typedef struct { char type[16]; double t; long f; char k[48]; int e, gp; float v; IrObs obs; char who[4], oth[4], row[32], s[260]; int a, b; float x; int schema; char personal[160]; } IrRec;
static const char *ir_find(const char *line, const char *key) {           // pointer just after "key": (top level only is fine: keys are unique per line)
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\":", key); const char *p = strstr(line, pat); return p ? p + strlen(pat) : NULL;
}
static int ir_get_str(const char *line, const char *key, char *out, int cap) {
    const char *p = ir_find(line, key); int n = 0; out[0] = 0; if (!p || *p != '"') return 0; p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') { c = *p++; if (c == 'u') { unsigned v = 0; for (int i = 0; i < 4 && *p; i++, p++) v = v * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); c = (char)v; } else if (!c) break; }
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0; return *p == '"';
}
static int ir_get_num(const char *line, const char *key, double *v) { const char *p = ir_find(line, key); char *e; if (!p) return 0; *v = strtod(p, &e); return e != p; }
static int ir_get_side(const char *line, const char *key, IrSide *s) {
    const char *p = ir_find(line, key); if (!p || *p != '[') return 0; p++;
    for (int i = 0; i < IR_SIDE_N; i++) { char *e; s->v[i] = (float)strtod(p, &e); if (e == p) return 0; p = e; if (*p == ',') p++; else if (i < IR_SIDE_N - 1) return 0; }
    return *p == ']';
}
static int ir_get_pose(const char *line, const char *key, IrPose *q) {      // optional "key":[x,y,yaw]; ok stays 0 when absent / malformed
    const char *p = ir_find(line, key); float f[3]; memset(q, 0, sizeof *q); if (!p || *p != '[') return 0; p++;
    for (int i = 0; i < 3; i++) { char *e; f[i] = (float)strtod(p, &e); if (e == p) return 0; p = e; if (i < 2) { if (*p != ',') return 0; p++; } }
    if (*p != ']') return 0; q->x = f[0]; q->y = f[1]; q->yaw = f[2]; q->ok = 1; return 1;
}
// 1 = a valid line of a known type; 0 = malformed
static int ir_dec_line(const char *line, IrRec *r) {
    double v; memset(r, 0, sizeof *r);
    if (!line || line[0] != '{' || !ir_get_str(line, "type", r->type, sizeof r->type)) return 0;
    if (!strcmp(r->type, "header")) { if (!ir_get_num(line, "schema", &v)) return 0; r->schema = (int)v; return ir_get_str(line, "personal_data", r->personal, sizeof r->personal) && r->personal[0]; }
    if (!ir_get_num(line, "t", &r->t) || !ir_get_num(line, "f", &v)) return 0; r->f = (long)v;
    if (!strcmp(r->type, "key")) { if (!ir_get_str(line, "k", r->k, sizeof r->k) || !ir_get_num(line, "e", &v)) return 0; r->e = (int)v; if (ir_get_num(line, "gp", &v)) r->gp = (int)v; return 1; }
    if (!strcmp(r->type, "axis")) { if (!ir_get_str(line, "k", r->k, sizeof r->k) || !ir_get_num(line, "v", &v)) return 0; r->v = (float)v; return 1; }
    if (!strcmp(r->type, "obs")) {
        IrObs *o = &r->obs; if (!ir_get_side(line, "p", &o->p) || !ir_get_side(line, "o", &o->o) || !ir_get_num(line, "d", &v)) return 0; o->d = (float)v;
        if (ir_get_num(line, "fight", &v)) o->fight = (int)v; if (ir_get_num(line, "round", &v)) o->round = (int)v; if (ir_get_num(line, "lock", &v)) o->lock = (int)v;
        if (ir_get_num(line, "dir", &v)) o->dir = (int)v; if (ir_get_num(line, "op", &v)) o->op = (int)v; ir_get_str(line, "on", o->on, sizeof o->on);
        ir_get_pose(line, "pp", &o->pp); ir_get_pose(line, "po", &o->po); ir_get_str(line, "pl", o->pl, sizeof o->pl); ir_get_str(line, "ol", o->ol, sizeof o->ol); return 1;
    }
    static const char *const ev[] = { "attack", "hit", "def", "layer", "world", "note" };
    for (int i = 0; i < 6; i++) if (!strcmp(r->type, ev[i])) {
        ir_get_str(line, "who", r->who, sizeof r->who); ir_get_str(line, "oth", r->oth, sizeof r->oth); ir_get_str(line, "row", r->row, sizeof r->row); ir_get_str(line, "s", r->s, sizeof r->s);
        if (ir_get_num(line, "a", &v)) r->a = (int)v; if (ir_get_num(line, "b", &v)) r->b = (int)v; if (ir_get_num(line, "x", &v)) r->x = (float)v; return 1;
    }
    return 0;
}

// In-memory buffer; the loader flushes it to the file.
#define IR_BUF_CAP (64 * 1024)
#define IR_FLUSH_AT (48 * 1024)
typedef struct { char b[IR_BUF_CAP]; int n; long long dropped; } IrBuf;
// append one encoded line (len from an ir_enc_* call, + its '\n'); 0 = no room (caller flushes first, then retries once; still no room = dropped and counted)
static int ir_buf_put(IrBuf *q, const char *line, int len) {
    if (len < 0) { q->dropped++; return 0; }
    if (q->n + len + 1 > IR_BUF_CAP) return 0;
    memcpy(q->b + q->n, line, (size_t)len + 1); q->n += len + 1; return 1;
}
static int ir_buf_due(const IrBuf *q) { return q->n >= IR_FLUSH_AT; }
#endif
