/* powers.h - pure helpers of the "powers" content kind (powers.inc). No game access; offline checks in test/powers_test.c. */
#ifndef POWERS_H
#define POWERS_H
#include <stdio.h>
#include <string.h>

/* JSON string escaping for the items entry that powers.inc synthesizes (quotes, backslashes, control characters; UTF-8 passes through). Returns the length written (always terminated). */
static int pw_json_escape(const char *in, char *out, int cap) {
    int n = 0;
    for (; in && *in && n + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    if (cap > 0) out[n] = 0;
    return n;
}
/* "all" -> -1, "fl" 0, "fr" 1, "br" 2, "bl" 3 (UsableItemData::m_Anims index = stance: FrontLeft, FrontRight, BackRight, BackLeft); anything else -2 */
static int pw_anim_index(const char *key) {
    static const char *const k[] = { "fl", "fr", "br", "bl" };
    if (!key) return -2;
    if (!strcmp(key, "all")) return -1;
    for (int i = 0; i < 4; i++) if (!strcmp(key, k[i])) return i;
    return -2;
}
/* numeric ranges of a power entry; returns 1 if usable, else 0 and the reason in err. Negative inputs mean "not given". */
static int pw_check_numbers(int cost, double cooldown, int target_req, int slot, double radius, char *err, int cap) {
    if (cost > 255) { snprintf(err, cap, "\"cost\" (shards) must be 0-255"); return 0; }
    if (cooldown > 100000.0) { snprintf(err, cap, "\"cooldown\" is in seconds and must be 0 or more"); return 0; }
    if (target_req > 255) { snprintf(err, cap, "\"target_requirements\" is a bit mask 0-255"); return 0; }
    if (slot > 1) { snprintf(err, cap, "\"equip_slot\" must be 0 (key 1) or 1 (key 3)"); return 0; }
    if (radius > 20000.0) { snprintf(err, cap, "\"radius\" must be 1-20000 Unreal units (the stock Shockwave is 280)"); return 0; }
    return 1;
}
#endif
