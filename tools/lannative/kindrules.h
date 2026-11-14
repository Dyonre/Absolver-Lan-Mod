// kindrules.h - the pure decisions of weaponslot.inc for SEVERAL weapon kinds on one family. No game types (pure code).
// A "kind entry" is eligible when the kind is exclusive, has a deck and sits on the asked family. For each eligible kind the caller passes:
//   mine[i]        the asked attack row is one of the kind's moves
//   own_current[i] the family's CURRENT deck is one of the kind's decks
//   allow[i]       which vanilla rows of the base family the kind also accepts: 0 none, 1 the "bare-handed" ones (allow_bare_hands: kicks, elbows ...),
//                  2 every vanilla Sword row (allow_sword_moves; one-way: the Sword never gets the kind's rows, those are "mine" of the kind)
// and for the row: row_level = kr_row_level(row) when the row belongs to NO kind, else 0 (1 = vanilla bare-handed Sword row, 2 = vanilla Sword weapon row).
#ifndef KINDRULES_H
#define KINDRULES_H
// Family check (IsAttackCompatibleWithWeaponFamily): 1 = answered (*r set), 0 = not a kind matter (the caller falls back to the item / vanilla rules).
// A kind's row is allowed only while that kind's deck is current; every other row is refused while ANY kind's deck is current - EXCEPT a vanilla row the current
// kind accepts (row_level > 0 and allow[i] >= row_level): then the vanilla rules answer.
static int kr_famchk(int n, const unsigned char *mine, const unsigned char *own_current, const unsigned char *allow, int row_level, unsigned char *r) {
    for (int i = 0; i < n; i++) {
        if (mine[i]) { *r = own_current[i]; return 1; }
        if (own_current[i]) { if (row_level > 0 && allow[i] >= row_level) return 0; *r = 0; return 1; }
    }
    return 0;
}
// Attack list filter (FilterAttack): show[i] = kind i's entry is shown (Attacks-page choice, else its deck is current); owner = index of the kind that owns the row, -1 = none.
// A kind's row is listed when its entry is shown; a row of no kind only when no kind entry is shown, or the shown kind accepts it (row_level > 0 and allow >= row_level).
static int kr_filter(int n, const unsigned char *show, const unsigned char *allow, int owner, int row_level) {
    int any = 0, ok = 0; for (int i = 0; i < n; i++) { any |= show[i]; if (show[i] && row_level > 0 && allow[i] >= row_level) ok = 1; }
    if (owner >= 0 && owner < n) return show[owner];
    return !any || ok;
}
// A vanilla Sword-family row: flagged "with sword" (+0x182). Level 1 = not a weapon attack (its attacking slot +0x108, EWeaponSlot, is not RightHand (1)), 2 = a weapon-hand attack.
static int kr_row_level(const unsigned char *row) { return !row || !row[0x182] ? 0 : row[0x108] != 1 ? 1 : 2; }
static int kr_row_bare(const unsigned char *row) { return kr_row_level(row) == 1; }   // 1 = a vanilla bare-handed Sword row
#endif
