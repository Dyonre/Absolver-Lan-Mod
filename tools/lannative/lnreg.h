// ---------------------------------------------------------------------------------------------------------------------
// lnreg.h - registration tables collected at build time (guide: README.md "Units and registration").
//
// A unit registers what it offers next to its own code; the linker collects every registration of one table into a
// sorted run of pointers, and the loader core walks that run. Nothing in the core names the unit, so a new unit is one
// file plus its #include line in units.inc.
//
//   LN_TABLE(tick)                              declares the table "tick" (once, lnhooks.h)
//   LN_HOOK(tick, 0300, hostmods) { hm_tick(); }   a void(void) hook named "hostmods", order key 0300
//   LN_RUN(tick);                               calls every hook of the table, lowest key first
//   LN_ENTRY(tbl, key, name, &desc)             the same for any descriptor struct; walk it with LN_FOREACH(tbl, Type, v) { ... }
//   LN_ENTRY_BY_NAME(tbl, name, &desc)          an entry of a table whose order does not matter (walked in name order)
//
// How it works (MSVC / PE only, like the rest of the loader): each entry is one `const void *` in the section
// ".lnr$<table>_k<key>_<name>"; the table's begin / end markers sit in ".lnr$<table>_a" / ".lnr$<table>_z". The linker
// merges everything before the '$' into one section and sorts the parts by the text after it, so a table's entries end up
// between its markers in key order (the same mechanism the C runtime uses for its initializer lists, .CRT$XCA .. .CRT$XCZ).
// The linker may pad between parts with zero bytes: a walk skips NULL slots. /include keeps every entry alive under
// /Gw, /GL and /OPT:REF.
//
// Rules, all checked by the compiler:
//   * the table exists: an entry for a table without LN_TABLE is error C2065 on lnb_<table> (it would otherwise sit in a
//     section nobody walks - e.g. when a wrapper macro's parameter has the table's name and replaces it);
//   * the order key is exactly 4 digits (0000 .. 9999), so text order = numeric order;
//   * a key is used once per table: a second registration with the same key is error C2374 on lnu_<table>_<key>
//     (pick the next free key; the order of a table is therefore total and does not depend on the #include order);
//   * a name is used once per table (C2374 on lnr_<table>_<name>); table and hook names are C identifiers, and table
//     names contain no underscore (so no table's section prefix is the start of another's).
// LN_FOREACH is two nested for statements: `continue` moves to the next entry, `break` does too (it does not leave the walk).
// ---------------------------------------------------------------------------------------------------------------------
#ifndef LNREG_H
#define LNREG_H

#define LN_STR_(x) #x
#define LN_STR(x) LN_STR_(x)
#define LN_SEC_(table, part) LN_STR(.lnr$##table##_##part)
#define LN_PUT_(table, part, sym, val) \
    __pragma(section(LN_SEC_(table, part), read)) \
    __declspec(allocate(LN_SEC_(table, part))) const void *const sym = (val); \
    __pragma(comment(linker, "/include:" LN_STR(sym)))

#define LN_TABLE(table) LN_PUT_(table, a, lnb_##table, 0) LN_PUT_(table, z, lne_##table, 0)

#define LN_ENTRY(table, key, name, ptr) \
    typedef char lnt_##table##_##name[sizeof(lnb_##table) ? 1 : -1]; /* C2065 here = no such table (LN_TABLE) */ \
    typedef char lnk_##table##_##name[sizeof(#key) == 5 ? 1 : -1];   /* the key must be 4 digits */ \
    static const char lnu_##table##_##key = 0;                       /* C2374 here = this key is already used in this table */ \
    LN_PUT_(table, k##key##_##name, lnr_##table##_##name, ptr)

// for tables whose order does not matter: no key, the entries are walked in name order
#define LN_ENTRY_BY_NAME(table, name, ptr) \
    typedef char lnt_##table##_##name[sizeof(lnb_##table) ? 1 : -1]; /* C2065 here = no such table (LN_TABLE) */ \
    LN_PUT_(table, k_##name, lnr_##table##_##name, ptr)

#define LN_FOREACH(table, T, v) \
    for (const void *const *lnp_ = &lnb_##table + 1; lnp_ < &lne_##table; lnp_++) \
        for (const T *v = (const T *)*lnp_; v; v = 0)

// the plain hook: a named void(void) function
typedef struct { const char *name; const char *key; void (*fn)(void); } LnHook;
#define LN_HOOK(table, key, name) \
    static void lnh_##table##_##name(void); \
    static const LnHook lnd_##table##_##name = { #name, #key, lnh_##table##_##name }; \
    LN_ENTRY(table, key, name, &lnd_##table##_##name) \
    static void lnh_##table##_##name(void)

static const char *g_ln_cur;                  // the hook that is running (NULL = none): names the unit when a guarded stage faults
#define LN_RUN(table) do { const char *lnprev_ = g_ln_cur; LN_FOREACH(table, LnHook, lnh_) { g_ln_cur = lnh_->name; lnh_->fn(); } g_ln_cur = lnprev_; } while (0)

// "name(key) name(key) ..." of a table whose descriptors start like LnHook (name, key), for the boot log; returns the number of entries
static int ln_describe_(const void *const *b, const void *const *e, char *out, int cap) {
    int n = 0, o = 0; if (cap > 0) out[0] = 0;
    for (const void *const *p = b + 1; p < e; p++) {
        const LnHook *h = (const LnHook *)*p; if (!h) continue;
        n++;
        for (const char *s = o ? " " : ""; *s && o < cap - 1; s++) out[o++] = *s;
        for (const char *s = h->name; *s && o < cap - 1; s++) out[o++] = *s;
        if (h->key[0] && o < cap - 1) out[o++] = '(';
        for (const char *s = h->key; *s && o < cap - 1; s++) out[o++] = *s;
        if (h->key[0] && o < cap - 1) out[o++] = ')';
        if (cap > 0) out[o] = 0;
    }
    return n;
}
#define LN_DESCRIBE(table, out, cap) ln_describe_(&lnb_##table, &lne_##table, out, cap)

#endif
