// menupad.h - gamepad for the loader's F1 / F2 menus Pure C: the caller reads XInput and passes
// the buttons and the left stick; this turns them into menu keys with auto-repeat.
//   d-pad up / down or left stick = move (repeat after 0.40 s, then every 0.09 s), A = select, B = back, d-pad left = back, d-pad right = select,
//   Back + Start together = open / close the F1 menu (pressing Back first keeps Start from the game: overlay.cpp hides it while Back is held).
#ifndef LANNATIVE_MENUPAD_H
#define LANNATIVE_MENUPAD_H

// XINPUT_GAMEPAD button bits (same values as xinput.h)
enum { MPB_UP = 0x0001, MPB_DOWN = 0x0002, MPB_LEFT = 0x0004, MPB_RIGHT = 0x0008, MPB_START = 0x0010, MPB_BACK = 0x0020, MPB_A = 0x1000, MPB_B = 0x2000 };
enum { MPK_NONE = 0, MPK_UP, MPK_DOWN, MPK_SELECT, MPK_BACK, MPK_TOGGLE };
typedef struct { unsigned prev; int held_dir; float held_t, next_rep; int chord_fired; } MenuPad;
#define MENUPAD_REPEAT_DELAY 0.40f
#define MENUPAD_REPEAT_EVERY 0.09f
#define MENUPAD_STICK 0.55f                                             // stick deflection (of 1) that counts as a d-pad press

// One step. buttons = XINPUT_GAMEPAD.wButtons, ly = left stick Y in -1..1 (up positive), dt in seconds. Returns MPK_* (one key per step at most).
static int menupad_step(MenuPad *p, unsigned buttons, float ly, float dt) {
    unsigned b = buttons; if (ly > MENUPAD_STICK) b |= MPB_UP; else if (ly < -MENUPAD_STICK) b |= MPB_DOWN;
    unsigned edge = b & ~p->prev; p->prev = b;
    int key = MPK_NONE;
    if ((b & (MPB_BACK | MPB_START)) == (MPB_BACK | MPB_START)) { if (!p->chord_fired) { p->chord_fired = 1; key = MPK_TOGGLE; } }
    else if (!(b & (MPB_BACK | MPB_START))) p->chord_fired = 0;
    int dir = (b & MPB_UP) && !(b & MPB_DOWN) ? MPK_UP : (b & MPB_DOWN) && !(b & MPB_UP) ? MPK_DOWN : MPK_NONE;
    if (dir != p->held_dir) { p->held_dir = dir; p->held_t = 0.f; p->next_rep = MENUPAD_REPEAT_DELAY; if (dir && !key) key = dir; }
    else if (dir) { p->held_t += dt; if (p->held_t >= p->next_rep) { p->next_rep += MENUPAD_REPEAT_EVERY; if (!key) key = dir; } }
    if (!key && (edge & (MPB_A | MPB_RIGHT))) key = MPK_SELECT;
    if (!key && (edge & (MPB_B | MPB_LEFT))) key = MPK_BACK;
    return key;
}
#endif
