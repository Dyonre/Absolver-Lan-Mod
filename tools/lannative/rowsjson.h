#ifndef LANNATIVE_ROWSJSON_H
#define LANNATIVE_ROWSJSON_H
// rows.json: row-level data edits for the game's DataTables (pure code: JSON reader, validation, conflict detection).
//   { "format": 1, "edits": [
//       { "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 8, "m_fDamageMax1": 12.5 } },
//       { "table": "attacks", "add": "MyJab", "copy_of": "004", "set": { "m_Anim": "/Game/Anim/X.X", "m_RealAttackName": "INVTEXT(\"My Jab\")" } } ] }
// Values are UE property text: numbers, "True"/"False", enum names ("BackRight"), INVTEXT("..."), /Game/... paths for soft references. A top-level array of edits is accepted too.
// Data assets (objects that are not tables) are edited with  { "object": "/Game/DB/Guard/GuardDB.GuardDB", "ops": [ { "append": "m_GuardAnimations.m_Array[0].m_Array[0].m_Array", "value": "AnimSequence'\"/Game/A.A\"'" } ] }
// where each op is "set", "append" or "insert" (before [i]) at a path into the property's text (see uetext.h); "value" is one UE value as text.
// An add may carry "learn": "always": the row then counts as learned (unlocked) for every character without editing the save (learn.inc).
// An "add" clones copy_of under the new name (skipped if that row already exists, e.g. another mod added it), then applies "set". Edits apply in mod load order, later wins.
// { "table": "EmoteDataTable", "remove": "076" } takes a row out of the table (for example one another mod added; a row that is not there is fine).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "uetext.h"

// ----- JSON
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct { JType t; int first, next; char *key, *str; } JNode;           // children and siblings are indices into JDoc.n (-1 = none)
typedef struct { JNode *n; int count, cap; const char *p, *begin; char err[160]; } JDoc;

static int jnew(JDoc *d, JType t) {
    if (d->count == d->cap) { int c = d->cap ? d->cap * 2 : 256; JNode *m = realloc(d->n, (size_t)c * sizeof *m); if (!m) return -1; d->n = m; d->cap = c; }
    JNode *n = &d->n[d->count]; n->t = t; n->first = n->next = -1; n->key = n->str = NULL; return d->count++;
}
static void jfail(JDoc *d, const char *what) {
    int line = 1; for (const char *q = d->begin; q < d->p; q++) if (*q == '\n') line++;
    snprintf(d->err, sizeof d->err, "%s (line %d)", what, line);
}
static void jskip(JDoc *d) { for (;;) { while (*d->p == ' ' || *d->p == '\t' || *d->p == '\r' || *d->p == '\n') d->p++;
    if (d->p[0] == '/' && d->p[1] == '/') { while (*d->p && *d->p != '\n') d->p++; continue; }                 // // comments are allowed: these files are written by hand
    break; } }
static int jutf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}
static unsigned jhex4(const char *p) { unsigned v = 0; for (int i = 0; i < 4; i++) { char c = p[i]; int x = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; if (x < 0) return 0xFFFFFFFFu; v = v << 4 | (unsigned)x; } return v; }
static char *jstring(JDoc *d) {                                    // at the opening quote; returns a malloc'd, unescaped string
    d->p++; size_t cap = 32, n = 0; char *s = malloc(cap); if (!s) return NULL;
    while (*d->p && *d->p != '"') {
        if (n + 8 >= cap) { cap *= 2; char *m = realloc(s, cap); if (!m) { free(s); return NULL; } s = m; }
        unsigned char c = (unsigned char)*d->p++;
        if (c < 0x20) { jfail(d, "control character in a string"); free(s); return NULL; }
        if (c != '\\') { s[n++] = (char)c; continue; }
        char e = *d->p++;
        switch (e) {
            case '"': s[n++] = '"'; break; case '\\': s[n++] = '\\'; break; case '/': s[n++] = '/'; break; case 'b': s[n++] = '\b'; break;
            case 'f': s[n++] = '\f'; break; case 'n': s[n++] = '\n'; break; case 'r': s[n++] = '\r'; break; case 't': s[n++] = '\t'; break;
            case 'u': { unsigned cp = jhex4(d->p); if (cp == 0xFFFFFFFFu) { jfail(d, "bad \\u escape"); free(s); return NULL; } d->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && d->p[0] == '\\' && d->p[1] == 'u') { unsigned lo = jhex4(d->p + 2); if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); d->p += 6; } }
                n += (size_t)jutf8(s + n, cp); break; }
            default: jfail(d, "bad escape in a string"); free(s); return NULL;
        }
    }
    if (*d->p != '"') { jfail(d, "unterminated string"); free(s); return NULL; }
    d->p++; s[n] = 0; return s;
}
static int jvalue(JDoc *d, int depth);
static int jcontainer(JDoc *d, int depth, char close, JType t) {
    int me = jnew(d, t); if (me < 0) return -1; d->p++;
    int last = -1; jskip(d);
    if (*d->p == close) { d->p++; return me; }
    for (;;) {
        jskip(d); char *key = NULL;
        if (t == J_OBJ) {
            if (*d->p != '"') { jfail(d, "expected a quoted property name"); return -1; }
            key = jstring(d); if (!key) return -1; jskip(d);
            if (*d->p != ':') { free(key); jfail(d, "expected ':'"); return -1; } d->p++; jskip(d);
        }
        int v = jvalue(d, depth + 1); if (v < 0) { free(key); return -1; }
        d->n[v].key = key;
        if (last < 0) d->n[me].first = v; else d->n[last].next = v; last = v;
        jskip(d);
        if (*d->p == ',') { d->p++; jskip(d); if (*d->p == close) { d->p++; return me; } continue; }       // a trailing comma is tolerated
        if (*d->p == close) { d->p++; return me; }
        jfail(d, t == J_OBJ ? "expected ',' or '}'" : "expected ',' or ']'"); return -1;
    }
}
static int jvalue(JDoc *d, int depth) {
    jskip(d);
    if (depth > 32) { jfail(d, "nested too deeply"); return -1; }
    char c = *d->p;
    if (c == '{') return jcontainer(d, depth, '}', J_OBJ);
    if (c == '[') return jcontainer(d, depth, ']', J_ARR);
    if (c == '"') { char *s = jstring(d); if (!s) return -1; int v = jnew(d, J_STR); if (v < 0) { free(s); return -1; } d->n[v].str = s; return v; }
    if (!strncmp(d->p, "true", 4) || !strncmp(d->p, "false", 5) || !strncmp(d->p, "null", 4)) {
        int isnull = d->p[0] == 'n', istrue = d->p[0] == 't'; size_t l = isnull || istrue ? 4 : 5; d->p += l;
        int v = jnew(d, isnull ? J_NULL : J_BOOL); if (v < 0) return -1;
        if (!isnull) d->n[v].str = _strdup(istrue ? "True" : "False"); return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *s = d->p; if (*d->p == '-') d->p++;
        while ((*d->p >= '0' && *d->p <= '9') || *d->p == '.' || *d->p == 'e' || *d->p == 'E' || *d->p == '+' || *d->p == '-') d->p++;
        size_t l = (size_t)(d->p - s); char *t = malloc(l + 1); if (!t) return -1; memcpy(t, s, l); t[l] = 0;
        int v = jnew(d, J_NUM); if (v < 0) { free(t); return -1; } d->n[v].str = t; return v;
    }
    jfail(d, "unexpected character"); return -1;
}
static int jdoc_parse(JDoc *d, const char *text) {                 // root = node 0; returns 0 on success
    memset(d, 0, sizeof *d); d->p = d->begin = text;
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) d->p += 3;
    jskip(d); if (!*d->p) { snprintf(d->err, sizeof d->err, "the file is empty"); return -1; }
    int root = jvalue(d, 0); if (root < 0) { if (!d->err[0]) jfail(d, "out of memory"); return -1; }
    jskip(d); if (*d->p) { jfail(d, "extra text after the end"); return -1; }
    return 0;
}
static void jdoc_free(JDoc *d) { for (int i = 0; i < d->count; i++) { free(d->n[i].key); free(d->n[i].str); } free(d->n); memset(d, 0, sizeof *d); }
static int jfind(const JDoc *d, int obj, const char *key) { for (int c = d->n[obj].first; c >= 0; c = d->n[c].next) if (d->n[c].key && !strcmp(d->n[c].key, key)) return c; return -1; }

// ----- ops
// Name limits: Blueprint-struct property names carry a GUID suffix (DT_LightSettings: HeightFogSunInscatteringColor_45_<32 hex> = 65 characters), so names may be long.
#define ROWS_NAME_MAX 192             // row / property / copy_of names: at most ROWS_NAME_MAX - 1 characters
#define ROWS_TABLE_MAX 96             // table names
typedef struct { int is_add, learn, is_remove; int mod; char table[ROWS_TABLE_MAX], row[ROWS_NAME_MAX], src[ROWS_NAME_MAX], prop[ROWS_NAME_MAX]; char *text; } RowOp;      // set: row.prop = text | add: clone src as row
typedef struct { int n; char line[8][300]; } RowsWarn;                          // fields skipped while reading a rows.json (n counts all, the first 8 are kept)
static void rows_warn_add(RowsWarn *w, const char *fmt, ...) {
    if (!w) return; if (w->n < 8) { va_list ap; va_start(ap, fmt); vsnprintf(w->line[w->n], sizeof w->line[0], fmt, ap); va_end(ap); } w->n++;
}
typedef struct { RowOp *a; int count, cap; } RowOps;
static int rowops_push(RowOps *o, const RowOp *op) {
    if (o->count == o->cap) { int c = o->cap ? o->cap * 2 : 512; RowOp *m = realloc(o->a, (size_t)c * sizeof *m); if (!m) return 0; o->a = m; o->cap = c; }
    o->a[o->count++] = *op; return 1;
}
static void rowops_free(RowOps *o) { for (int i = 0; i < o->count; i++) free(o->a[i].text); free(o->a); memset(o, 0, sizeof *o); }
static int rows_name_ok(const char *s, size_t cap) { size_t n = strlen(s); if (!n || n >= cap) return 0; for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x21 || s[i] == '"' || s[i] == '\\' || s[i] == '/' ) return 0; return 1; }
// row names may hold inner blanks (EmoteShop keys such as "Simple Chin", "Kneel Down" are real FNames); no leading / trailing blank
static int rows_row_ok(const char *s, size_t cap) { size_t n = strlen(s); if (!n || n >= cap || s[0] == ' ' || s[n - 1] == ' ') return 0; for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] < 0x21 && s[i] != ' ') || s[i] == '"' || s[i] == '\\' || s[i] == '/') return 0; return 1; }

typedef struct { int mod; int kind; char object[160]; char path[200]; char *value; } ObjOp;     // kind = UtOp: 0 set, 1 append, 2 insert
typedef struct { ObjOp *a; int count, cap; } ObjOps;
static int objops_push(ObjOps *o, const ObjOp *op) {
    if (o->count == o->cap) { int c = o->cap ? o->cap * 2 : 128; ObjOp *m = realloc(o->a, (size_t)c * sizeof *m); if (!m) return 0; o->a = m; o->cap = c; }
    o->a[o->count++] = *op; return 1;
}
static void objops_free(ObjOps *o) { for (int i = 0; i < o->count; i++) free(o->a[i].value); free(o->a); memset(o, 0, sizeof *o); }
static int objpath_ok(const char *s) {                                              // /Game/DB/Guard/GuardDB.GuardDB : one '.', no spaces
    size_t n = strlen(s); if (n < 5 || n >= 160 || s[0] != '/') return 0; int dots = 0;
    for (size_t i = 0; i < n; i++) { char c = s[i]; if (c == '.') dots++; else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '-')) return 0; }
    return dots == 1 && s[n - 1] != '.';
}
// path syntax: segments of name and/or [index] separated by '.'; the first segment must be a property name
static int objop_path_ok(const char *path, int kind, char *why, size_t cap) {
    if (!*path || strlen(path) >= 200) { snprintf(why, cap, "the path is empty or too long"); return 0; }
    const char *p = path; UtSeg seg; int first = 1; char e[100] = "";
    while (*p) { if (!ut_segment(&p, &seg, e, sizeof e)) { snprintf(why, cap, "%s", e); return 0; } if (first && !seg.name[0]) { snprintf(why, cap, "the path must start with a property name"); return 0; } first = 0; }
    if (kind == UT_INSERT) { size_t n = strlen(path); if (n < 3 || path[n - 1] != ']') { snprintf(why, cap, "an insert path must end with the [index] to insert before"); return 0; } }
    return 1;
}

// Converts a parsed rows.json into ops (appended to rows/objs, tagged with mod). Returns the number of ops.
// A bad single field of a "set" (a name that is too long or has bad characters, an array / object / null value) is SKIPPED with a line in warn; the rest of the file
// still loads. Anything else wrong (structure, table, row, object ops) is all-or-nothing: nothing is appended and err says why.
static int rows_from_json_w(const JDoc *d, int mod, RowOps *out, ObjOps *objs, char *err, size_t errcap, RowsWarn *warn);
static int rows_from_json(const JDoc *d, int mod, RowOps *out, ObjOps *objs, char *err, size_t errcap) { return rows_from_json_w(d, mod, out, objs, err, errcap, NULL); }
static int rows_from_json_w(const JDoc *d, int mod, RowOps *out, ObjOps *objs, char *err, size_t errcap, RowsWarn *warn) {
    int base = out->count, obase = objs->count; err[0] = 0;
    int list = 0;
    if (d->n[0].t == J_ARR) list = 0; else if (d->n[0].t == J_OBJ) { list = jfind(d, 0, "edits"); if (list < 0 || d->n[list].t != J_ARR) { snprintf(err, errcap, "no \"edits\" array"); return 0; } }
    else { snprintf(err, errcap, "the file must be an object with an \"edits\" array (or an array of edits)"); return 0; }
    int idx = 0;
    for (int e = d->n[list].first; e >= 0; e = d->n[e].next, idx++) {
        if (d->n[e].t != J_OBJ) { snprintf(err, errcap, "edit %d is not an object", idx); goto fail; }
        int jo = jfind(d, e, "object");
        if (jo >= 0) {                                                                  // ---- data asset edit
            int jp = jfind(d, e, "ops");
            if (d->n[jo].t != J_STR || !objpath_ok(d->n[jo].str)) { snprintf(err, errcap, "edit %d: \"object\" must be a path like /Game/DB/Guard/GuardDB.GuardDB", idx); goto fail; }
            if (jp < 0 || d->n[jp].t != J_ARR || d->n[jp].first < 0) { snprintf(err, errcap, "edit %d: \"ops\" must be a non-empty array", idx); goto fail; }
            int oi = 0;
            for (int o = d->n[jp].first; o >= 0; o = d->n[o].next, oi++) {
                if (d->n[o].t != J_OBJ) { snprintf(err, errcap, "edit %d, op %d is not an object", idx, oi); goto fail; }
                int js = jfind(d, o, "set"), ja = jfind(d, o, "append"), ji = jfind(d, o, "insert"), jv = jfind(d, o, "value");
                int which = (js >= 0) + (ja >= 0) + (ji >= 0);
                if (which != 1) { snprintf(err, errcap, "edit %d, op %d: give exactly one of \"set\", \"append\", \"insert\" (the path)", idx, oi); goto fail; }
                int jpath = js >= 0 ? js : ja >= 0 ? ja : ji; int kind = js >= 0 ? UT_SET : ja >= 0 ? UT_APPEND : UT_INSERT;
                if (d->n[jpath].t != J_STR) { snprintf(err, errcap, "edit %d, op %d: the path must be text", idx, oi); goto fail; }
                char why[120]; if (!objop_path_ok(d->n[jpath].str, kind, why, sizeof why)) { snprintf(err, errcap, "edit %d, op %d: %s", idx, oi, why); goto fail; }
                if (jv < 0 || d->n[jv].t == J_ARR || d->n[jv].t == J_OBJ || d->n[jv].t == J_NULL) { snprintf(err, errcap, "edit %d, op %d: \"value\" must be text, a number or true/false", idx, oi); goto fail; }
                { UDoc ud; int bad = un_parse(&ud, d->n[jv].str) != 0; if (bad) snprintf(why, sizeof why, "%s", ud.err); un_free(&ud);
                  if (bad) { snprintf(err, errcap, "edit %d, op %d: the value is not one valid property value (%s)", idx, oi, why); goto fail; } }
                ObjOp op; memset(&op, 0, sizeof op); op.mod = mod; op.kind = kind; strcpy(op.object, d->n[jo].str); strcpy(op.path, d->n[jpath].str); op.value = _strdup(d->n[jv].str);
                if (!op.value || !objops_push(objs, &op)) { free(op.value); snprintf(err, errcap, "out of memory"); goto fail; }
            }
            continue;
        }
        int jt = jfind(d, e, "table"), jr = jfind(d, e, "row"), ja = jfind(d, e, "add"), jc = jfind(d, e, "copy_of"), js = jfind(d, e, "set");
        if (jt < 0 || d->n[jt].t != J_STR || !rows_name_ok(d->n[jt].str, ROWS_TABLE_MAX)) { snprintf(err, errcap, "edit %d: \"table\" must be a short table name (for example attacks), or use \"object\" for a data asset", idx); goto fail; }
        RowOp op; memset(&op, 0, sizeof op); op.mod = mod; strcpy(op.table, d->n[jt].str);
        int jm = jfind(d, e, "remove");
        if (jm >= 0) {
            if (jr >= 0 || ja >= 0 || js >= 0 || jc >= 0) { snprintf(err, errcap, "edit %d: \"remove\" stands alone (no row / add / copy_of / set with it)", idx); goto fail; }
            if ((d->n[jm].t != J_STR && d->n[jm].t != J_NUM) || !rows_row_ok(d->n[jm].str, ROWS_NAME_MAX)) { snprintf(err, errcap, "edit %d: bad row name to remove", idx); goto fail; }
            op.is_remove = 1; strcpy(op.row, d->n[jm].str);
            if (!rowops_push(out, &op)) { snprintf(err, errcap, "out of memory"); goto fail; }
            continue;
        }
        if ((jr < 0) == (ja < 0)) { snprintf(err, errcap, "edit %d: give exactly one of \"row\" (edit an existing row) or \"add\" (new row name)", idx); goto fail; }
        const char *target = d->n[jr >= 0 ? jr : ja].str;
        if (d->n[jr >= 0 ? jr : ja].t != J_STR && d->n[jr >= 0 ? jr : ja].t != J_NUM) { snprintf(err, errcap, "edit %d: the row name must be text or a number", idx); goto fail; }
        if (!rows_row_ok(target, ROWS_NAME_MAX)) { snprintf(err, errcap, "edit %d: bad row name '%.80s' (empty, longer than %d characters, or a blank / quote / slash in it)", idx, target, ROWS_NAME_MAX - 1); goto fail; }
        if (ja >= 0) {
            if (jc < 0 || (d->n[jc].t != J_STR && d->n[jc].t != J_NUM) || !rows_row_ok(d->n[jc].str, ROWS_NAME_MAX)) { snprintf(err, errcap, "edit %d: \"add\" needs \"copy_of\" (an existing row to clone)", idx); goto fail; }
            op.is_add = 1; strcpy(op.row, target); strcpy(op.src, d->n[jc].str);
            int jl = jfind(d, e, "learn");
            if (jl >= 0) { if (d->n[jl].t != J_STR || strcmp(d->n[jl].str, "always")) { snprintf(err, errcap, "edit %d: \"learn\" must be \"always\" (or left out)", idx); goto fail; } op.learn = 1; }
            if (!rowops_push(out, &op)) { snprintf(err, errcap, "out of memory"); goto fail; }
        } else strcpy(op.row, target);
        if (js >= 0) {
            if (d->n[js].t != J_OBJ) { snprintf(err, errcap, "edit %d: \"set\" must be an object of property: value", idx); goto fail; }
            for (int p = d->n[js].first; p >= 0; p = d->n[p].next) {
                if (!rows_name_ok(d->n[p].key, ROWS_NAME_MAX)) { rows_warn_add(warn, "edit %d (row %.60s): property '%.80s%s' SKIPPED: the name is empty, longer than %d characters (%d), or has a blank / quote / slash in it",
                                                                               idx, op.row, d->n[p].key, strlen(d->n[p].key) > 80 ? "..." : "", ROWS_NAME_MAX - 1, (int)strlen(d->n[p].key)); continue; }
                if (d->n[p].t == J_ARR || d->n[p].t == J_OBJ || d->n[p].t == J_NULL) { rows_warn_add(warn, "edit %d (row %.60s): property %.120s SKIPPED: the value must be a string, number or true/false", idx, op.row, d->n[p].key); continue; }
                RowOp s; memset(&s, 0, sizeof s); s.mod = mod; strcpy(s.table, op.table); strcpy(s.row, op.row); strcpy(s.prop, d->n[p].key); s.text = _strdup(d->n[p].str);
                if (!s.text || !rowops_push(out, &s)) { free(s.text); snprintf(err, errcap, "out of memory"); goto fail; }
            }
        } else if (ja < 0) { snprintf(err, errcap, "edit %d: nothing to set", idx); goto fail; }
    }
    return (out->count - base) + (objs->count - obase);
fail:
    for (int i = base; i < out->count; i++) free(out->a[i].text);
    out->count = base;
    for (int i = obase; i < objs->count; i++) free(objs->a[i].value);
    objs->count = obase; return 0;
}

// ----- validation
// ImportText silently turns garbage numbers into 0 and reports success, so numbers and booleans are checked here before they reach the engine.
// kind is the property class name without the "Property" suffix (Int, Float, Bool, Byte, Enum, Name, Str, Text, SoftObject, ...). Returns 1 if the text is acceptable.
static int rowval_check(const char *kind, const char *text, char *why, size_t cap) {
    why[0] = 0;
    if (!text) { snprintf(why, cap, "no value"); return 0; }
    for (const char *p = text; *p; p++) if ((unsigned char)*p < 0x20) { snprintf(why, cap, "control character in the value"); return 0; }
    if (!strncmp(kind, "Int", 3) || !strncmp(kind, "UInt", 4)) {
        const char *p = text; if (*p == '-' || *p == '+') p++; if (!*p) { snprintf(why, cap, "'%s' is not a whole number", text); return 0; }
        for (; *p; p++) if (*p < '0' || *p > '9') { snprintf(why, cap, "'%s' is not a whole number", text); return 0; }
        return 1;
    }
    if (!strcmp(kind, "Float") || !strcmp(kind, "Double")) {
        char *end = NULL; double v = strtod(text, &end);
        if (end == text || *end || !isfinite(v)) { snprintf(why, cap, "'%s' is not a number", text); return 0; }
        return 1;
    }
    if (!strcmp(kind, "Bool")) {
        if (!_stricmp(text, "true") || !_stricmp(text, "false") || !strcmp(text, "1") || !strcmp(text, "0")) return 1;
        snprintf(why, cap, "'%s' is not True/False", text); return 0;
    }
    if (!strcmp(kind, "Byte") || !strcmp(kind, "Enum")) {
        if (!*text) { snprintf(why, cap, "empty value"); return 0; }
        const char *p = text; int digits = 1; for (; *p; p++) if (*p < '0' || *p > '9') digits = 0;
        if (digits) return 1;
        for (p = text; *p; p++) if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == ':')) { snprintf(why, cap, "'%s' is not a number or an enum name", text); return 0; }
        return 1;
    }
    return 1;
}

// ----- hard object references
// Pure text helpers for rows_objref_ok (rowedit.inc explains the boot hang).
static int rowval_is_hardref(const char *kind) { return !strcmp(kind, "Object") || !strcmp(kind, "Class") || !strcmp(kind, "Interface"); }
// "/Game/A/B.B", "\"/Game/A/B.B\"" or "Class'/Game/A/B.B'" -> full = "/Game/A/B.B", pkg = "/Game/A/B". Returns 0 if the text names no path (empty, None, a bare name).
static int rowval_objpath(const char *text, char *full, size_t fcap, char *pkg, size_t pcap) {
    const char *p = text; while (*p == ' ') p++;
    const char *q = strchr(p, '\''), *sl = strchr(p, '/'); if (q && (!sl || q < sl)) p = q + 1;
    if (*p == '"') p++;
    if (*p != '/') return 0;
    size_t n = 0; while (p[n] && p[n] != '"' && p[n] != '\'' && n + 1 < fcap) n++;
    while (n && p[n - 1] == ' ') n--;
    if (!n) return 0; memcpy(full, p, n); full[n] = 0;
    const char *dot = strchr(full, '.'); size_t pn = dot ? (size_t)(dot - full) : n; if (pn + 1 > pcap) return 0;
    memcpy(pkg, full, pn); pkg[pn] = 0; return 1;
}
// package name -> the file name the pak index uses, without the extension: /Game/DB/X -> ../../../Absolver/Content/DB/X, /Engine/Y -> ../../../Engine/Content/Y. 0 for /Script/ and others.
static int rowval_pakbase(const char *pkg, char *out, size_t cap) {
    if (!strncmp(pkg, "/Game/", 6)) return snprintf(out, cap, "../../../Absolver/Content/%s", pkg + 6) < (int)cap;
    if (!strncmp(pkg, "/Engine/", 8)) return snprintf(out, cap, "../../../Engine/Content/%s", pkg + 8) < (int)cap;
    return 0;
}

// ----- conflicts
typedef struct { char table[ROWS_TABLE_MAX], row[ROWS_NAME_MAX], prop[ROWS_NAME_MAX]; int first_mod, last_mod; int same; } RowConflict;      // same = every mod that set it used the same text
static const RowOp *g_cmp_ops;
static int rowop_key_cmp(const void *a, const void *b) {
    const RowOp *x = &g_cmp_ops[*(const int *)a], *y = &g_cmp_ops[*(const int *)b]; int c;
    if ((c = _stricmp(x->table, y->table))) return c; if ((c = _stricmp(x->row, y->row))) return c; if ((c = _stricmp(x->prop, y->prop))) return c;
    return *(const int *)a - *(const int *)b;                       // stable: file order (= load order) within one key
}
// Field-level conflicts: the same table.row.property set by more than one mod. Ops are in load order, so the last one wins. Adds of the same new row by two mods are reported with prop "(add)".
static int rows_conflicts(const RowOps *ops, RowConflict *out, int max) {
    int n = ops->count; if (!n) return 0;
    int *idx = malloc((size_t)n * sizeof *idx); if (!idx) return 0;
    int m = 0; for (int i = 0; i < n; i++) idx[m++] = i;
    g_cmp_ops = ops->a; qsort(idx, (size_t)m, sizeof *idx, rowop_key_cmp);
    int found = 0;
    for (int i = 0; i < m;) {
        const RowOp *a = &ops->a[idx[i]]; int j = i + 1, distinct = 0, same = 1;
        while (j < m && !_stricmp(ops->a[idx[j]].table, a->table) && !_stricmp(ops->a[idx[j]].row, a->row) &&
               (a->is_add == ops->a[idx[j]].is_add) && (a->is_remove == ops->a[idx[j]].is_remove) && (a->is_add || a->is_remove || !_stricmp(ops->a[idx[j]].prop, a->prop))) j++;
        int first = ops->a[idx[i]].mod, last = ops->a[idx[j - 1]].mod;
        for (int k = i + 1; k < j; k++) { const RowOp *p = &ops->a[idx[k]], *q = &ops->a[idx[k - 1]]; if (p->mod != q->mod) distinct = 1; if (!a->is_add && strcmp(p->text ? p->text : "", q->text ? q->text : "")) same = 0; }
        if (distinct && first != last) {
            if (found < max) { strcpy(out[found].table, a->table); strcpy(out[found].row, a->row); strcpy(out[found].prop, a->is_add ? "(add)" : a->is_remove ? "(remove)" : a->prop); out[found].first_mod = first; out[found].last_mod = last; out[found].same = a->is_add ? 0 : a->is_remove ? 1 : same; }
            found++;
        }
        i = j;
    }
    free(idx); return found;
}

typedef struct { char object[160], path[200]; int first_mod, last_mod, same; } ObjConflict;
static const ObjOps *g_cmp_objs;
static int objop_key_cmp(const void *a, const void *b) {
    const ObjOp *x = &g_cmp_objs->a[*(const int *)a], *y = &g_cmp_objs->a[*(const int *)b]; int c;
    if ((c = strcmp(x->object, y->object))) return c; if ((c = strcmp(x->path, y->path))) return c; return *(const int *)a - *(const int *)b;
}
// Only "set" can conflict: appends and inserts from different mods combine. Reports (object, path) set by more than one mod; same = every set used the same value.
static int objops_conflicts(const ObjOps *ops, ObjConflict *out, int max) {
    int n = 0; int *idx = malloc((size_t)(ops->count ? ops->count : 1) * sizeof *idx); if (!idx) return 0;
    for (int i = 0; i < ops->count; i++) if (ops->a[i].kind == UT_SET) idx[n++] = i;
    g_cmp_objs = ops; qsort(idx, (size_t)n, sizeof *idx, objop_key_cmp); int found = 0;
    for (int i = 0; i < n;) {
        const ObjOp *a = &ops->a[idx[i]]; int j = i + 1; while (j < n && !strcmp(ops->a[idx[j]].object, a->object) && !strcmp(ops->a[idx[j]].path, a->path)) j++;
        int first = ops->a[idx[i]].mod, last = ops->a[idx[j - 1]].mod, distinct = 0, same = 1;
        for (int k = i + 1; k < j; k++) { if (ops->a[idx[k]].mod != ops->a[idx[k - 1]].mod) distinct = 1; if (strcmp(ops->a[idx[k]].value, ops->a[idx[k - 1]].value)) same = 0; }
        if (distinct && first != last) { if (found < max) { strcpy(out[found].object, a->object); strcpy(out[found].path, a->path); out[found].first_mod = first; out[found].last_mod = last; out[found].same = same; } found++; }
        i = j;
    }
    free(idx); return found;
}
#endif
