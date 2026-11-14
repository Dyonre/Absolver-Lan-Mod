#ifndef LANNATIVE_UETEXT_H
#define LANNATIVE_UETEXT_H
// Path-based edits of Unreal "property text" (the format UProperty::ExportText / ImportText use), pure code.
// The game's data assets keep big nested animation containers as properties, e.g.
//   (m_Array[0]=(m_Array[0]=(m_Array=(AnimSequence'"/Game/A.A"',AnimSequence'"/Game/B.B"')),m_Array[1]=(...)))
// Structs are (name=value,...) (a static array member is written name[i]=value), a TArray is (v0,v1,...), values may be quoted strings or Class'"path"' references.
// A path walks that text:  m_Array[0].m_Array  = member m_Array[0] (static array element) then member m_Array (an array); a trailing / separate [2] picks the third element of a TArray.
// Operations:  set (replace one value)  |  append (add an element at the end of an array)  |  insert (add an element before position [i] of an array, i == count appends)
// Edits are small, so two mods appending to the same array both survive; the whole property is never replaced by one mod's copy.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int is_list; int s, e; int first, next; int ns, ne; } UNode;      // value span [s,e); name span [ns,ne) for a struct member (ne == ns otherwise)
typedef struct { UNode *n; int count, cap; const char *t; int len; char err[160]; } UDoc;

static int un_new(UDoc *d) {
    if (d->count == d->cap) { int c = d->cap ? d->cap * 2 : 1024; UNode *m = realloc(d->n, (size_t)c * sizeof *m); if (!m) return -1; d->n = m; d->cap = c; }
    UNode *x = &d->n[d->count]; memset(x, 0, sizeof *x); x->first = x->next = -1; return d->count++;
}
static int un_ident(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '[' || c == ']'; }
static int un_value(UDoc *d, int *pos, int depth);
static void un_skip(UDoc *d, int *p) { while (*p < d->len && (d->t[*p] == ' ' || d->t[*p] == '\t' || d->t[*p] == '\r' || d->t[*p] == '\n')) (*p)++; }
// end of a leaf token: the next top-level ',' or ')' (quotes and nested parentheses are skipped)
static int un_leaf_end(UDoc *d, int p) {
    int depth = 0;
    while (p < d->len) {
        char c = d->t[p];
        if (c == '"') { p++; while (p < d->len && d->t[p] != '"') { if (d->t[p] == '\\' && p + 1 < d->len) p++; p++; } if (p < d->len) p++; continue; }
        if (c == '\'') { p++; while (p < d->len && d->t[p] != '\'') p++; if (p < d->len) p++; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth == 0) return p; depth--; }
        else if (c == ',' && depth == 0) return p;
        p++;
    }
    return p;
}
static int un_value(UDoc *d, int *pos, int depth) {
    if (depth > 64) { snprintf(d->err, sizeof d->err, "nested too deeply"); return -1; }
    un_skip(d, pos);
    int me = un_new(d); if (me < 0) { snprintf(d->err, sizeof d->err, "out of memory"); return -1; }
    int start = *pos;
    if (*pos < d->len && d->t[*pos] == '(') {
        d->n[me].is_list = 1; d->n[me].s = start; (*pos)++;
        int last = -1;
        for (;;) {
            un_skip(d, pos);
            if (*pos >= d->len) { snprintf(d->err, sizeof d->err, "missing ')' for the '(' at %d", start); return -1; }
            if (d->t[*pos] == ')') { (*pos)++; break; }
            int ns = *pos, ne = *pos;
            while (ne < d->len && un_ident(d->t[ne])) ne++;
            int named = ne > ns && ne < d->len && d->t[ne] == '=';
            if (named) *pos = ne + 1; else ne = ns;
            int c = un_value(d, pos, depth + 1); if (c < 0) return -1;
            if (named) { d->n[c].ns = ns; d->n[c].ne = ne; }
            if (last < 0) d->n[me].first = c; else d->n[last].next = c; last = c;
            un_skip(d, pos);
            if (*pos < d->len && d->t[*pos] == ',') (*pos)++;
        }
        d->n[me].e = *pos; return me;
    }
    int e = un_leaf_end(d, *pos); int tr = e; while (tr > start && (d->t[tr - 1] == ' ' || d->t[tr - 1] == '\t')) tr--;
    d->n[me].s = start; d->n[me].e = tr; *pos = e; return me;
}
static int un_parse(UDoc *d, const char *text) {                    // root = node 0; returns 0 on success
    memset(d, 0, sizeof *d); d->t = text; d->len = (int)strlen(text); int p = 0;
    if (un_value(d, &p, 0) < 0) return -1;
    un_skip(d, &p); if (p < d->len) { snprintf(d->err, sizeof d->err, "extra text after the value (position %d)", p); return -1; }
    return 0;
}
static void un_free(UDoc *d) { free(d->n); memset(d, 0, sizeof *d); }
static int un_count(const UDoc *d, int node) { int n = 0; for (int c = d->n[node].first; c >= 0; c = d->n[c].next) n++; return n; }
static int un_nth(const UDoc *d, int node, int i) { int c = d->n[node].first; while (c >= 0 && i-- > 0) c = d->n[c].next; return c; }

// path segment: name [A-Za-z0-9_]* then an optional [index]
typedef struct { char name[80]; int idx; } UtSeg;                   // idx -1 = none
static int ut_segment(const char **pp, UtSeg *s, char *err, size_t cap) {
    const char *p = *pp; size_t n = 0; s->idx = -1;
    while (*p && *p != '.' && *p != '[') { if (n + 1 >= sizeof s->name) { snprintf(err, cap, "path segment too long"); return 0; } s->name[n++] = *p++; }
    s->name[n] = 0;
    if (*p == '[') { char *end; long v = strtol(p + 1, &end, 10); if (end == p + 1 || *end != ']' || v < 0) { snprintf(err, cap, "bad [index] in the path"); return 0; } s->idx = (int)v; p = end + 1; }
    if (!n && s->idx < 0) { snprintf(err, cap, "empty path segment"); return 0; }
    if (*p == '.') p++; else if (*p) { snprintf(err, cap, "unexpected '%c' in the path", *p); return 0; }
    *pp = p; return 1;
}
static int un_named(const UDoc *d, int node, const char *name, int idx) {          // member "name" or "name[idx]" of a struct node
    char full[100]; if (idx >= 0) snprintf(full, sizeof full, "%s[%d]", name, idx); else snprintf(full, sizeof full, "%s", name);
    size_t fl = strlen(full);
    for (int c = d->n[node].first; c >= 0; c = d->n[c].next) if (d->n[c].ne > d->n[c].ns && (size_t)(d->n[c].ne - d->n[c].ns) == fl && !strncmp(d->t + d->n[c].ns, full, fl)) return c;
    return -1;
}
// one step down from node; sets *via_array to the TArray node when the step ended in an element of a TArray member
static int un_step(const UDoc *d, int node, const UtSeg *s, char *err, size_t cap) {
    if (!d->n[node].is_list) { snprintf(err, cap, "'%s' is a plain value, it has no members or elements", s->name); return -1; }
    if (!s->name[0]) { int c = un_nth(d, node, s->idx); if (c < 0) snprintf(err, cap, "element [%d] does not exist (the array has %d)", s->idx, un_count(d, node)); return c; }
    int c = s->idx >= 0 ? un_named(d, node, s->name, s->idx) : -1;
    if (c >= 0) return c;                                                                // name[idx]= member (static array element)
    c = un_named(d, node, s->name, -1);
    if (c < 0) { snprintf(err, cap, "no member '%s' here", s->name); return -1; }
    if (s->idx < 0) return c;
    if (!d->n[c].is_list) { snprintf(err, cap, "'%s' is not an array", s->name); return -1; }
    int e = un_nth(d, c, s->idx); if (e < 0) snprintf(err, cap, "'%s' has %d element(s), [%d] does not exist", s->name, un_count(d, c), s->idx);
    return e;
}
static char *ut_splice(const char *t, int at, int cut, const char *ins) {           // t with [at, at+cut) replaced by ins
    size_t tl = strlen(t), il = strlen(ins); char *o = malloc(tl - (size_t)cut + il + 1); if (!o) return NULL;
    memcpy(o, t, (size_t)at); memcpy(o + at, ins, il); memcpy(o + at + il, t + at + cut, tl - (size_t)at - (size_t)cut + 1); return o;
}
typedef enum { UT_SET, UT_APPEND, UT_INSERT } UtOp;
// Starting text of a property for an object op: its exported text. A whole-property SET of a property that exports as NOTHING (an empty FText) starts from "" (it never reads the old value).
static char *ut_start_text(char *exported, int whole_set) { if (exported) return exported;   /* takes ownership */ return whole_set ? _strdup("") : NULL; }

// Applies one operation at path (relative to the property value; "" = the value itself) to text. Returns a malloc'd new text, or NULL with err set.
static char *ut_apply(const char *text, const char *path, UtOp op, const char *value, char *err, size_t cap) {
    UDoc d; err[0] = 0; if (un_parse(&d, text) < 0) { snprintf(err, cap, "the current value could not be read: %s", d.err); un_free(&d); return NULL; }
    int node = 0, parent = -1; UtSeg seg; const char *p = path; char *out = NULL;
    while (*p) {
        if (!ut_segment(&p, &seg, err, cap)) { un_free(&d); return NULL; }
        if (op == UT_INSERT && !*p && seg.idx >= 0) {                                    // last segment of an insert: the element to insert before
            UtSeg owner = seg; owner.idx = -1; int arr = seg.name[0] ? un_step(&d, node, &owner, err, cap) : node;
            if (arr < 0) { un_free(&d); return NULL; }
            if (!d.n[arr].is_list) { snprintf(err, cap, "'%s' is not an array", seg.name); un_free(&d); return NULL; }
            int cnt = un_count(&d, arr);
            if (seg.idx > cnt) { snprintf(err, cap, "cannot insert at [%d]: the array has %d element(s)", seg.idx, cnt); un_free(&d); return NULL; }
            if (seg.idx == cnt) { node = arr; op = UT_APPEND; break; }
            int el = un_nth(&d, arr, seg.idx); size_t vl = strlen(value); char *ins = malloc(vl + 2); if (!ins) { un_free(&d); return NULL; }
            memcpy(ins, value, vl); ins[vl] = ','; ins[vl + 1] = 0; out = ut_splice(text, d.n[el].s, 0, ins); free(ins); un_free(&d); return out;
        }
        parent = node; node = un_step(&d, node, &seg, err, cap); if (node < 0) { un_free(&d); return NULL; }
    }
    (void)parent;
    if (op == UT_SET) out = ut_splice(text, d.n[node].s, d.n[node].e - d.n[node].s, value);
    else {
        if (!d.n[node].is_list) { snprintf(err, cap, "cannot append: the value at that path is not an array"); un_free(&d); return NULL; }
        if (!d.n[node].first) { /* never: first is -1 for empty */ }
        if (d.n[node].first < 0) { size_t vl = strlen(value); char *ins = malloc(vl + 3); if (!ins) { un_free(&d); return NULL; } ins[0] = '('; memcpy(ins + 1, value, vl); ins[vl + 1] = ')'; ins[vl + 2] = 0; out = ut_splice(text, d.n[node].s, d.n[node].e - d.n[node].s, ins); free(ins); }
        else { size_t vl = strlen(value); char *ins = malloc(vl + 2); if (!ins) { un_free(&d); return NULL; } ins[0] = ','; memcpy(ins + 1, value, vl); ins[vl + 1] = 0; out = ut_splice(text, d.n[node].e - 1, 0, ins); free(ins); }
    }
    un_free(&d); if (!out) snprintf(err, cap, "out of memory");
    return out;
}
#endif
