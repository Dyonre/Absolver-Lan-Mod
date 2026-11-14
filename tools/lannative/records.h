// records.h - flats, templates and inheritance on top of rows.json. Pure code, no engine calls. Works on the parsed JDoc of
// rowsjson.h and rewrites it IN PLACE into plain rows.json edits, so rowsjson.h / rowedit.inc apply it unchanged.
//   "flats":     { "Weapon.Light.Startup": 15, ... }                        named constants; a value that is exactly "$Name" becomes the flat ("$$x" = the text "$x")
//   "templates": [ { "id": "WeaponLight", "base": "Other", "set": {...} } ]  field sets; "base" = another template (chain <= REC_DEPTH, the child wins)
//   an edit's "base": "<template id>"                                        the template's resolved fields go first, the edit's own "set" wins
// Flats and templates are GLOBAL over all enabled mods in load order: rec_collect every mod first (a later definition of the same name wins, both are recorded),
// then rec_expand each mod's document. Problems skip one field (unknown flat) or one edit (missing / cyclic base) and are reported through RowsWarn; the rest loads.
// A document without flats, templates, base or "$" values is never touched (rec_uses() == 0).
#ifndef LANNATIVE_RECORDS_H
#define LANNATIVE_RECORDS_H
#include "rowsjson.h"

#define REC_NAME_MAX 96
#define REC_MOD_MAX 64
#define REC_DEPTH 16
#define REC_FIELDS 128
typedef struct { char name[REC_NAME_MAX], mod[REC_MOD_MAX], prev[REC_MOD_MAX]; char *val; int overrides; } RecFlat;                  // prev = the mod overridden last
typedef struct { char id[REC_NAME_MAX], base[REC_NAME_MAX], mod[REC_MOD_MAX], prev[REC_MOD_MAX]; int n, overrides; char *key[REC_FIELDS], *val[REC_FIELDS]; } RecTpl;
typedef struct { RecFlat *f; int nf, capf; RecTpl *t; int nt, capt; int inherited, fields_inherited, subs, skipped_edits, skipped_fields; } RecCtx;

static int rec_name_ok(const char *s) { size_t n = s ? strlen(s) : 0; if (!n || n >= REC_NAME_MAX) return 0; for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_' || *s == '.' || *s == '-')) return 0; return 1; }
static int rec_is_ref(const char *v) { return v && v[0] == '$' && v[1] != '$'; }
static RecFlat *rec_flat(const RecCtx *c, const char *name) { for (int i = 0; i < c->nf; i++) if (!strcmp(c->f[i].name, name)) return &c->f[i]; return NULL; }
static RecTpl *rec_tpl(const RecCtx *c, const char *id) { for (int i = 0; i < c->nt; i++) if (!strcmp(c->t[i].id, id)) return &c->t[i]; return NULL; }
static void rec_tpl_clear(RecTpl *t) { for (int k = 0; k < t->n; k++) { free(t->key[k]); free(t->val[k]); } t->n = 0; }
static void rec_free(RecCtx *c) { for (int i = 0; i < c->nf; i++) free(c->f[i].val); for (int i = 0; i < c->nt; i++) rec_tpl_clear(&c->t[i]); free(c->f); free(c->t); memset(c, 0, sizeof *c); }

// does the document use any records feature? (flats / templates at the top, "base" on an edit, a "$" value anywhere a value can be)
static int rec_has_dollar(const JDoc *d, int node) {
    for (int c = d->n[node].first; c >= 0; c = d->n[c].next) {
        if (d->n[c].t == J_STR && d->n[c].str && d->n[c].str[0] == '$') return 1;
        if ((d->n[c].t == J_OBJ || d->n[c].t == J_ARR) && rec_has_dollar(d, c)) return 1;
    }
    return 0;
}
static int rec_edits(const JDoc *d) { if (d->count < 1) return -1; if (d->n[0].t == J_ARR) return 0; int e = d->n[0].t == J_OBJ ? jfind(d, 0, "edits") : -1; return e >= 0 && d->n[e].t == J_ARR ? e : -1; }
static int rec_uses(const JDoc *d) {
    if (d->count < 1) return 0;
    if (d->n[0].t == J_OBJ && (jfind(d, 0, "flats") >= 0 || jfind(d, 0, "templates") >= 0)) return 1;
    int el = rec_edits(d); if (el < 0) return 0;
    for (int e = d->n[el].first; e >= 0; e = d->n[e].next) if (d->n[e].t == J_OBJ && jfind(d, e, "base") >= 0) return 1;
    return rec_has_dollar(d, el);
}

// ---- collect (pass 1, every enabled mod in load order)
static int rec_flat_put(RecCtx *c, const char *name, const char *val, const char *mod) {
    RecFlat *f = rec_flat(c, name);
    if (!f) { if (c->nf == c->capf) { int n = c->capf ? c->capf * 2 : 64; RecFlat *m = realloc(c->f, (size_t)n * sizeof *m); if (!m) return 0; c->f = m; c->capf = n; }
              f = &c->f[c->nf++]; memset(f, 0, sizeof *f); snprintf(f->name, sizeof f->name, "%s", name); }
    else { f->overrides++; snprintf(f->prev, sizeof f->prev, "%s", f->mod); free(f->val); }
    snprintf(f->mod, sizeof f->mod, "%s", mod ? mod : "?"); f->val = _strdup(val); return f->val != NULL;
}
static int rec_collect(RecCtx *c, const JDoc *d, const char *mod, RowsWarn *w) {
    int n = 0; if (d->count < 1 || d->n[0].t != J_OBJ) return 0;
    int fl = jfind(d, 0, "flats"), tl = jfind(d, 0, "templates");
    if (fl >= 0) {
        if (d->n[fl].t != J_OBJ) rows_warn_add(w, "records: \"flats\" must be an object of name: value - ignored");
        else for (int p = d->n[fl].first; p >= 0; p = d->n[p].next) {
            const JNode *v = &d->n[p];
            if (!rec_name_ok(v->key)) { rows_warn_add(w, "records: flat '%.60s' SKIPPED: names are letters, digits, _ . - (at most %d)", v->key ? v->key : "", REC_NAME_MAX - 1); continue; }
            if (v->t == J_ARR || v->t == J_OBJ || v->t == J_NULL || !v->str) { rows_warn_add(w, "records: flat %s SKIPPED: the value must be text, a number or true/false", v->key); continue; }
            if (rec_is_ref(v->str)) { rows_warn_add(w, "records: flat %s SKIPPED: a flat cannot name another flat ('%.40s')", v->key, v->str); continue; }
            if (rec_flat_put(c, v->key, v->str, mod)) n++;
        }
    }
    if (tl >= 0) {
        if (d->n[tl].t != J_ARR) rows_warn_add(w, "records: \"templates\" must be an array - ignored");
        else for (int t = d->n[tl].first; t >= 0; t = d->n[t].next) {
            int ji = d->n[t].t == J_OBJ ? jfind(d, t, "id") : -1, jb = ji >= 0 ? jfind(d, t, "base") : -1, js = ji >= 0 ? jfind(d, t, "set") : -1;
            const char *id = ji >= 0 && d->n[ji].t == J_STR ? d->n[ji].str : NULL;
            if (!rec_name_ok(id)) { rows_warn_add(w, "records: a template without a valid \"id\" SKIPPED"); continue; }
            if (jb >= 0 && (d->n[jb].t != J_STR || !rec_name_ok(d->n[jb].str))) { rows_warn_add(w, "records: template %s SKIPPED: bad \"base\"", id); continue; }
            if (js >= 0 && d->n[js].t != J_OBJ) { rows_warn_add(w, "records: template %s SKIPPED: \"set\" must be an object", id); continue; }
            RecTpl *r = rec_tpl(c, id);
            if (!r) { if (c->nt == c->capt) { int m2 = c->capt ? c->capt * 2 : 32; RecTpl *m = realloc(c->t, (size_t)m2 * sizeof *m); if (!m) return n; c->t = m; c->capt = m2; }
                      r = &c->t[c->nt++]; memset(r, 0, sizeof *r); snprintf(r->id, sizeof r->id, "%s", id); }
            else { r->overrides++; snprintf(r->prev, sizeof r->prev, "%s", r->mod); rec_tpl_clear(r); }
            snprintf(r->mod, sizeof r->mod, "%s", mod ? mod : "?"); snprintf(r->base, sizeof r->base, "%s", jb >= 0 ? d->n[jb].str : "");
            for (int p = js >= 0 ? d->n[js].first : -1; p >= 0; p = d->n[p].next) {
                const JNode *v = &d->n[p];
                if (!v->key || !rows_name_ok(v->key, ROWS_NAME_MAX) || v->t == J_ARR || v->t == J_OBJ || v->t == J_NULL || !v->str) { rows_warn_add(w, "records: template %s field '%.60s' SKIPPED (bad name or value)", id, v->key ? v->key : ""); continue; }
                if (r->n >= REC_FIELDS) { rows_warn_add(w, "records: template %s: more than %d fields, the rest SKIPPED", id, REC_FIELDS); break; }
                r->key[r->n] = _strdup(v->key); r->val[r->n] = _strdup(v->str); if (r->key[r->n] && r->val[r->n]) r->n++; else { free(r->key[r->n]); free(r->val[r->n]); }
            }
            n++;
        }
    }
    return n;
}

// ---- resolve a template chain into key/value pairs (base first, the child overrides). 1 = ok, 0 = missing base / cycle / too deep (why says which)
typedef struct { int n; const char *key[REC_FIELDS], *val[REC_FIELDS]; } RecFields;
static void rec_fields_put(RecFields *o, const char *k, const char *v) { for (int i = 0; i < o->n; i++) if (!strcmp(o->key[i], k)) { o->val[i] = v; return; } if (o->n < REC_FIELDS) { o->key[o->n] = k; o->val[o->n] = v; o->n++; } }
static int rec_resolve(const RecCtx *c, const char *id, RecFields *out, char *why, size_t cap) {
    const RecTpl *chain[REC_DEPTH]; int n = 0; const char *cur = id;
    while (cur && cur[0]) {
        const RecTpl *t = rec_tpl(c, cur);
        if (!t) { snprintf(why, cap, "template '%.60s' does not exist", cur); return 0; }
        for (int i = 0; i < n; i++) if (chain[i] == t) { snprintf(why, cap, "template chain of '%.40s' has a cycle at '%.40s'", id, cur); return 0; }
        if (n >= REC_DEPTH) { snprintf(why, cap, "template chain of '%.40s' is deeper than %d", id, REC_DEPTH); return 0; }
        chain[n++] = t; cur = t->base;
    }
    out->n = 0;
    for (int i = n - 1; i >= 0; i--) for (int k = 0; k < chain[i]->n; k++) rec_fields_put(out, chain[i]->key[k], chain[i]->val[k]);
    return 1;
}

// ---- expand (pass 2, one mod's document; changed in place). Returns the number of edits / fields changed, warnings in w.
static int rec_set_str(JDoc *d, int node, const char *s) { char *t = _strdup(s); if (!t) return 0; free(d->n[node].str); d->n[node].str = t; d->n[node].t = J_STR; return 1; }
// one value node: "$Name" -> the flat's text, "$$x" -> "$x". 1 = ok (maybe unchanged), 0 = unknown flat (the caller drops the field)
static int rec_subst(RecCtx *c, JDoc *d, int node, const char *ctx, RowsWarn *w) {
    const char *s = d->n[node].t == J_STR ? d->n[node].str : NULL; if (!s || s[0] != '$') return 1;
    if (s[1] == '$') return rec_set_str(d, node, s + 1);
    const RecFlat *f = rec_flat(c, s + 1);
    if (!f) { rows_warn_add(w, "records: %s: flat '%.60s' is not defined by any enabled mod - field SKIPPED", ctx, s + 1); c->skipped_fields++; return 0; }
    c->subs++; return rec_set_str(d, node, f->val);
}
// unlink child `node` from object/array `parent`
static void rec_unlink(JDoc *d, int parent, int node) {
    if (d->n[parent].first == node) { d->n[parent].first = d->n[node].next; return; }
    for (int c = d->n[parent].first; c >= 0; c = d->n[c].next) if (d->n[c].next == node) { d->n[c].next = d->n[node].next; return; }
}
static int rec_new_value(JDoc *d, const char *key, const char *val) {
    int v = jnew(d, J_STR); if (v < 0) return -1; d->n[v].key = _strdup(key); d->n[v].str = _strdup(val);
    if (!d->n[v].key || !d->n[v].str) return -1; return v;
}
static int rec_expand(RecCtx *c, JDoc *d, const char *mod, RowsWarn *w) {
    int changed = 0, el = rec_edits(d); if (el < 0) return 0;
    for (int e = d->n[el].first, idx = 0, next; e >= 0; e = next, idx++) {
        next = d->n[e].next;
        if (d->n[e].t != J_OBJ) continue;                                      // rows_from_json reports structure errors
        char ctx[160]; int jr = jfind(d, e, "row"), ja = jfind(d, e, "add"), jo = jfind(d, e, "object");
        snprintf(ctx, sizeof ctx, "%s edit %d (%s %.60s)", mod ? mod : "?", idx, jo >= 0 ? "object" : "row", jo >= 0 && d->n[jo].str ? d->n[jo].str : jr >= 0 && d->n[jr].str ? d->n[jr].str : ja >= 0 && d->n[ja].str ? d->n[ja].str : "?");
        int jb = jfind(d, e, "base");
        if (jb >= 0) {
            char why[160] = ""; RecFields rf;
            if (d->n[jb].t != J_STR || !rec_name_ok(d->n[jb].str)) snprintf(why, sizeof why, "\"base\" must be a template id");
            else if (jo >= 0) snprintf(why, sizeof why, "\"base\" works on table rows, not on data asset ops");
            if (why[0] || !rec_resolve(c, d->n[jb].str, &rf, why, sizeof why)) { rows_warn_add(w, "records: %s SKIPPED: %s", ctx, why); rec_unlink(d, el, e); c->skipped_edits++; changed++; continue; }
            int js = jfind(d, e, "set");
            if (js >= 0 && d->n[js].t != J_OBJ) { rows_warn_add(w, "records: %s SKIPPED: \"set\" must be an object", ctx); rec_unlink(d, el, e); c->skipped_edits++; changed++; continue; }
            if (js < 0) { js = jnew(d, J_OBJ); if (js < 0) return changed; d->n[js].key = _strdup("set"); d->n[js].next = d->n[e].first; d->n[e].first = js; }
            // inherited fields go FIRST (the edit's own fields come later and win); a field the edit sets itself is not inherited
            int head = -1, tail = -1, added = 0;
            for (int k = 0; k < rf.n; k++) {
                if (jfind(d, js, rf.key[k]) >= 0) continue;
                int v = rec_new_value(d, rf.key[k], rf.val[k]); if (v < 0) return changed;
                if (head < 0) head = v; else d->n[tail].next = v; tail = v; added++;
            }
            if (tail >= 0) { d->n[tail].next = d->n[js].first; d->n[js].first = head; }
            rec_unlink(d, e, jb); c->inherited++; c->fields_inherited += added; changed++;
        }
        // flat references in "set" values and in data asset op values
        int js = jfind(d, e, "set");
        if (js >= 0 && d->n[js].t == J_OBJ) for (int p = d->n[js].first, pn; p >= 0; p = pn) { pn = d->n[p].next; char cx[220]; snprintf(cx, sizeof cx, "%s field %.60s", ctx, d->n[p].key ? d->n[p].key : "?");
            const char *was = d->n[p].str; int ref = d->n[p].t == J_STR && was && was[0] == '$';
            if (!rec_subst(c, d, p, cx, w)) { rec_unlink(d, js, p); changed++; } else if (ref) changed++; }
        int jp = jo >= 0 ? jfind(d, e, "ops") : -1;
        if (jp >= 0 && d->n[jp].t == J_ARR) for (int o = d->n[jp].first; o >= 0; o = d->n[o].next) { int jv = d->n[o].t == J_OBJ ? jfind(d, o, "value") : -1; if (jv < 0) continue;
            const char *was = d->n[jv].str; int ref = d->n[jv].t == J_STR && was && was[0] == '$';
            if (!rec_subst(c, d, jv, ctx, w)) { rows_warn_add(w, "records: %s: the op keeps its unresolved value and will fail validation", ctx); } else if (ref) changed++; }
    }
    return changed;
}

// one-paragraph report for mods.txt: flats / templates with their winners and overrides, inheritance counters
static int rec_report(const RecCtx *c, char *out, size_t cap) {
    size_t w = 0; if (!cap) return 0; out[0] = 0;
#define RW(...) do { if (w < cap) { int r_ = snprintf(out + w, cap - w, __VA_ARGS__); if (r_ > 0) w += (size_t)r_; if (w >= cap) w = cap - 1; } } while (0)
    RW("Records (flats / templates): %d flat(s), %d template(s); %d edit(s) inherited %d field(s), %d flat reference(s) resolved, %d edit(s) and %d field(s) skipped\r\n",
       c->nf, c->nt, c->inherited, c->fields_inherited, c->subs, c->skipped_edits, c->skipped_fields);
    for (int i = 0; i < c->nf; i++) if (c->f[i].overrides) RW("  flat %s = %s  (from %s, overrides %s%s)\r\n", c->f[i].name, c->f[i].val, c->f[i].mod, c->f[i].prev, c->f[i].overrides > 1 ? " and earlier" : "");
    for (int i = 0; i < c->nt; i++) if (c->t[i].overrides) RW("  template %s  (from %s, overrides %s)\r\n", c->t[i].id, c->t[i].mod, c->t[i].prev);
#undef RW
    return (int)w;
}
#endif
