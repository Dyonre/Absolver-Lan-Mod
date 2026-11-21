/* Item catalog of the Cheats "Add items" tab: the list, its search filter and its order. Pure C (no engine calls). The plugin fills it from the game's loaded item data assets when the tab opens (plugin.c cat_build) and the ImGui window reads it. */
#ifndef CHEATS_CATALOG_H
#define CHEATS_CATALOG_H
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAT_GEAR = 1, CAT_WEAPON = 2, CAT_POWER = 4, CAT_ALL = 7 };   /* a kind, also usable as a filter mask */
#define CAT_MAX 3000
#define CAT_VARS 16
typedef struct {
    char path[200];            /* /Game/DB/... object path, the key of the entry */
    char name[64];             /* display name; the object's short name when the game has none */
    char slot[24];             /* equipment slot text (Mask, Weapon, Item, ...) */
    int kind;                  /* CAT_GEAR / CAT_WEAPON / CAT_POWER */
    int mod;                   /* 1 = from a mod (path under /Game/Mods/) */
    int nvar;                  /* colour / material variants, 0 = a single look */
    char var[CAT_VARS][24];    /* variant labels, "" = "Variant N" */
    int owned;                 /* in the inventory when the list was last refreshed */
} CatItem;
typedef struct { CatItem *it; int n; } Catalog;

static int cat_ieq_sub(const char *hay, const char *needle, size_t nl) {   /* case-insensitive substring */
    if (!nl) return 1;
    for (; *hay; hay++) { size_t i = 0; while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++; if (i == nl) return 1; }
    return 0;
}
static const char *cat_kind_text(int kind) { return kind == CAT_WEAPON ? "Weapon" : kind == CAT_POWER ? "Power" : "Gear"; }

/* an entry matches when every space-separated word of the query occurs (any case) in "name slot kind path-tail"; kinds is a CAT_* mask */
static int cat_match(const CatItem *c, const char *query, int kinds) {
    if (!(c->kind & kinds)) return 0;
    char hay[400]; const char *tail = strrchr(c->path, '/'); snprintf(hay, sizeof hay, "%s %s %s %s%s", c->name, c->slot, cat_kind_text(c->kind), tail ? tail + 1 : c->path, c->mod ? " mod" : "");
    const char *p = query ? query : "";
    while (*p) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != ' ') e++;
        if (e > p && !cat_ieq_sub(hay, p, (size_t)(e - p))) return 0;
        p = e;
    }
    return 1;
}
/* indexes of the matching entries in catalog order (which cat_sort made kind, slot, name) -> count, at most cap */
static int cat_filter(const Catalog *cat, const char *query, int kinds, int *out, int cap) {
    int n = 0; for (int i = 0; i < cat->n && n < cap; i++) if (cat_match(&cat->it[i], query, kinds)) out[n++] = i;
    return n;
}
static int cat_find(const Catalog *cat, const char *path) { for (int i = 0; i < cat->n; i++) if (!strcmp(cat->it[i].path, path)) return i; return -1; }
/* adds an entry unless the path is known (then the existing entry is kept: -1) or the list is full (-2); returns the index */
static int cat_add(Catalog *cat, const CatItem *c) {
    if (!c->path[0]) return -2;
    if (cat_find(cat, c->path) >= 0) return -1;
    if (cat->n >= CAT_MAX) return -2;
    if (!cat->it) { cat->it = (CatItem *)calloc(CAT_MAX, sizeof(CatItem)); if (!cat->it) return -2; }
    cat->it[cat->n] = *c; return cat->n++;
}
static int cat_cmp(const void *a, const void *b) {
    const CatItem *x = (const CatItem *)a, *y = (const CatItem *)b; int d;
    if (x->kind != y->kind) return x->kind - y->kind;
    if ((d = _stricmp(x->slot, y->slot)) != 0) return d;
    if ((d = _stricmp(x->name, y->name)) != 0) return d;
    return strcmp(x->path, y->path);
}
static void cat_sort(Catalog *cat) { if (cat->n > 1) qsort(cat->it, (size_t)cat->n, sizeof(CatItem), cat_cmp); }
static void cat_free(Catalog *cat) { free(cat->it); cat->it = NULL; cat->n = 0; }
/* label of variant v (0-based): its own text, else "Variant N" (N from 1); "Default" when the item has no variants */
static const char *cat_var_label(const CatItem *c, int v, char *buf, size_t cap) {
    if (c->nvar <= 0) return "Default";
    if (v >= 0 && v < c->nvar && v < CAT_VARS && c->var[v][0]) return c->var[v];
    snprintf(buf, cap, "Variant %d", v + 1); return buf;
}
/* clamps a requested variant into the item's range (0 = the default look) */
static int cat_var_clamp(const CatItem *c, int v) { return (c->nvar <= 0 || v < 0) ? 0 : v >= c->nvar ? c->nvar - 1 : v; }
/* clamps the "count" box: powers and weapons are single items, gear too in practice; 1..99 */
static int cat_count_clamp(int n) { return n < 1 ? 1 : n > 99 ? 99 : n; }
#endif
