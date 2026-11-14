/* npcs.h - pure helpers of the "npcs" content kind and its API (npcs.inc). No game access; offline checks in test/npcs_test.c. */
#ifndef NPCS_H
#define NPCS_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* deck keys of an entry (the asset's own field names, same left/right mirror as save decks): 0..3 quadrants, 4 = the four alternates; anything else -1 */
static int np_deck_key(const char *k) {
    static const char *const n[] = { "FrontLeft", "FrontRight", "BackRight", "BackLeft", "Alt" };
    if (!k) return -1;
    for (int i = 0; i < 5; i++) if (!strcmp(k, n[i])) return i;
    return -1;
}
/* the AttacksComboDB property that holds a deck key's rows */
static const char *np_deck_field(int key) {
    static const char *const f[] = { "m_ComboQuadrantsFrontLeftXls", "m_ComboQuadrantsFrontRightXls", "m_ComboQuadrantsBackRightXls", "m_ComboQuadrantsBackLeftXls", "m_AlternativeAttacksXls" };
    return key >= 0 && key < 5 ? f[key] : NULL;
}
/* an attack row name: letters, digits, _ ; 1..15 characters */
static int np_row_ok(const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s); if (n > 15) return 0;
    for (; *s; s++) { char c = *s; if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0; }
    return 1;
}
/* one alternate slot of the m_Struct text in the engine's own export form m_AlternativeAttacksXls[i]="042" (id NULL / "" -> None). m_AlternativeAttacksXls is a
 * STATIC array of 4 FNames, not a TArray: "m_AlternativeAttacksXls=(...)" is not that form (shared by npcs.inc and bots.h). Returns the new length. */
static int np_alt_part(char *out, int len, int cap, int comma, int slot, const char *id) {
    if (id && *id) return len + snprintf(out + len, cap - len, "%sm_AlternativeAttacksXls[%d]=\"%s\"", comma ? "," : "", slot, id);
    return len + snprintf(out + len, cap - len, "%sm_AlternativeAttacksXls[%d]=None", comma ? "," : "", slot);
}
/* append one deck field to the m_Struct text: ("078","138") -> m_ComboQuadrantsFrontLeftXls=("078","138"); the Alt key (4) -> m_AlternativeAttacksXls[0]="078",[1]=...
 * (slots in order, at most 4); returns the new length (rows are already checked with np_row_ok) */
static int np_deck_part(char *out, int len, int cap, int parts, int key, const char *const *rows, int nrows) {
    if (key == 4) { for (int i = 0; i < nrows && i < 4; i++) len = np_alt_part(out, len, cap, parts || i, i, rows[i]); return len; }
    len += snprintf(out + len, cap - len, "%s%s=(", parts ? "," : "", np_deck_field(key));
    for (int i = 0; i < nrows; i++) len += snprintf(out + len, cap - len, "%s\"%s\"", i ? "," : "", rows[i]);
    len += snprintf(out + len, cap - len, ")");
    return len;
}
/* numeric ranges; negative / NaN inputs mean "not given" except where noted. Returns 1 if usable, else 0 and the reason in err. */
static int np_check_numbers(double health, double style, double faction, double cooldown, char *err, int cap) {
    if (health >= 0 && (health < 1 || health > 100000)) { snprintf(err, cap, "\"health\" must be 1..100000"); return 0; }
    if (style >= 0 && style > 63) { snprintf(err, cap, "\"style\" must be 0..63 (0 Forsaken, 1 Windfall, 2 Lost Prospect, 3 Kahlt, 4 Stagger, 5 Faejin, 6+ mod styles)"); return 0; }
    if (faction >= 0 && faction > 5) { snprintf(err, cap, "\"faction\" must be 0..5 (the game has 6 factions; AI characters default to 2)"); return 0; }
    if (cooldown >= 0 && cooldown > 600) { snprintf(err, cap, "\"ai.attack_cooldown\" is in seconds, 0..600"); return 0; }
    return 1;
}
/* reaction actions: "log <text>", "message <text>", "heal", "ai on", "ai off", "target player", "faction <0-5>", "remove" */
enum { NPA_BAD = -1, NPA_LOG, NPA_MESSAGE, NPA_HEAL, NPA_AI_ON, NPA_AI_OFF, NPA_TARGET_PLAYER, NPA_FACTION, NPA_REMOVE };
static int np_action(const char *s) {
    if (!s) return NPA_BAD;
    if (!strncmp(s, "log ", 4)) return NPA_LOG;
    if (!strncmp(s, "message ", 8)) return NPA_MESSAGE;
    if (!strcmp(s, "heal")) return NPA_HEAL;
    if (!strcmp(s, "ai on")) return NPA_AI_ON;
    if (!strcmp(s, "ai off")) return NPA_AI_OFF;
    if (!strcmp(s, "target player")) return NPA_TARGET_PLAYER;
    if (!strncmp(s, "faction ", 8) && s[8] >= '0' && s[8] <= '5' && !s[9]) return NPA_FACTION;
    if (!strcmp(s, "remove")) return NPA_REMOVE;
    return NPA_BAD;
}
/* the health fraction trigger: 1 when health has fallen to or below fraction * max (and max is known); a dead or zero-max reading never fires */
static int np_hp_below(float health, float max_health, double fraction) {
    return fraction > 0 && fraction <= 1 && max_health > 0 && health >= 0 && health <= max_health * (float)fraction;
}
/* 2D distance (the area test ignores height: hills and ledges must not hide an NPC that stands right next to you); -1 if a coordinate is not finite */
static float np_dist2d(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1]; if (!isfinite(dx) || !isfinite(dy)) return -1.f;
    return sqrtf(dx * dx + dy * dy);
}
/* one step of an area watcher: dist = current 2D distance (< 0 = unknown, keeps the state), was_in = state before. Returns +1 entered, -1 left, 0 unchanged; *now_in = new state.
 * Leaving needs radius * 1.05 (hysteresis), so a character that stands on the border does not fire enter / leave every poll. */
static int np_area_step(float dist, float radius, int was_in, int *now_in) {
    *now_in = was_in;
    if (dist < 0 || radius <= 0) return 0;
    if (!was_in && dist <= radius) { *now_in = 1; return 1; }
    if (was_in && dist > radius * 1.05f) { *now_in = 0; return -1; }
    return 0;
}
/* index into UFactionsManager::m_FactionsTargetTable (6x6 bools, row = the attacker's faction) or -1 */
static int np_faction_index(int a, int b) { return a >= 0 && a < 6 && b >= 0 && b < 6 ? a * 6 + b : -1; }
/* the asset path of an entry: /Game/Mods/<mod>/<id>.<id> (mod already sanitized with cs_sanitize) */
static int np_asset_path(const char *mod, const char *id, char *out, int cap) { return snprintf(out, cap, "/Game/Mods/%s/%s.%s", mod, id, id); }
/* does a text (an archetype description printed by the engine) name the asset path of an entry? */
static int np_text_names(const char *text, const char *asset_path) {
    if (!text || !asset_path || !*asset_path) return 0;
    size_t n = strlen(asset_path);
    for (const char *p = text; (p = strstr(p, asset_path)) != NULL; p++) { char c = p[n]; if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 1; }
    return 0;
}
#endif
