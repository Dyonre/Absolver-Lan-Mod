#ifndef LANNATIVE_EQUIPRULE_H
#define LANNATIVE_EQUIPRULE_H
// Auto-equip rule for content items with "equip": true (several mods may equip into one slot; without a rule the last one in load order would silently win
// and replace an earlier mod's item after the world loaded). Pure code (no OS / engine calls).
//   * first grant only: the item is equipped only in the pass that adds it to the inventory. An item already owned (a save made earlier) is left as the player has it.
//   * one auto-equip per slot per session: the first mod item (load order) that equips into a slot keeps it; later ones are granted but not equipped (and warned about).
//   * never over the player's own choice: a slot that already holds an item the loader did not put there (hand-equipped, or from the save) is left alone.
#include <string.h>
#include <stdio.h>

typedef enum { EQ_YES = 0, EQ_NOT_FIRST_GRANT, EQ_SLOT_TAKEN_BY_MOD, EQ_SLOT_HAS_OTHER_ITEM } EqVerdict;
// slot key from the base asset path: "/Game/DB/Weapons/Sword/SwordPike.SwordPike" -> "weapon"; "/Game/DB/Equipment/Masks/X.X" -> "Equipment/Masks"; anything else -> its folder under /Game/DB/
static void eq_slot_of(const char *base, char *out, size_t cap) {
    const char *p = strstr(base, "/Game/DB/"); p = p ? p + 9 : base;
    if (!strncmp(p, "Weapons/", 8)) { snprintf(out, cap, "weapon"); return; }
    const char *last = strrchr(p, '/'); size_t n = last ? (size_t)(last - p) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0; if (!n) snprintf(out, cap, "item");
}
// newly_granted: this pass added the item. owner: id of the mod item that already auto-equipped into this slot in this session (NULL/"" = none).
// other_item_equipped: the slot holds an item that is not ours (only known for weapons today; pass 0 when it cannot be read).
static EqVerdict eq_decide(int newly_granted, const char *owner, int other_item_equipped) {
    if (!newly_granted) return EQ_NOT_FIRST_GRANT;
    if (owner && owner[0]) return EQ_SLOT_TAKEN_BY_MOD;
    if (other_item_equipped) return EQ_SLOT_HAS_OTHER_ITEM;
    return EQ_YES;
}
static const char *eq_why(EqVerdict v) {
    switch (v) {
    case EQ_YES: return "equipped";
    case EQ_NOT_FIRST_GRANT: return "already owned, not equipped again (the player's choice stands)";
    case EQ_SLOT_TAKEN_BY_MOD: return "slot already taken by another mod item";
    default: return "slot already holds an item the loader did not equip";
    }
}
#endif
