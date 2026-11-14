// sidecar.inc - the per-character metadata store. Included by secore.inc after combodeck.inc. Pure logic: sidecar.h.
// File: LanNative\state\<GUID>.txt (format: sidecar.h; never in SaveGames).
// The document of the character in play is cached and written through on every change (temp file + rename).
// On first open the old kv-file keys are migrated to "legacy.*" (sidecar.h). Game thread only (reads the live save). API 2.18: sidecar_get / sidecar_set ("p.<plugin>.<key>").
#include "sidecar.h"
static ScDoc g_sc; static char g_sc_id[48]; static int g_sc_have, g_sc_writes;

static void sc_char_ids(char *id, int idcap, char *name, int namecap) {      // id = GUID text ("" = no live save), name = character name (UTF-8)
    id[0] = 0; name[0] = 0; void *s = cd_live_save(); if (!s) return; char why[100]; uint32_t *g = NULL; SeFString *n = NULL;
    __try { g = cd_path(s, "m_SavedData.m_FightingCharacterGuid", why, sizeof why); n = cd_path(s, "m_SavedData.m_CharaName", why, sizeof why);
            if (n && n->data) WideCharToMultiByte(CP_UTF8, 0, n->data, -1, name, namecap, NULL, NULL);
            char t[33]; if (g && sc_guid_text(g, t)) snprintf(id, idcap, "%s", t); } __except (EXCEPTION_EXECUTE_HANDLER) { id[0] = 0; }
    if (!id[0] && s) {                                                          // no GUID yet (the game assigns one to a brand-new character): key by name, logged
        char o[100]; sc_old_name(name, o, sizeof o); for (char *c = o; *c; c++) if (!isalnum((unsigned char)*c)) *c = '_'; snprintf(id, idcap, "noguid_%s", o);
        static int logged; if (!logged++) logf_("sidecar: the save has no character GUID yet - state keyed by name (%s) until it has one", id);
    }
}
static void sc_file(const char *id, wchar_t *out, int tmp) { wchar_t dir[MAX_PATH], w[64]; swprintf(dir, MAX_PATH, L"%sstate", g_dir); CreateDirectoryW(dir, NULL); u8_to_w(id, w, 64); swprintf(out, MAX_PATH, L"%ls\\%ls.txt%ls", dir, w, tmp ? L".tmp" : L""); }
static char *sc_read_all(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = n >= 0 && n < (64 << 20) ? malloc((size_t)n + 1) : NULL; if (b) { b[fread(b, 1, (size_t)n, f)] = 0; } fclose(f); return b;
}
static int sc_flush(void) {
    char hdr[160]; const char *nm = sc_get(&g_sc, "name"); snprintf(hdr, sizeof hdr, "LanNative per-character state (metadata only), character %s, GUID %s", nm ? nm : "?", g_sc_id);
    char *t = sc_render(&g_sc, hdr); wchar_t p[MAX_PATH], tp[MAX_PATH]; sc_file(g_sc_id, p, 0); sc_file(g_sc_id, tp, 1); int ok = 0;
    FILE *f = t ? _wfopen(tp, L"wb") : NULL; if (f) { size_t L = strlen(t); ok = fwrite(t, 1, L, f) == L; ok = fclose(f) == 0 && ok; }
    if (ok) ok = MoveFileExW(tp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    free(t); g_sc_writes++; if (!ok) logf_("sidecar: WRITE FAILED for %ls (error %lu) - the state lives in memory until the next write", p, GetLastError());
    return ok;
}
// remove keys from the loader's kv file (lines "key=value"), keeping every other line as it is
static void sc_kv_remove(char (*keys)[160], int n) {
    if (n <= 0) return; wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sdata\\Content.txt", g_dir); char *t = sc_read_all(p); if (!t) return;
    size_t L = strlen(t), w = 0; char *out = malloc(L + 1); if (!out) { free(t); return; }
    for (char *q = t; *q; ) {
        char *e = strchr(q, '\n'); size_t len = e ? (size_t)(e - q) + 1 : strlen(q); int drop = 0;
        for (int k = 0; k < n; k++) { size_t kl = strlen(keys[k]); if (len > kl && !strncmp(q, keys[k], kl) && q[kl] == '=') drop = 1; }
        if (!drop) { memcpy(out + w, q, len); w += len; } q += len;
    }
    FILE *f = _wfopen(p, L"wb"); if (f) { fwrite(out, 1, w, f); fclose(f); }
    for (int k = 0; k < n; k++) logf_("sidecar: migrated kv key '%s' removed from data\\Content.txt%s", keys[k], f ? "" : " - FAILED (file not writable)");
    free(out); free(t);
}
// the document of the character in play (NULL: no live save)
static ScDoc *sc_doc(void) {
    char id[48], name[100]; sc_char_ids(id, sizeof id, name, sizeof name); if (!id[0]) return NULL;
    if (g_sc_have && !strcmp(id, g_sc_id)) {
        const char *was = sc_get(&g_sc, "name"); if (name[0] && (!was || strcmp(was, name))) { logf_("sidecar: character %s renamed '%s' -> '%s': its state is kept (keyed by GUID)", id, was ? was : "?", name); sc_set(&g_sc, "name", name); sc_flush(); }
        return &g_sc;
    }
    sc_free(&g_sc); snprintf(g_sc_id, sizeof g_sc_id, "%s", id); g_sc_have = 1;
    wchar_t p[MAX_PATH]; sc_file(id, p, 0); char *t = sc_read_all(p); int nk = sc_parse(&g_sc, t), had = t != NULL, dirty = 0; free(t);
    const char *was = sc_get(&g_sc, "name");
    logf_("sidecar: character '%s' GUID %s: %ls %s (%d key(s))%s%s%s", name, id, p, had ? "loaded" : "not there yet", nk, was && name[0] && strcmp(was, name) ? ", renamed from '" : "", was && name[0] && strcmp(was, name) ? was : "", was && name[0] && strcmp(was, name) ? "' (state kept)" : "");
    if (name[0] && (!was || strcmp(was, name))) dirty |= sc_set(&g_sc, "name", name) > 0;
    if (!sc_get(&g_sc, "migrated")) {                                         // once per character: the old name-keyed kv keys
        wchar_t kp[MAX_PATH]; swprintf(kp, MAX_PATH, L"%sdata\\Content.txt", g_dir); char *kv = sc_read_all(kp), on[100], rm[8][160]; int nrm = 0;
        sc_old_name(name, on, sizeof on); int took = sc_migrate(&g_sc, kv, on, rm, 8, &nrm); free(kv); dirty = 1;
        logf_("sidecar: migration from the name-keyed kv store ('%s.*'): %d key(s) taken%s", on, took, took ? " (legacy.staff_deck / legacy.sword_deck / legacy.style)" : "");
        if (dirty && sc_flush()) sc_kv_remove(rm, nrm); dirty = 0;           // the old keys go only after the new file is on disk
    }
    if (dirty) sc_flush();
    return &g_sc;
}
static int sc_read_key(const char *key, char *out, int cap) { ScDoc *d = sc_doc(); const char *v = d ? sc_get(d, key) : NULL; if (out && cap > 0) snprintf(out, (size_t)cap, "%s", v ? v : ""); return v != NULL; }
static int sc_write_key(const char *key, const char *value) {                  // 1 = stored (and on disk), 0 = no character / bad key
    ScDoc *d = sc_doc(); if (!d) { static int n; if (n++ < 10) logf_("sidecar: '%s' not stored - no character save loaded", key ? key : "?"); return 0; }
    int r = sc_set(d, key, value); if (r < 0) { logf_("sidecar: bad key '%s'", key ? key : "?"); return 0; }
    if (r > 0) { logf_("sidecar: %s %s = %s", g_sc_id, key, value ? value : "(removed)"); return sc_flush(); }
    return 1;
}
// ----- API 2.18: plugin keys "p.<plugin>.<key>" in the local character's document
static int sc_api_key(const AbsApi *self, void *ch, const char *key, char *out, int cap) {
    const char *who = plug_name_of(plug_of(self)); void *pawn = cd_local_pawn();
    if (ch && ch != pawn) { static int n; if (n++ < 10) logf_("plugin %s: sidecar: only the local player's character has a sidecar (character %p)", who, ch); return 0; }
    if (!key || !*key || strpbrk(key, "=\r\n")) return 0; if (!api_p_is_game_thread(self)) { logf_("plugin %s: sidecar refused - not on the game thread", who); return 0; }
    int n = snprintf(out, (size_t)cap, "p.%s.%s", who, key); return n > 0 && n < cap;
}
static int api_p_sidecar_get(const AbsApi *self, void *ch, const char *key, char *out, int cap) {
    char k[300]; if (out && cap > 0) out[0] = 0; if (!out || cap < 1 || !sc_api_key(self, ch, key, k, sizeof k)) return 0; return sc_read_key(k, out, cap);
}
static int api_p_sidecar_set(const AbsApi *self, void *ch, const char *key, const char *value) { char k[300]; return sc_api_key(self, ch, key, k, sizeof k) && sc_write_key(k, value); }
static void sc_fill_api(AbsApi *a) { a->sidecar_get = api_p_sidecar_get; a->sidecar_set = api_p_sidecar_set; }
